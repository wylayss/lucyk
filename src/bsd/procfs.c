#include <bsd/bsd.h>
#include <bsd/errno.h>
#include <bsd/proc.h>
#include <bsd/vfs.h>
#include <cpu.h>
#include <elf.h>
#include <io/pci.h>
#include <kalloc.h>
#include <pmm.h>
#include <smp.h>
#include <string.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

/* Linux-style introspection trees, generated on every read:
 *
 *   /proc/meminfo             PMM frame accounting
 *   /proc/cpuinfo             online CPUs, vendor string, APIC ids
 *   /proc/self                the calling process's /proc/<pid>
 *   /proc/<pid>/status        process, task and QoS summary
 *   /proc/<pid>/maps          live user mappings with rwxp flags
 *   /sys/bus/pci/devices      enumerated PCI functions (read-only helpers)
 *
 * Directory and file vnodes for a pid are allocated per lookup and freed when
 * their last reference goes; every other node is permanent. Files are regular
 * and read-only; their size reads as 0, as on Linux, and read() regenerates the
 * text and returns the requested window of it. */

#define PROCFS_TEXT_MAX (32U * 1024U)
#define PROCFS_MAX_RANGES 128U

#define PROCFS_INO_BASE 0x00500000ULL

typedef enum procfs_kind {
    PFS_PROC_ROOT = 0,
    PFS_MEMINFO,
    PFS_CPUINFO,
    PFS_PID_DIR,
    PFS_PID_STATUS,
    PFS_PID_MAPS,
    PFS_SYS_ROOT,
    PFS_SYS_BUS,
    PFS_SYS_PCI,
    PFS_SYS_PCI_DEVICES
} procfs_kind_t;

typedef struct procfs_node {
    procfs_kind_t kind;
    int32_t       pid;
    bool          dynamic;
} procfs_node_t;

typedef struct procfs_dynamic {
    vnode_t       vnode;
    procfs_node_t node;
    char          name[16];
} procfs_dynamic_t;

static sys_status_t procfs_read(vnode_t *vnode, void *buf, size_t len,
                                uint64_t offset, size_t *done);
static sys_status_t procfs_lookup(vnode_t *dir, const char *name,
                                  vnode_t **out);
static sys_status_t procfs_readdir(vnode_t *dir, uint64_t index,
                                   bsd_dirent_t *entry);
static void procfs_release(vnode_t *vnode);

static vnode_ops_t procfs_ops = {
    .read = procfs_read,
    .lookup = procfs_lookup,
    .readdir = procfs_readdir,
    .release = procfs_release,
};

typedef struct procfs_static {
    vnode_t       vnode;
    procfs_node_t node;
} procfs_static_t;

#define PROCFS_STATIC(kind_, type_, name_)                                  \
    {                                                                       \
        .vnode = {                                                          \
            .type = (type_),                                                \
            .mode = ((type_) == VNODE_DIR) ? (BSD_S_IFDIR | 0555U)          \
                                           : (BSD_S_IFREG | 0444U),         \
            .size = 0,                                                      \
            .ino = PROCFS_INO_BASE + (uint64_t)(kind_),                     \
            .ref_count = 1,                                                 \
            .ops = &procfs_ops,                                             \
            .fs_data = NULL,                                                \
            .parent = NULL,                                                 \
            .name = (name_),                                                \
        },                                                                  \
        .node = { .kind = (kind_), .pid = 0, .dynamic = false },            \
    }

static procfs_static_t proc_root = PROCFS_STATIC(PFS_PROC_ROOT, VNODE_DIR, "proc");
static procfs_static_t proc_meminfo = PROCFS_STATIC(PFS_MEMINFO, VNODE_REG, "meminfo");
static procfs_static_t proc_cpuinfo = PROCFS_STATIC(PFS_CPUINFO, VNODE_REG, "cpuinfo");
static procfs_static_t sys_root = PROCFS_STATIC(PFS_SYS_ROOT, VNODE_DIR, "sys");
static procfs_static_t sys_bus = PROCFS_STATIC(PFS_SYS_BUS, VNODE_DIR, "bus");
static procfs_static_t sys_pci = PROCFS_STATIC(PFS_SYS_PCI, VNODE_DIR, "pci");
static procfs_static_t sys_pci_devices =
    PROCFS_STATIC(PFS_SYS_PCI_DEVICES, VNODE_REG, "devices");

static void procfs_static_bind(procfs_static_t *node, procfs_static_t *parent)
{
    node->vnode.fs_data = &node->node;
    node->vnode.parent = parent != NULL ? &parent->vnode : NULL;
}

static void procfs_init_once(void)
{
    if (proc_root.vnode.fs_data != NULL) {
        return;
    }

    procfs_static_bind(&proc_meminfo, &proc_root);
    procfs_static_bind(&proc_cpuinfo, &proc_root);
    procfs_static_bind(&sys_bus, &sys_root);
    procfs_static_bind(&sys_pci, &sys_bus);
    procfs_static_bind(&sys_pci_devices, &sys_pci);
    procfs_static_bind(&sys_root, NULL);
    procfs_static_bind(&proc_root, NULL);
}

vnode_t *procfs_root(void)
{
    procfs_init_once();
    return &proc_root.vnode;
}

vnode_t *sysfs_root(void)
{
    procfs_init_once();
    return &sys_root.vnode;
}

static procfs_node_t *node_of(vnode_t *vnode)
{
    return (vnode != NULL) ? (procfs_node_t *)vnode->fs_data : NULL;
}

/* ------------------------------------------------------------------------- */
/* Text builder                                                               */
/* ------------------------------------------------------------------------- */

typedef struct text {
    char  *data;
    size_t len;
    size_t cap;
} text_t;

static void put_char(text_t *t, char c)
{
    if (t->len < t->cap) {
        t->data[t->len++] = c;
    }
}

static void put_str(text_t *t, const char *s)
{
    while (*s != '\0') {
        put_char(t, *s++);
    }
}

static void put_dec(text_t *t, uint64_t value)
{
    char digits[24];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    } while (value != 0);

    while (count > 0) {
        put_char(t, digits[--count]);
    }
}

static void put_sdec(text_t *t, int64_t value)
{
    if (value < 0) {
        put_char(t, '-');
        put_dec(t, (uint64_t)(-(value + 1)) + 1U);
        return;
    }
    put_dec(t, (uint64_t)value);
}

static void put_hex(text_t *t, uint64_t value, uint32_t width)
{
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    uint32_t count = 0;

    do {
        digits[count++] = hex[value & 0xFU];
        value >>= 4;
    } while (value != 0 && count < 16U);

    while (count < width && count < 16U) {
        digits[count++] = '0';
    }
    while (count > 0) {
        put_char(t, digits[--count]);
    }
}

static void put_field(text_t *t, const char *key, uint64_t value,
                      const char *unit)
{
    put_str(t, key);
    put_char(t, '\t');
    put_dec(t, value);
    if (unit != NULL) {
        put_char(t, ' ');
        put_str(t, unit);
    }
    put_char(t, '\n');
}

static const char *qos_name(uint8_t qos)
{
    switch (qos) {
    case THREAD_QOS_BACKGROUND:
        return "background";
    case THREAD_QOS_DEFAULT:
        return "default";
    case THREAD_QOS_INTERACTIVE:
        return "interactive";
    default:
        return "unknown";
    }
}

/* ------------------------------------------------------------------------- */
/* Generators                                                                 */
/* ------------------------------------------------------------------------- */

static void gen_meminfo(text_t *t)
{
    uint64_t total = pmm_total_frame_count();
    uint64_t free = pmm_free_frame_count();
    uint64_t used = total >= free ? total - free : 0;
    uint64_t kib = PAGE_SIZE / 1024ULL;

    put_field(t, "MemTotal:", total * kib, "kB");
    put_field(t, "MemFree:", free * kib, "kB");
    put_field(t, "MemUsed:", used * kib, "kB");
    put_field(t, "PmmTotalFrames:", total, NULL);
    put_field(t, "PmmFreeFrames:", free, NULL);
    put_field(t, "PmmUsedFrames:", used, NULL);
    put_field(t, "PageSize:", PAGE_SIZE, "B");
}

static void gen_cpuinfo(text_t *t)
{
    char vendor[13];
    uint32_t online = smp_online_count();

    cpu_vendor_string(vendor);
    if (online == 0) {
        online = 1;
    }

    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        cpu_local_t *local = cpu_local_for(cpu);

        /* Before smp_prepare() the BSP is the only CPU and is not flagged. */
        if (local == NULL ||
            (cpu != 0 && __atomic_load_n(&local->online, __ATOMIC_ACQUIRE) == 0)) {
            continue;
        }

        put_str(t, "processor\t: ");
        put_dec(t, cpu);
        put_str(t, "\nvendor_id\t: ");
        put_str(t, vendor);
        put_str(t, "\napicid\t\t: ");
        put_dec(t, local->lapic_id);
        put_str(t, "\ncore id\t\t: ");
        put_dec(t, cpu);
        put_str(t, "\ncpu cores\t: ");
        put_dec(t, online);
        put_str(t, "\nflags\t\t: fpu sse sse2 fxsr\n\n");
    }
}

static int32_t resolve_pid(const procfs_node_t *node)
{
    return node->pid;
}

static void gen_status(text_t *t, int32_t pid)
{
    bsd_proc_info_t info;

    if (!bsd_proc_snapshot(pid, false, &info)) {
        return;
    }

    put_str(t, "Name:\t");
    put_str(t, info.comm);
    put_str(t, "\nState:\t");
    put_str(t, info.state == PROC_ZOMBIE ? "Z (zombie)" : "R (running)");
    put_str(t, "\nPid:\t");
    put_sdec(t, info.pid);
    put_str(t, "\nPPid:\t");
    put_sdec(t, info.ppid);
    put_str(t, "\nPGid:\t");
    put_sdec(t, info.pgid);
    put_str(t, "\nUid:\t");
    put_dec(t, info.uid);
    put_str(t, "\nGid:\t");
    put_dec(t, info.gid);
    put_str(t, "\nThreads:\t");
    put_dec(t, info.threads);
    put_str(t, "\nTaskId:\t");
    put_dec(t, info.task_id);
    put_str(t, "\nQosBase:\t");
    put_str(t, qos_name(info.base_qos));
    put_str(t, "\nQosEffective:\t");
    put_str(t, qos_name(info.effective_qos));
    put_str(t, "\nSigPnd:\t");
    put_hex(t, info.pending_signals, 16);
    put_str(t, "\nSigBlk:\t");
    put_hex(t, info.signal_mask, 16);
    put_str(t, "\nBrk:\t");
    put_hex(t, info.brk_base, 16);
    put_char(t, '-');
    put_hex(t, info.brk_current, 16);
    put_char(t, '\n');
}

typedef struct range_list {
    vspace_range_t *ranges;
    uint32_t        count;
    bool            truncated;
} range_list_t;

static bool collect_range(const vspace_range_t *range, void *ctx)
{
    range_list_t *list = (range_list_t *)ctx;

    if (list->count >= PROCFS_MAX_RANGES) {
        list->truncated = true;
        return false;
    }
    list->ranges[list->count++] = *range;
    return true;
}

static bool overlaps(vaddr_t a_start, vaddr_t a_end, vaddr_t b_start,
                     vaddr_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

static const char *range_label(const bsd_proc_info_t *info,
                               const vspace_range_t *range)
{
    vaddr_t stack_low = ELF_USER_STACK_TOP - ELF_USER_STACK_SIZE;

    if ((range->prot & VSPACE_PROT_DEVICE) != 0) {
        return "[device]";
    }
    if (overlaps(range->start, range->end, stack_low, ELF_USER_STACK_TOP)) {
        return "[stack]";
    }
    if (info->brk_current > info->brk_base &&
        overlaps(range->start, range->end, info->brk_base,
                 info->brk_current)) {
        return "[heap]";
    }
    for (size_t i = 0; i < BSD_MAX_MMAPS; i++) {
        const bsd_mmap_region_t *region = &info->mmap_regions[i];

        if (region->length != 0 &&
            overlaps(range->start, range->end, region->base,
                     region->base + region->length)) {
            return "[mmap]";
        }
    }
    if (range->start >= BSD_MMAP_BASE && range->start < BSD_MMAP_LIMIT) {
        return "[anon]";
    }
    return info->comm;
}

static void gen_maps(text_t *t, int32_t pid)
{
    bsd_proc_info_t info;

    if (!bsd_proc_snapshot(pid, true, &info) || info.vspace == NULL) {
        return;
    }

    range_list_t list = { NULL, 0, false };

    list.ranges = (vspace_range_t *)kalloc(sizeof(vspace_range_t) *
                                           PROCFS_MAX_RANGES);
    if (list.ranges != NULL) {
        vspace_user_ranges(info.vspace, collect_range, &list);
    }
    vspace_destroy(info.vspace);

    if (list.ranges == NULL) {
        return;
    }

    for (uint32_t i = 0; i < list.count; i++) {
        const vspace_range_t *range = &list.ranges[i];

        put_hex(t, range->start, 12);
        put_char(t, '-');
        put_hex(t, range->end, 12);
        put_char(t, ' ');
        put_char(t, 'r');
        put_char(t, (range->prot & VSPACE_PROT_WRITE) != 0 ? 'w' : '-');
        put_char(t, (range->prot & VSPACE_PROT_EXEC) != 0 ? 'x' : '-');
        put_char(t, (range->prot & VSPACE_PROT_DEVICE) != 0 ? 's' : 'p');
        put_str(t, " 00000000 00:00 0 ");
        put_str(t, range_label(&info, range));
        put_char(t, '\n');
    }

    kfree(list.ranges);
}

static void gen_pci_devices(text_t *t)
{
    pci_device_t device;

    put_str(t, "# bus:slot.func vendor:device class/subclass irq pin "
               "bar0..bar5 (i = I/O port space)\n");

    for (size_t index = 0; pci_device_snapshot(index, &device); index++) {
        put_hex(t, device.bus, 2);
        put_char(t, ':');
        put_hex(t, device.slot, 2);
        put_char(t, '.');
        put_hex(t, device.func, 1);
        put_char(t, ' ');
        put_hex(t, device.vendor_id, 4);
        put_char(t, ':');
        put_hex(t, device.device_id, 4);
        put_char(t, ' ');
        put_hex(t, device.class_code, 2);
        put_char(t, '/');
        put_hex(t, device.subclass, 2);
        put_str(t, " irq=");
        put_dec(t, device.interrupt_line);
        put_str(t, " pin=");
        put_dec(t, device.interrupt_pin);

        for (uint32_t bar = 0; bar < PCI_BAR_COUNT; bar++) {
            put_str(t, " bar");
            put_dec(t, bar);
            put_str(t, "=0x");
            put_hex(t, device.bar[bar], 1);
            if (device.bar[bar] != 0 && device.bar_io[bar]) {
                put_char(t, 'i');
            }
        }
        put_char(t, '\n');
    }
}

static sys_status_t procfs_read(vnode_t *vnode, void *buf, size_t len,
                                uint64_t offset, size_t *done)
{
    procfs_node_t *node = node_of(vnode);

    *done = 0;
    if (node == NULL || vnode->type != VNODE_REG) {
        return BSD_ERR(BSD_EINVAL);
    }

    text_t text = { NULL, 0, PROCFS_TEXT_MAX };

    text.data = (char *)kalloc(PROCFS_TEXT_MAX);
    if (text.data == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    switch (node->kind) {
    case PFS_MEMINFO:
        gen_meminfo(&text);
        break;
    case PFS_CPUINFO:
        gen_cpuinfo(&text);
        break;
    case PFS_PID_STATUS:
        gen_status(&text, resolve_pid(node));
        break;
    case PFS_PID_MAPS:
        gen_maps(&text, resolve_pid(node));
        break;
    case PFS_SYS_PCI_DEVICES:
        gen_pci_devices(&text);
        break;
    default:
        break;
    }

    if (offset < text.len) {
        size_t chunk = text.len - (size_t)offset;

        if (chunk > len) {
            chunk = len;
        }
        memcpy(buf, text.data + offset, chunk);
        *done = chunk;
    }

    kfree(text.data);
    return SYS_OK;
}

/* ------------------------------------------------------------------------- */
/* Namespace                                                                  */
/* ------------------------------------------------------------------------- */

static void format_pid(char out[16], int32_t pid)
{
    text_t text = { out, 0, 15 };

    put_dec(&text, (uint64_t)(uint32_t)pid);
    out[text.len] = '\0';
}

static sys_status_t make_dynamic(procfs_kind_t kind, int32_t pid,
                                 const char *name, vnode_t **out)
{
    procfs_dynamic_t *dyn = (procfs_dynamic_t *)kalloc(sizeof(*dyn));

    if (dyn == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    memset(dyn, 0, sizeof(*dyn));
    size_t length = strlen(name);
    if (length >= sizeof(dyn->name)) {
        length = sizeof(dyn->name) - 1U;
    }
    memcpy(dyn->name, name, length);

    bool dir = kind == PFS_PID_DIR;

    dyn->node.kind = kind;
    dyn->node.pid = pid;
    dyn->node.dynamic = true;
    dyn->vnode.type = dir ? VNODE_DIR : VNODE_REG;
    dyn->vnode.mode = dir ? (BSD_S_IFDIR | 0555U) : (BSD_S_IFREG | 0444U);
    dyn->vnode.ino = PROCFS_INO_BASE + 0x1000ULL +
                     (uint64_t)(uint32_t)pid * 8ULL + (uint64_t)kind;
    dyn->vnode.ref_count = 1;
    dyn->vnode.ops = &procfs_ops;
    dyn->vnode.fs_data = &dyn->node;
    /* Only permanent nodes are used as parents, so ".." never dangles. */
    dyn->vnode.parent = &proc_root.vnode;
    dyn->vnode.name = dyn->name;

    *out = &dyn->vnode;
    return SYS_OK;
}

static void procfs_release(vnode_t *vnode)
{
    procfs_node_t *node = node_of(vnode);

    if (node != NULL && node->dynamic) {
        kfree((procfs_dynamic_t *)(void *)vnode);
    }
}

static bool parse_pid(const char *name, int32_t *pid_out)
{
    int64_t value = 0;

    if (name[0] == '\0' || name[0] == '0') {
        return false;
    }
    for (size_t i = 0; name[i] != '\0'; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = value * 10 + (name[i] - '0');
        if (value >= BSD_MAX_PROCS) {
            return false;
        }
    }

    *pid_out = (int32_t)value;
    return true;
}

static sys_status_t lookup_pid_dir(int32_t pid, vnode_t **out)
{
    bsd_proc_info_t info;

    if (!bsd_proc_snapshot(pid, false, &info)) {
        return BSD_ERR(BSD_ENOENT);
    }

    char name[16];
    format_pid(name, pid);
    return make_dynamic(PFS_PID_DIR, pid, name, out);
}

static sys_status_t static_ref(procfs_static_t *node, vnode_t **out)
{
    *out = vfs_vnode_ref(&node->vnode);
    return SYS_OK;
}

static sys_status_t procfs_lookup(vnode_t *dir, const char *name,
                                  vnode_t **out)
{
    procfs_node_t *node = node_of(dir);

    if (node == NULL || name == NULL || out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    switch (node->kind) {
    case PFS_PROC_ROOT: {
        if (strcmp(name, "meminfo") == 0) {
            return static_ref(&proc_meminfo, out);
        }
        if (strcmp(name, "cpuinfo") == 0) {
            return static_ref(&proc_cpuinfo, out);
        }
        if (strcmp(name, "self") == 0) {
            bsd_proc_t *self = bsd_proc_current();

            return self != NULL ? lookup_pid_dir(self->pid, out)
                                : BSD_ERR(BSD_ENOENT);
        }

        int32_t pid = 0;
        if (parse_pid(name, &pid)) {
            return lookup_pid_dir(pid, out);
        }
        return BSD_ERR(BSD_ENOENT);
    }
    case PFS_PID_DIR:
        if (strcmp(name, "status") == 0) {
            return make_dynamic(PFS_PID_STATUS, node->pid, "status", out);
        }
        if (strcmp(name, "maps") == 0) {
            return make_dynamic(PFS_PID_MAPS, node->pid, "maps", out);
        }
        return BSD_ERR(BSD_ENOENT);
    case PFS_SYS_ROOT:
        return strcmp(name, "bus") == 0 ? static_ref(&sys_bus, out)
                                         : BSD_ERR(BSD_ENOENT);
    case PFS_SYS_BUS:
        return strcmp(name, "pci") == 0 ? static_ref(&sys_pci, out)
                                         : BSD_ERR(BSD_ENOENT);
    case PFS_SYS_PCI:
        return strcmp(name, "devices") == 0
                   ? static_ref(&sys_pci_devices, out)
                   : BSD_ERR(BSD_ENOENT);
    default:
        return BSD_ERR(BSD_ENOTDIR);
    }
}

static void dirent_fill(bsd_dirent_t *entry, const char *name, uint64_t ino,
                        bool dir)
{
    memset(entry, 0, sizeof(*entry));

    size_t length = strlen(name);
    if (length > BSD_NAME_MAX) {
        length = BSD_NAME_MAX;
    }

    memcpy(entry->d_name, name, length);
    entry->d_namlen = (uint8_t)length;
    entry->d_ino = ino;
    entry->d_type = (uint8_t)(dir ? BSD_DT_DIR : BSD_DT_REG);
    entry->d_reclen = (uint16_t)sizeof(*entry);
}

static sys_status_t procfs_readdir(vnode_t *dir, uint64_t index,
                                   bsd_dirent_t *entry)
{
    procfs_node_t *node = node_of(dir);

    if (node == NULL || entry == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    switch (node->kind) {
    case PFS_PROC_ROOT: {
        if (index == 0) {
            dirent_fill(entry, "meminfo", proc_meminfo.vnode.ino, false);
            return SYS_OK;
        }
        if (index == 1) {
            dirent_fill(entry, "cpuinfo", proc_cpuinfo.vnode.ino, false);
            return SYS_OK;
        }
        if (index == 2) {
            dirent_fill(entry, "self", PROCFS_INO_BASE + 0xFFFULL, true);
            return SYS_OK;
        }

        int32_t pid = 0;
        for (uint64_t skip = index - 3; ; skip--) {
            pid = bsd_proc_next_pid(pid);
            if (pid == 0) {
                return BSD_ERR(BSD_ENOENT);
            }
            if (skip == 0) {
                break;
            }
        }

        char name[16];
        format_pid(name, pid);
        dirent_fill(entry, name,
                    PROCFS_INO_BASE + 0x1000ULL + (uint64_t)pid * 8ULL +
                        PFS_PID_DIR,
                    true);
        return SYS_OK;
    }
    case PFS_PID_DIR:
        if (index == 0) {
            dirent_fill(entry, "status", dir->ino + 1U, false);
            return SYS_OK;
        }
        if (index == 1) {
            dirent_fill(entry, "maps", dir->ino + 2U, false);
            return SYS_OK;
        }
        return BSD_ERR(BSD_ENOENT);
    case PFS_SYS_ROOT:
        if (index == 0) {
            dirent_fill(entry, "bus", sys_bus.vnode.ino, true);
            return SYS_OK;
        }
        return BSD_ERR(BSD_ENOENT);
    case PFS_SYS_BUS:
        if (index == 0) {
            dirent_fill(entry, "pci", sys_pci.vnode.ino, true);
            return SYS_OK;
        }
        return BSD_ERR(BSD_ENOENT);
    case PFS_SYS_PCI:
        if (index == 0) {
            dirent_fill(entry, "devices", sys_pci_devices.vnode.ino, false);
            return SYS_OK;
        }
        return BSD_ERR(BSD_ENOENT);
    default:
        return BSD_ERR(BSD_ENOTDIR);
    }
}
