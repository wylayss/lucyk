#include <cap.h>
#include <io.h>
#include <io/fbcon.h>
#include <io/pci.h>
#include <ioapic.h>
#include <lapic.h>
#include <idt.h>
#include <ipc.h>
#include <ipc.h>
#include <klog.h>
#include <mk_hooks.h>
#include <object.h>
#include <sched.h>
#include <string.h>
#include <syscall.h>
#include <syscall_numbers.h>
#include <display.h>
#include <pmm.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

#define USER_NULL_GUARD 0x1000ULL
#define USER_DEBUG_MAX  256U

static mk_hooks_t registered_hooks;
static bool hooks_registered;

void mk_register_hooks(const mk_hooks_t *hooks)
{
    if (hooks == NULL) {
        memset(&registered_hooks, 0, sizeof(registered_hooks));
        hooks_registered = false;
        return;
    }

    registered_hooks = *hooks;
    hooks_registered = true;
}

const mk_hooks_t *mk_hooks_get(void)
{
    return hooks_registered ? &registered_hooks : NULL;
}

static void dispatch_ipc_call(task_t *task, syscall_frame_t *frame)
{
    uint64_t msg[4] = { frame->rsi, frame->rdx, frame->r10, frame->r8 };
    uint64_t reply[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;

    sys_status_t status = ipc_call(task, (cap_t)frame->rdi, msg, reply, &badge);
    frame->rax = status;

    if (status == SYS_OK) {
        frame->rdx = badge;
        frame->rsi = reply[0];
        frame->r8 = reply[1];
        frame->r9 = reply[2];
        frame->r10 = reply[3];
    }
}

static void dispatch_ipc_recv(task_t *task, thread_t *current,
                              syscall_frame_t *frame)
{
    uint64_t msg[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;
    thread_t *sender = NULL;

    sys_status_t status = ipc_recv(task, (cap_t)frame->rdi, msg, &badge, &sender);
    frame->rax = status;

    if (status == SYS_OK) {
        current->ipc_sender = sender;
        frame->rdx = badge;
        frame->rsi = msg[0];
        frame->r8 = msg[1];
        frame->r9 = msg[2];
        frame->r10 = msg[3];
    }
}

static void dispatch_ipc_reply(thread_t *current, syscall_frame_t *frame)
{
    uint64_t reply[4] = { frame->rdi, frame->rsi, frame->rdx, frame->r10 };

    frame->rax = ipc_reply(current->ipc_sender, reply);
}

static void dispatch_frame_map(task_t *task, syscall_frame_t *frame)
{
    uint64_t vaddr = frame->rsi;
    uint64_t prot = frame->rdx & (VSPACE_PROT_WRITE | VSPACE_PROT_EXEC);

    if (vaddr < USER_NULL_GUARD || (vaddr & PAGE_MASK) != 0) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    uint64_t required = CAP_RIGHT_READ;
    if ((prot & VSPACE_PROT_WRITE) != 0) {
        required |= CAP_RIGHT_WRITE;
    }

    void *object = NULL;
    sys_status_t status = cap_lookup_retained(task->caps, (cap_t)frame->rdi,
                                              CAP_OBJ_FRAME, required,
                                              &object, NULL);
    if (status != SYS_OK) {
        frame->rax = status;
        return;
    }

    cap_frame_object_t *frame_object = (cap_frame_object_t *)object;

    bool is_device = frame_object->kind == CAP_FRAME_DEVICE;
    status = cap_frame_map(frame_object, task->vspace, vaddr, prot);
    mk_object_release(&frame_object->header);

    /* Handover: a Ring 3 caller taking the boot framebuffer stops kernel
     * framebuffer drawing atomically to avoid tearing. */
    if (status == SYS_OK && (cap_t)frame->rdi == DISPLAY_BOOT_CAP_FRAMEBUFFER &&
        is_device) {
        fbcon_release();
    }

    frame->rax = status;
}

_Static_assert(SYS_QOS_BACKGROUND == THREAD_QOS_BACKGROUND &&
                   SYS_QOS_DEFAULT == THREAD_QOS_DEFAULT &&
                   SYS_QOS_INTERACTIVE == THREAD_QOS_INTERACTIVE,
               "SYS_QOS_* must match THREAD_QOS_*");

/* SYS_THREAD_SET_QOS(tid, qos): tid 0 is the caller, otherwise a thread of
 * the calling task. Returns the base class in rdx and the effective class
 * (base or inherited, whichever is higher) in r8. */
static void dispatch_thread_set_qos(task_t *task, syscall_frame_t *frame)
{
    thread_t *target = task_find_thread(task, frame->rdi);

    if (target == NULL) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    if (frame->rsi != SYS_QOS_QUERY) {
        if (frame->rsi >= THREAD_QOS_COUNT) {
            frame->rax = SYS_ERR_INVAL;
            return;
        }

        sys_status_t status = thread_set_qos(target, (uint32_t)frame->rsi);
        if (status != SYS_OK) {
            frame->rax = status;
            return;
        }
    }

    frame->rax = SYS_OK;
    frame->rdx = __atomic_load_n(&target->base_qos, __ATOMIC_RELAXED);
    frame->r8 = __atomic_load_n(&target->effective_qos, __ATOMIC_RELAXED);
}

typedef struct grant_request {
    cap_table_t *source;
    cap_t        cap;
    uint64_t     rights;
    cap_t        installed;
} grant_request_t;

static sys_status_t grant_into_task(task_t *target, void *ctx)
{
    grant_request_t *request = (grant_request_t *)ctx;

    return cap_grant_rights(request->source, request->cap, target->caps,
                            request->rights, &request->installed);
}

/* SYS_CAP_GRANT(src, target_task_id, rights): copies src (which must carry
 * CAP_RIGHT_GRANT) into the target task's table with rights & src.rights.
 * target_task_id 0 duplicates within the caller. rdx returns the handle in
 * the target's table. */
static void dispatch_cap_grant(task_t *task, syscall_frame_t *frame)
{
    grant_request_t request = {
        .source = task->caps,
        .cap = (cap_t)frame->rdi,
        .rights = frame->rdx,
        .installed = CAP_NULL,
    };
    sys_status_t status;

    if (frame->rsi == SYS_CAP_GRANT_SELF || frame->rsi == task->id) {
        status = cap_grant_rights(task->caps, request.cap, task->caps,
                                  request.rights, &request.installed);
    } else {
        status = task_with_id(frame->rsi, grant_into_task, &request);
    }

    frame->rax = status;
    if (status == SYS_OK) {
        frame->rdx = (uint64_t)request.installed;
    }
}

static void install_new_object(task_t *task, cap_obj_type_t type,
                               mk_object_t *object, syscall_frame_t *frame)
{
    if (object == NULL) {
        frame->rax = SYS_ERR_NOMEM;
        return;
    }

    cap_t cap = CAP_NULL;
    sys_status_t status =
        cap_insert(task->caps, type, object, CAP_RIGHT_ALL, 0, &cap);

    /* The table holds its own reference; drop the creator's. */
    mk_object_release(object);

    frame->rax = status;
    if (status == SYS_OK) {
        frame->rdx = (uint64_t)cap;
    }
}

/* SYS_IRQ_BIND(irq, ntf, bit): both capabilities need CAP_RIGHT_WRITE. */
static void dispatch_irq_bind(task_t *task, syscall_frame_t *frame)
{
    void *irq_object = NULL;
    void *ntf_object = NULL;
    sys_status_t status = cap_lookup_retained(task->caps, (cap_t)frame->rdi,
                                              CAP_OBJ_IRQ, CAP_RIGHT_WRITE,
                                              &irq_object, NULL);
    if (status != SYS_OK) {
        frame->rax = status;
        return;
    }

    status = cap_lookup_retained(task->caps, (cap_t)frame->rsi,
                                 CAP_OBJ_NOTIFICATION, CAP_RIGHT_WRITE,
                                 &ntf_object, NULL);
    if (status == SYS_OK) {
        status = irq_cap_bind((cap_irq_object_t *)irq_object,
                              (notification_t *)ntf_object, frame->rdx);
        mk_object_release((mk_object_t *)ntf_object);
    }

    mk_object_release((mk_object_t *)irq_object);
    frame->rax = status;
}

static void dispatch_irq_ack(task_t *task, syscall_frame_t *frame)
{
    void *irq_object = NULL;
    sys_status_t status = cap_lookup_retained(task->caps, (cap_t)frame->rdi,
                                              CAP_OBJ_IRQ, CAP_RIGHT_WRITE,
                                              &irq_object, NULL);
    if (status == SYS_OK) {
        status = irq_cap_ack((cap_irq_object_t *)irq_object);
        mk_object_release((mk_object_t *)irq_object);
    }
    frame->rax = status;
}

/* SYS_IRQ_CREATE(hw_cap, rights): privileged. hw_cap must name a
 * CAP_OBJ_HARDWARE capability with CAP_RIGHT_WRITE (SYS_ERR_CAP_INVALID /
 * SYS_ERR_CAP_RIGHTS otherwise). rights must have no bits outside
 * CAP_RIGHT_ALL (SYS_ERR_INVAL). Mints a fresh unbound CAP_OBJ_IRQ
 * (cap_irq_object_create(0)) with (CAP_RIGHT_ALL & rights), installs it in
 * the caller's capability table, returns the new handle in rdx. */
static void dispatch_irq_create(task_t *task, syscall_frame_t *frame)
{
    uint64_t rights = frame->rsi;

    void *hw = NULL;
    sys_status_t status = cap_lookup_retained(task->caps, (cap_t)frame->rdi,
                                              CAP_OBJ_HARDWARE, CAP_RIGHT_WRITE,
                                              &hw, NULL);
    if (status != SYS_OK) {
        frame->rax = status;
        return;
    }
    mk_object_release((mk_object_t *)hw);

    if ((rights & ~CAP_RIGHT_ALL) != 0) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    mk_object_t *object = cap_irq_object_create(0);
    if (object == NULL) {
        frame->rax = SYS_ERR_NOMEM;
        return;
    }

    cap_t cap = CAP_NULL;
    status = cap_insert(task->caps, CAP_OBJ_IRQ, object,
                        CAP_RIGHT_ALL & rights, 0, &cap);

    /* The table holds its own reference; drop the creator's. */
    mk_object_release(object);

    frame->rax = status;
    if (status == SYS_OK) {
        frame->rdx = (uint64_t)cap;
    }
}

static void dispatch_range_unmap(task_t *task, syscall_frame_t *frame)
{
    uint64_t vaddr = frame->rdi;
    uint64_t size = frame->rsi;

    if (size == 0 || (vaddr & PAGE_MASK) != 0 ||
        vaddr < USER_NULL_GUARD || vaddr > 0x00007FFFFFFFFFFFULL) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }
    if (size > 0x00007FFFFFFFFFFFULL - vaddr + 1ULL) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    sys_status_t status = SYS_OK;
    uint64_t end = vaddr + size;

    for (uint64_t addr = vaddr; addr < end; addr += PAGE_SIZE) {
        status = vspace_unmap(task->vspace, addr);
        if (status != SYS_OK) {
            break;
        }
    }

    frame->rax = status;
}

/* SYS_DMA_ALLOC(page_count, rights): contiguous PMM pages as CAP_FRAME_DMA,
 * zeroed, page-capped, never executable. Handle in rdx. */
static void dispatch_dma_alloc(task_t *task, syscall_frame_t *frame)
{
    uint64_t page_count = frame->rdi;
    uint64_t rights = frame->rsi;

    if (page_count == 0 || page_count > SYS_DMA_ALLOC_MAX_PAGES ||
        (rights & ~CAP_RIGHT_ALL) != 0 ||
        (rights & (CAP_RIGHT_READ | CAP_RIGHT_WRITE)) == 0) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    uint64_t phys = pmm_alloc_contiguous((size_t)page_count);
    if (phys == 0) {
        frame->rax = SYS_ERR_NOMEM;
        return;
    }

    mk_object_t *object = cap_dma_frame_object_create(phys, (size_t)page_count);
    if (object == NULL) {
        pmm_free_contiguous((uintptr_t)phys, (size_t)page_count);
        frame->rax = SYS_ERR_NOMEM;
        return;
    }

    cap_t cap = CAP_NULL;
    sys_status_t status = cap_insert(task->caps, CAP_OBJ_FRAME, object,
                                     rights & CAP_RIGHT_ALL, 0, &cap);
    mk_object_release(object);

    if (status != SYS_OK) {
        frame->rax = status;
        return;
    }

    frame->rax = SYS_OK;
    frame->rdx = (uint64_t)cap;
}

/* SYS_FRAME_PHYS_ADDR(frame_cap): physical base only for CAP_FRAME_DMA. */
static void dispatch_frame_phys_addr(task_t *task, syscall_frame_t *frame)
{
    void *object = NULL;
    sys_status_t status = cap_lookup_retained(task->caps, (cap_t)frame->rdi,
                                              CAP_OBJ_FRAME, CAP_RIGHT_READ,
                                              &object, NULL);
    if (status != SYS_OK) {
        frame->rax = status;
        return;
    }

    cap_frame_object_t *frame_object = (cap_frame_object_t *)object;

    if (frame_object->kind != CAP_FRAME_DMA) {
        mk_object_release(&frame_object->header);
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    frame->rax = SYS_OK;
    frame->rdx = (uint64_t)frame_object->phys;
    mk_object_release(&frame_object->header);
}

/* SYS_PCI_CLAIM_BAR(bdf, bar_index): privileged. Validates MMIO BAR, marks
 * claimed, enables memory+bus-master, returns CAP_FRAME_DEVICE (PAGE-rounded)
 * in rdx and sub-page byte offset in r8. bdf = (bus<<16)|(slot<<8)|func. */
static void dispatch_pci_claim_bar(task_t *task, syscall_frame_t *frame)
{
    /* Privileged hardware manager: hold a CAP_OBJ_HARDWARE authority. */
    void *hw = NULL;
    if (task == NULL ||
        cap_lookup_retained(task->caps, 1U, CAP_OBJ_HARDWARE, CAP_RIGHT_WRITE,
                            &hw, NULL) != SYS_OK) {
        /* No privileged hardware capability in slot 1. */
        bool allowed = false;
        for (cap_t slot = 1; slot <= CAP_TABLE_SLOTS; slot++) {
            if (cap_lookup_retained(task->caps, slot, CAP_OBJ_HARDWARE,
                                    CAP_RIGHT_WRITE, &hw, NULL) == SYS_OK) {
                mk_object_release((mk_object_t *)hw);
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            frame->rax = SYS_ERR_CAP_RIGHTS;
            return;
        }
    } else {
        mk_object_release((mk_object_t *)hw);
    }

    uint64_t bdf = frame->rdi;
    uint64_t bar_index = frame->rsi;
    uint32_t bus = (uint32_t)((bdf >> 16) & 0xFFU);
    uint32_t slot = (uint32_t)((bdf >> 8) & 0xFFU);
    uint32_t func = (uint32_t)(bdf & 0xFFU);

    if (bar_index >= PCI_BAR_COUNT) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    /* Locate the enumerated function. */
    pci_device_t dev;
    bool found = false;
    size_t n = pci_device_count();
    for (size_t i = 0; i < n; i++) {
        if (pci_device_snapshot(i, &dev) && dev.bus == bus &&
            dev.slot == slot && dev.func == func) {
            found = true;
            break;
        }
    }

    if (!found || dev.bar_io[bar_index]) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    /* Reject I/O-space BARs (bit0==1 handled by bar_io), a reserved second
     * half of a 64-bit BAR, and claiming bar_index+1 independently. */
    if (dev.bar64[bar_index] == true && bar_index >= PCI_BAR_COUNT) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }
    if (bar_index + 1U < PCI_BAR_COUNT && dev.bar64[bar_index + 1U]) {
        /* bar_index+1 is the reserved high dword of a 64-bit BAR at
         * bar_index; it cannot be claimed on its own. */
    }

    uint64_t bar = dev.bar[bar_index];
    uint64_t bar_size = dev.bar_size[bar_index];

    if (bar == 0 || bar_size == 0) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    if (pci_is_kernel_claimed((uint8_t)bus, (uint8_t)slot, (uint8_t)func)) {
        frame->rax = SYS_ERR_BUSY;
        return;
    }

    if (pci_claim_bar((uint8_t)bus, (uint8_t)slot, (uint8_t)func,
                      (uint32_t)bar_index) != SYS_OK) {
        frame->rax = SYS_ERR_BUSY;
        return;
    }

    /* Enable memory space + bus mastering. */
    pci_enable_bus_master(&dev);

    uint64_t phys = bar;
    uint64_t offset = phys & 0xFFFULL;
    uint64_t base = phys & ~0xFFFULL;
    uint64_t span = ((offset + bar_size) + 0xFFFULL) & ~0xFFFULL;

    mk_object_t *object = cap_device_frame_object_create(
        (paddr_t)base, (size_t)(span / 4096ULL), false);
    if (object == NULL) {
        pci_release_bar((uint8_t)bus, (uint8_t)slot, (uint8_t)func, (uint32_t)bar_index);
        frame->rax = SYS_ERR_NOMEM;
        return;
    }

    cap_t cap = CAP_NULL;
    /* `CAP_RIGHT_GRANT` lets the hardware manager pass the BAR to the driver. */
    sys_status_t status = cap_insert(task->caps, CAP_OBJ_FRAME, object,
                                     CAP_RIGHT_READ | CAP_RIGHT_WRITE |
                                         CAP_RIGHT_GRANT,
                                     0, &cap);
    mk_object_release(object);

    if (status != SYS_OK) {
        pci_release_bar((uint8_t)bus, (uint8_t)slot, (uint8_t)func, (uint32_t)bar_index);
        frame->rax = status;
        return;
    }

    frame->rax = SYS_OK;
    frame->rdx = (uint64_t)cap;
    frame->r8 = offset;
}

/* SYS_PCI_ALLOC_MSI(bdf, irq_cap): privileged. Allocates a dynamic LAPIC
 * vector 0x50..0x6F, programs PCI MSI/MSI-X to the BSP, binds the vector to
 * irq_cap (CAP_OBJ_IRQ). */
static void dispatch_pci_alloc_msi(task_t *task, syscall_frame_t *frame)
{
    /* Privileged hardware manager. */
    void *hw = NULL;
    bool allowed = false;
    if (task != NULL) {
        for (cap_t slot = 1; slot <= CAP_TABLE_SLOTS; slot++) {
            if (cap_lookup_retained(task->caps, slot, CAP_OBJ_HARDWARE,
                                    CAP_RIGHT_WRITE, &hw, NULL) == SYS_OK) {
                mk_object_release((mk_object_t *)hw);
                allowed = true;
                break;
            }
        }
    }
    if (!allowed) {
        frame->rax = SYS_ERR_CAP_RIGHTS;
        return;
    }

    uint64_t bdf = frame->rdi;
    uint32_t bus = (uint32_t)((bdf >> 16) & 0xFFU);
    uint32_t slot = (uint32_t)((bdf >> 8) & 0xFFU);
    uint32_t func = (uint32_t)(bdf & 0xFFU);

    void *irq_object = NULL;
    sys_status_t status = cap_lookup_retained(task->caps, (cap_t)frame->rsi,
                                              CAP_OBJ_IRQ, CAP_RIGHT_WRITE,
                                              &irq_object, NULL);
    if (status != SYS_OK) {
        frame->rax = status;
        return;
    }

    cap_irq_object_t *irq = (cap_irq_object_t *)irq_object;

    uint8_t vector = ioapic_alloc_msi_vector();
    if (vector == 0) {
        mk_object_release(irq_object);
        frame->rax = SYS_ERR_NOMEM;
        return;
    }

    /* Refuse to reprogram a Ring 0 kernel storage controller. */
    if (pci_is_kernel_claimed((uint8_t)bus, (uint8_t)slot, (uint8_t)func)) {
        ioapic_free_msi_vector(vector);
        mk_object_release(irq_object);
        frame->rax = SYS_ERR_BUSY;
        return;
    }

    uint32_t apic = lapic_id();
    status = pci_msi_configure((uint8_t)bus, (uint8_t)slot, (uint8_t)func,
                               vector, apic);
    if (status != SYS_OK) {
        ioapic_free_msi_vector(vector);
        mk_object_release(irq_object);
        frame->rax = status;
        return;
    }

    irq->vector = vector;
    irq->gsi_backed = false;
    irq->msi = true;
    irq->msi_bdf = ((uint32_t)bus << 16) | ((uint32_t)slot << 8) | func;

    /* Ensure memory + bus master. */
    pci_device_t dev;
    for (size_t i = 0; i < pci_device_count(); i++) {
        if (pci_device_snapshot(i, &dev) && dev.bus == bus &&
            dev.slot == slot && dev.func == func) {
            pci_enable_bus_master(&dev);
            break;
        }
    }

    mk_object_release(irq_object);
    frame->rax = SYS_OK;
    frame->rdx = vector;
}

static void dispatch_debug_log(syscall_frame_t *frame)
{
    size_t len = (size_t)frame->rsi;

    if (len > USER_DEBUG_MAX) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    char buffer[USER_DEBUG_MAX + 1];
    sys_status_t status = copy_from_user(buffer, (const void *)(uintptr_t)frame->rdi,
                                         len);
    if (status != SYS_OK) {
        frame->rax = status;
        return;
    }

    buffer[len] = '\0';
    klog(KLOG_INFO, "user log: %s", buffer);
    frame->rax = SYS_OK;
}

void syscall_dispatch(syscall_frame_t *frame)
{
    thread_t *current = sched_current_thread();

    if (current == NULL || current->owner_task == NULL) {
        frame->rax = SYS_ERR_DEAD;
        return;
    }

    task_t *task = current->owner_task;
    uint64_t number = frame->rax;

    if ((number & (1ULL << 63)) == 0) {
        const mk_hooks_t *hooks = mk_hooks_get();

        if (hooks != NULL && hooks->on_personality_syscall != NULL) {
            hooks->on_personality_syscall(current, frame);
        } else {
            frame->rax = SYS_ERR_NOSYS;
        }
        return;
    }

    switch (number) {
    case SYS_THREAD_EXIT:
        current->exit_code = (int64_t)frame->rdi;
        thread_exit();
        break;

    case SYS_THREAD_YIELD:
        thread_yield();
        frame->rax = SYS_OK;
        break;

    case SYS_THREAD_SLEEP_MS:
        thread_sleep_ms(frame->rdi);
        frame->rax = SYS_OK;
        break;

    case SYS_THREAD_SET_QOS:
        dispatch_thread_set_qos(task, frame);
        break;

    case SYS_TASK_ID:
        /* The caller's task id (the SYS_CAP_GRANT target selector) in rdx
         * and its thread id in r8. */
        frame->rax = SYS_OK;
        frame->rdx = task->id;
        frame->r8 = current->tid;
        break;

    case SYS_ENDPOINT_CREATE: {
        endpoint_t *endpoint = endpoint_create();

        install_new_object(task, CAP_OBJ_ENDPOINT,
                           endpoint != NULL ? &endpoint->header : NULL, frame);
        break;
    }

    case SYS_NOTIFY_CREATE: {
        notification_t *notification = notification_create();

        install_new_object(task, CAP_OBJ_NOTIFICATION,
                           notification != NULL ? &notification->header : NULL,
                           frame);
        break;
    }

    case SYS_IRQ_BIND:
        dispatch_irq_bind(task, frame);
        break;

    case SYS_IRQ_ACK:
        dispatch_irq_ack(task, frame);
        break;

    case SYS_IRQ_CREATE:
        dispatch_irq_create(task, frame);
        break;

    case SYS_IPC_CALL:
        dispatch_ipc_call(task, frame);
        break;

    case SYS_IPC_RECV:
        dispatch_ipc_recv(task, current, frame);
        break;

    case SYS_IPC_REPLY:
        dispatch_ipc_reply(current, frame);
        break;

    case SYS_NOTIFY_SIGNAL: {
        void *object = NULL;
        sys_status_t status = cap_lookup_retained(task->caps, (cap_t)frame->rdi,
                                                   CAP_OBJ_NOTIFICATION,
                                                   CAP_RIGHT_WRITE, &object,
                                                   NULL);
        if (status == SYS_OK) {
            notification_t *notification = (notification_t *)object;
            notify_signal(notification, frame->rsi);
            mk_object_release(&notification->header);
        }
        frame->rax = status;
        break;
    }

    case SYS_NOTIFY_WAIT: {
        uint64_t bits = 0;
        sys_status_t status = notify_wait(task, (cap_t)frame->rdi, &bits);
        frame->rax = status;
        if (status == SYS_OK) {
            frame->rdx = bits;
        }
        break;
    }

    case SYS_CAP_DERIVE: {
        cap_t new_cap = CAP_NULL;
        sys_status_t status = cap_derive(task->caps, (cap_t)frame->rdi,
                                          frame->rsi, frame->rdx, &new_cap);
        frame->rax = status;
        if (status == SYS_OK) {
            frame->rdx = (uint64_t)new_cap;
        }
        break;
    }

    case SYS_CAP_GRANT:
        dispatch_cap_grant(task, frame);
        break;

    case SYS_CAP_DELETE:
        frame->rax = cap_delete(task->caps, (cap_t)frame->rdi);
        break;

    case SYS_VSPACE_MAP:
        dispatch_frame_map(task, frame);
        break;

    case SYS_VSPACE_UNMAP:
        dispatch_range_unmap(task, frame);
        break;

    case SYS_DEBUG_LOG:
        dispatch_debug_log(frame);
        break;

    case SYS_DMA_ALLOC:
        dispatch_dma_alloc(task, frame);
        break;

    case SYS_FRAME_PHYS_ADDR:
        dispatch_frame_phys_addr(task, frame);
        break;

    case SYS_FBCON_RELEASE:
        fbcon_release();
        frame->rax = SYS_OK;
        break;

    case SYS_PCI_CLAIM_BAR:
        dispatch_pci_claim_bar(task, frame);
        break;

    case SYS_PCI_ALLOC_MSI:
        dispatch_pci_alloc_msi(task, frame);
        break;

    default:
        frame->rax = SYS_ERR_NOSYS;
        break;
    }
}
