#include <cap.h>
#include <io.h>
#include <kalloc.h>
#include <klog.h>
#include <object.h>
#include <pmm.h>
#include <spinlock.h>
#include <string.h>
#include <types.h>
#include <vspace.h>

/* Serialises every capability-table mutation and lookup. Tables of different
 * tasks can be touched from different CPUs (a cross-task SYS_CAP_GRANT writes
 * into the target's table), so masking local interrupts is not enough. Lock
 * order: task registry -> cap. Object releases always happen after this lock
 * is dropped, because a release may wake threads or free page tables. */
static spinlock_t cap_lock;

#define USER_TOP_ADDR 0x00007FFFFFFFFFFFULL

typedef struct cap_boot_entry {
    cap_obj_type_t type;
    mk_object_t   *object;
    uint64_t       rights;
} cap_boot_entry_t;

static cap_boot_entry_t boot_caps[CAP_BOOT_MAX];
static uint32_t boot_cap_count;

static bool slot_is_empty(const cap_slot_t *slot)
{
    return slot->object == NULL;
}

static int find_free_slot(const cap_table_t *table)
{
    for (size_t i = 0; i < CAP_TABLE_SLOTS; i++) {
        if (slot_is_empty(&table->slots[i])) {
            return (int)i;
        }
    }
    return -1;
}

static bool cap_handle_valid(cap_t cap)
{
    return cap != CAP_NULL && cap <= CAP_TABLE_SLOTS;
}

static cap_slot_t *slot_for(cap_table_t *table, cap_t cap)
{
    return &table->slots[cap - 1U];
}

cap_table_t *cap_table_create(void)
{
    cap_table_t *table = (cap_table_t *)kalloc(sizeof(cap_table_t));
    if (table == NULL) {
        return NULL;
    }

    memset(table, 0, sizeof(*table));
    return table;
}

void cap_table_destroy(cap_table_t *table)
{
    if (table == NULL) {
        return;
    }

    for (size_t i = 0; i < CAP_TABLE_SLOTS; i++) {
        uint64_t flags = spin_lock_irqsave(&cap_lock);
        cap_slot_t *slot = &table->slots[i];
        mk_object_t *object = slot->object;

        if (object != NULL) {
            slot->object = NULL;
            slot->type = CAP_OBJ_NONE;
            slot->rights = 0;
            slot->badge = 0;
            if (table->used > 0) {
                table->used--;
            }
        }
        spin_unlock_irqrestore(&cap_lock, flags);

        if (object != NULL) {
            mk_object_release(object);
        }
    }

    kfree(table);
}

sys_status_t cap_insert(cap_table_t *table, cap_obj_type_t type, void *obj,
                        uint64_t rights, uint64_t badge, cap_t *cap_out)
{
    if (table == NULL || obj == NULL || cap_out == NULL) {
        return SYS_ERR_INVAL;
    }
    if (type == CAP_OBJ_NONE || (rights & ~CAP_RIGHT_ALL) != 0) {
        return SYS_ERR_INVAL;
    }

    mk_object_t *object = (mk_object_t *)obj;
    if (object->type != (uint32_t)type) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&cap_lock);

    int index = find_free_slot(table);
    if (index < 0) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_NOMEM;
    }

    mk_object_retain(object);

    cap_slot_t *slot = &table->slots[index];
    slot->object = object;
    slot->type = type;
    slot->rights = rights;
    slot->badge = badge;
    table->used++;

    *cap_out = (cap_t)(index + 1);

    spin_unlock_irqrestore(&cap_lock, flags);
    return SYS_OK;
}

static sys_status_t lookup_common(cap_table_t *table, cap_t cap,
                                  cap_obj_type_t expected_type,
                                  uint64_t required_rights, void **obj_out,
                                  uint64_t *badge_out, bool retain)
{
    if (table == NULL || obj_out == NULL) {
        return SYS_ERR_INVAL;
    }
    if (!cap_handle_valid(cap)) {
        return SYS_ERR_CAP_INVALID;
    }

    uint64_t flags = spin_lock_irqsave(&cap_lock);
    cap_slot_t *slot = slot_for(table, cap);

    if (slot_is_empty(slot) || slot->type != expected_type) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_CAP_INVALID;
    }
    if ((slot->rights & required_rights) != required_rights) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_CAP_RIGHTS;
    }

    if (retain) {
        mk_object_retain(slot->object);
    }

    *obj_out = slot->object;
    if (badge_out != NULL) {
        *badge_out = slot->badge;
    }

    spin_unlock_irqrestore(&cap_lock, flags);
    return SYS_OK;
}

sys_status_t cap_lookup(cap_table_t *table, cap_t cap, cap_obj_type_t expected_type,
                        uint64_t required_rights, void **obj_out, uint64_t *badge_out)
{
    return lookup_common(table, cap, expected_type, required_rights, obj_out,
                         badge_out, false);
}

sys_status_t cap_lookup_retained(cap_table_t *table, cap_t cap,
                                 cap_obj_type_t expected_type,
                                 uint64_t required_rights, void **obj_out,
                                 uint64_t *badge_out)
{
    return lookup_common(table, cap, expected_type, required_rights, obj_out,
                         badge_out, true);
}

sys_status_t cap_derive(cap_table_t *table, cap_t src, uint64_t new_rights,
                        uint64_t badge, cap_t *dst_out)
{
    if (table == NULL || dst_out == NULL) {
        return SYS_ERR_INVAL;
    }
    if (!cap_handle_valid(src)) {
        return SYS_ERR_CAP_INVALID;
    }

    uint64_t flags = spin_lock_irqsave(&cap_lock);

    cap_slot_t *source = slot_for(table, src);
    if (slot_is_empty(source)) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_CAP_INVALID;
    }

    int index = find_free_slot(table);
    if (index < 0) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_NOMEM;
    }

    mk_object_retain(source->object);

    cap_slot_t *slot = &table->slots[index];
    slot->object = source->object;
    slot->type = source->type;
    slot->rights = new_rights & source->rights & CAP_RIGHT_ALL;
    slot->badge = badge;
    table->used++;

    *dst_out = (cap_t)(index + 1);

    spin_unlock_irqrestore(&cap_lock, flags);
    return SYS_OK;
}

sys_status_t cap_grant_rights(cap_table_t *src_table, cap_t src,
                              cap_table_t *dst_table, uint64_t rights,
                              cap_t *dst_out)
{
    if (src_table == NULL || dst_table == NULL || dst_out == NULL) {
        return SYS_ERR_INVAL;
    }
    if (!cap_handle_valid(src)) {
        return SYS_ERR_CAP_INVALID;
    }
    if ((rights & ~CAP_RIGHT_ALL) != 0) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&cap_lock);

    cap_slot_t *source = slot_for(src_table, src);
    if (slot_is_empty(source)) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_CAP_INVALID;
    }
    if ((source->rights & CAP_RIGHT_GRANT) == 0) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_CAP_RIGHTS;
    }

    int index = find_free_slot(dst_table);
    if (index < 0) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_NOMEM;
    }

    mk_object_retain(source->object);

    cap_slot_t *slot = &dst_table->slots[index];
    slot->object = source->object;
    slot->type = source->type;
    slot->rights = source->rights & rights;
    slot->badge = source->badge;
    dst_table->used++;

    *dst_out = (cap_t)(index + 1);

    spin_unlock_irqrestore(&cap_lock, flags);
    return SYS_OK;
}

sys_status_t cap_grant(cap_table_t *src_table, cap_t src, cap_table_t *dst_table,
                       cap_t *dst_out)
{
    return cap_grant_rights(src_table, src, dst_table, CAP_RIGHT_ALL, dst_out);
}

sys_status_t cap_delete(cap_table_t *table, cap_t cap)
{
    if (table == NULL || !cap_handle_valid(cap)) {
        return SYS_ERR_CAP_INVALID;
    }

    uint64_t flags = spin_lock_irqsave(&cap_lock);
    cap_slot_t *slot = slot_for(table, cap);

    if (slot_is_empty(slot)) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_CAP_INVALID;
    }

    mk_object_t *object = slot->object;

    slot->object = NULL;
    slot->type = CAP_OBJ_NONE;
    slot->rights = 0;
    slot->badge = 0;
    if (table->used > 0) {
        table->used--;
    }

    spin_unlock_irqrestore(&cap_lock, flags);

    mk_object_release(object);
    return SYS_OK;
}

static void vspace_object_release(mk_object_t *object)
{
    cap_vspace_object_t *wrapper = (cap_vspace_object_t *)object;

    if (wrapper->vspace != NULL) {
        vspace_destroy(wrapper->vspace);
    }
    kfree(wrapper);
}

mk_object_t *cap_vspace_object_create(vspace_t *vspace)
{
    if (vspace == NULL) {
        return NULL;
    }

    cap_vspace_object_t *wrapper =
        (cap_vspace_object_t *)kalloc(sizeof(cap_vspace_object_t));

    if (wrapper == NULL) {
        return NULL;
    }

    memset(wrapper, 0, sizeof(*wrapper));
    vspace_ref(vspace);
    wrapper->vspace = vspace;
    mk_object_init(&wrapper->header, CAP_OBJ_VSPACE, vspace_object_release);

    return &wrapper->header;
}

static void frame_object_release(mk_object_t *object)
{
    cap_frame_object_t *wrapper = (cap_frame_object_t *)object;

    switch (wrapper->kind) {
    case CAP_FRAME_ANON:
        if (!wrapper->transferred && wrapper->phys != 0) {
            pmm_free_frame(wrapper->phys);
        }
        break;
    case CAP_FRAME_SHARED:
        /* Drops the object's own reference; live mappings hold theirs. */
        if (wrapper->phys != 0) {
            pmm_frame_unref(wrapper->phys);
        }
        break;
    case CAP_FRAME_DEVICE:
        /* Hardware memory: never returned to the PMM. */
        break;
    }
    kfree(wrapper);
}

static mk_object_t *frame_object_alloc(paddr_t phys, cap_frame_kind_t kind,
                                       size_t page_count, bool wc)
{
    cap_frame_object_t *wrapper =
        (cap_frame_object_t *)kalloc(sizeof(cap_frame_object_t));

    if (wrapper == NULL) {
        return NULL;
    }

    memset(wrapper, 0, sizeof(*wrapper));
    wrapper->phys = phys;
    wrapper->kind = kind;
    wrapper->page_count = page_count;
    wrapper->write_combining = wc;
    mk_object_init(&wrapper->header, CAP_OBJ_FRAME, frame_object_release);

    return &wrapper->header;
}

mk_object_t *cap_frame_object_create(paddr_t phys)
{
    return frame_object_alloc(phys, CAP_FRAME_ANON, 1, false);
}

mk_object_t *cap_shared_frame_object_create(paddr_t phys)
{
    if (phys == 0 || (phys & PAGE_MASK) != 0) {
        return NULL;
    }
    return frame_object_alloc(phys, CAP_FRAME_SHARED, 1, false);
}

mk_object_t *cap_device_frame_object_create(paddr_t phys, size_t page_count,
                                            bool write_combining)
{
    if ((phys & PAGE_MASK) != 0 || page_count == 0) {
        return NULL;
    }
    if (page_count > (size_t)((UINT64_MAX - phys) / PAGE_SIZE)) {
        return NULL;
    }
    return frame_object_alloc(phys, CAP_FRAME_DEVICE, page_count,
                              write_combining);
}

static sys_status_t frame_map_anon(cap_frame_object_t *frame, vspace_t *vs,
                                   vaddr_t vaddr, uint64_t prot)
{
    /* Transfer frame ownership to the vspace. This prevents a later cap delete
     * from freeing a frame that remains mapped, and prevents a second map of
     * the same frame capability from claiming the same physical frame twice. */
    uint64_t flags = spin_lock_irqsave(&cap_lock);
    if (frame->transferred) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_BUSY;
    }
    frame->transferred = true;
    spin_unlock_irqrestore(&cap_lock, flags);

    sys_status_t status = vspace_map_user(vs, vaddr, frame->phys,
                                          prot | VSPACE_PROT_USER |
                                              VSPACE_PROT_OWNED);
    if (status != SYS_OK) {
        flags = spin_lock_irqsave(&cap_lock);
        frame->transferred = false;
        spin_unlock_irqrestore(&cap_lock, flags);
    }
    return status;
}

static sys_status_t frame_map_shared(cap_frame_object_t *frame, vspace_t *vs,
                                     vaddr_t vaddr, uint64_t prot)
{
    /* The mapping owns its own reference, so unmapping it or tearing down the
     * vspace never frees the frame out from under the capability. */
    sys_status_t status = pmm_frame_ref(frame->phys);
    if (status != SYS_OK) {
        return status;
    }

    status = vspace_map_user(vs, vaddr, frame->phys,
                             prot | VSPACE_PROT_USER | VSPACE_PROT_OWNED);
    if (status != SYS_OK) {
        pmm_frame_unref(frame->phys);
    }
    return status;
}

static sys_status_t frame_map_device(cap_frame_object_t *frame, vspace_t *vs,
                                     vaddr_t vaddr, uint64_t prot)
{
    if ((prot & VSPACE_PROT_EXEC) != 0) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = prot | VSPACE_PROT_USER | VSPACE_PROT_DEVICE;

    if (frame->write_combining) {
        flags |= VSPACE_PROT_WC;
    } else {
        flags |= VSPACE_PROT_PCD | VSPACE_PROT_PWT;
    }

    for (size_t page = 0; page < frame->page_count; page++) {
        vaddr_t va = vaddr + (vaddr_t)page * PAGE_SIZE;
        sys_status_t status =
            vspace_map_user(vs, va, frame->phys + (paddr_t)page * PAGE_SIZE,
                            flags);

        if (status != SYS_OK) {
            for (size_t undo = 0; undo < page; undo++) {
                (void)vspace_unmap(vs, vaddr + (vaddr_t)undo * PAGE_SIZE);
            }
            return status;
        }
    }

    return SYS_OK;
}

sys_status_t cap_frame_map(cap_frame_object_t *frame, vspace_t *vs,
                           vaddr_t vaddr, uint64_t prot)
{
    if (frame == NULL || vs == NULL || (vaddr & PAGE_MASK) != 0 ||
        frame->page_count == 0) {
        return SYS_ERR_INVAL;
    }
    if ((prot & ~(uint64_t)(VSPACE_PROT_WRITE | VSPACE_PROT_EXEC)) != 0) {
        return SYS_ERR_INVAL;
    }
    if (vaddr > USER_TOP_ADDR ||
        frame->page_count > (size_t)((USER_TOP_ADDR - vaddr) / PAGE_SIZE) + 1U) {
        return SYS_ERR_INVAL;
    }

    switch (frame->kind) {
    case CAP_FRAME_ANON:
        return frame_map_anon(frame, vs, vaddr, prot);
    case CAP_FRAME_SHARED:
        return frame_map_shared(frame, vs, vaddr, prot);
    case CAP_FRAME_DEVICE:
        return frame_map_device(frame, vs, vaddr, prot);
    }

    return SYS_ERR_INVAL;
}

sys_status_t cap_boot_register(cap_obj_type_t type, mk_object_t *object,
                               uint64_t rights, cap_t *handle_out)
{
    if (object == NULL || type == CAP_OBJ_NONE ||
        object->type != (uint32_t)type || (rights & ~CAP_RIGHT_ALL) != 0) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&cap_lock);

    if (boot_cap_count >= CAP_BOOT_MAX) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_NOMEM;
    }

    mk_object_retain(object);
    boot_caps[boot_cap_count].type = type;
    boot_caps[boot_cap_count].object = object;
    boot_caps[boot_cap_count].rights = rights;
    boot_cap_count++;

    if (handle_out != NULL) {
        *handle_out = (cap_t)boot_cap_count;
    }

    spin_unlock_irqrestore(&cap_lock, flags);
    return SYS_OK;
}

sys_status_t cap_boot_install(cap_table_t *table, uint32_t *installed_out)
{
    if (table == NULL) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&cap_lock);

    if (table->used != 0 || boot_cap_count > CAP_TABLE_SLOTS) {
        spin_unlock_irqrestore(&cap_lock, flags);
        return SYS_ERR_BUSY;
    }

    for (uint32_t i = 0; i < boot_cap_count; i++) {
        cap_slot_t *slot = &table->slots[i];

        mk_object_retain(boot_caps[i].object);
        slot->object = boot_caps[i].object;
        slot->type = boot_caps[i].type;
        slot->rights = boot_caps[i].rights;
        slot->badge = 0;
        table->used++;
    }

    if (installed_out != NULL) {
        *installed_out = boot_cap_count;
    }

    spin_unlock_irqrestore(&cap_lock, flags);
    return SYS_OK;
}
