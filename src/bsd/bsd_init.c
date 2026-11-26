#include <bsd/bsd.h>
#include <bsd/console.h>
#include <bsd/errno.h>
#include <bsd/proc.h>
#include <bsd/vfs.h>
#include <elf.h>
#include <kalloc.h>
#include <klog.h>
#include <mk_hooks.h>
#include <object.h>
#include <pmm.h>
#include <sched.h>
#include <string.h>
#include <syscall.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

#define BSD_INIT_HEAP_RESERVE (1024ULL * 1024ULL)
#define BSD_INIT_IMAGE_MAX    (16ULL * 1024ULL * 1024ULL)

typedef struct bsd_init_job {
    int32_t pid;
    size_t  frames_before;
} bsd_init_job_t;

static bsd_init_job_t bsd_init_job;

static bool module_name_matches(const char *path, const char *name)
{
    size_t path_len = strlen(path);
    size_t name_len = strlen(name);

    if (name_len > path_len) {
        return false;
    }

    const char *tail = path + (path_len - name_len);

    if (strcmp(tail, name) != 0) {
        return false;
    }

    return path_len == name_len || tail[-1] == '/';
}

static const bsd_module_t *module_find(const bsd_module_t *modules,
                                       size_t module_count,
                                       const char *name)
{
    if (modules == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < module_count; i++) {
        if (modules[i].name != NULL && modules[i].address != NULL &&
            module_name_matches(modules[i].name, name)) {
            return &modules[i];
        }
    }

    return NULL;
}

static sys_status_t load_init_image(const char *path, void **image_out,
                                    size_t *size_out)
{
    file_t *file = NULL;
    sys_status_t status = vfs_open(NULL, path, BSD_O_RDONLY, 0, &file);

    if (status != SYS_OK) {
        return status;
    }

    vnode_t *vnode = file->vnode;
    if (vnode == NULL || vnode->type != VNODE_REG || vnode->size == 0 ||
        vnode->size > BSD_INIT_IMAGE_MAX) {
        vfs_file_unref(file);
        return BSD_ERR(BSD_ENOEXEC);
    }

    size_t size = (size_t)vnode->size;
    uint8_t *image = (uint8_t *)kalloc(size);

    if (image == NULL) {
        vfs_file_unref(file);
        return BSD_ERR(BSD_ENOMEM);
    }

    size_t total = 0;

    while (total < size) {
        size_t done = 0;

        status = vfs_read(file, image + total, size - total, &done);
        if (status != SYS_OK || done == 0) {
            break;
        }
        total += done;
    }

    vfs_file_unref(file);

    if (total != size) {
        kfree(image);
        return status != SYS_OK ? status : BSD_ERR(BSD_EIO);
    }

    *image_out = image;
    *size_out = size;
    return SYS_OK;
}

static void bsd_user_entry(void *arg)
{
    enter_usermode((uint64_t)(uintptr_t)arg, ELF_USER_STACK_TOP);
    __builtin_unreachable();
}

/* Page-aligned end of the highest PT_LOAD segment, used as the initial brk. */
uint64_t bsd_elf_image_break(const void *image, size_t size)
{
    if (image == NULL || size < sizeof(elf64_ehdr_t)) {
        return 0;
    }

    const elf64_ehdr_t *header = (const elf64_ehdr_t *)image;

    if (header->e_ident[0] != ELF_MAGIC0 ||
        header->e_ident[1] != ELF_MAGIC1 ||
        header->e_ident[2] != ELF_MAGIC2 ||
        header->e_ident[3] != ELF_MAGIC3) {
        return 0;
    }
    if (header->e_phentsize != sizeof(elf64_phdr_t) || header->e_phnum == 0) {
        return 0;
    }
    if (header->e_phoff > size ||
        (uint64_t)header->e_phnum >
            (size - header->e_phoff) / sizeof(elf64_phdr_t)) {
        return 0;
    }

    uint64_t end = 0;

    for (uint16_t i = 0; i < header->e_phnum; i++) {
        const elf64_phdr_t *segment =
            (const elf64_phdr_t *)((const uint8_t *)image + header->e_phoff +
                                   ((uint64_t)i * sizeof(elf64_phdr_t)));

        if (segment->p_type != PT_LOAD || segment->p_memsz == 0) {
            continue;
        }
        if (segment->p_vaddr > UINT64_MAX - segment->p_memsz) {
            return 0;
        }

        uint64_t segment_end = segment->p_vaddr + segment->p_memsz;

        if (segment_end > end) {
            end = segment_end;
        }
    }

    if (end == 0 || end > 0x00007FFFFFFFFFFFULL - PAGE_SIZE) {
        return 0;
    }

    return PAGE_ALIGN_UP(end);
}

/* Tears down a partially created PID 1 after a spawn failure. No thread has
 * been attached yet, so task_destroy() is legal. */
static void discard_init(task_t *task, bsd_proc_t *proc)
{
    if (proc != NULL) {
        (void)bsd_proc_exit(proc, 127);
        task->personality_data = NULL;
        proc->mk_task = NULL;
        bsd_proc_reap(proc);
    }
    task_destroy(task);
}

static bsd_proc_t *spawn_init(void)
{
    task_t *task = task_create();
    if (task == NULL) {
        klog(KLOG_ERROR, "bsd: task_create for PID 1 failed");
        return NULL;
    }

    void *image = NULL;
    size_t image_size = 0;
    sys_status_t status = load_init_image("/bin/init", &image, &image_size);

    if (status != SYS_OK) {
        klog(KLOG_ERROR, "bsd: cannot read /bin/init from initrd (%ld)",
             (long)status);
        task_destroy(task);
        return NULL;
    }

    uint64_t brk_base = bsd_elf_image_break(image, image_size);
    uint64_t entry = 0;
    status = elf64_load(task->vspace, image, image_size, &entry);
    kfree(image);

    if (status != SYS_OK) {
        klog(KLOG_ERROR, "bsd: /bin/init is not a loadable ELF64 image (%ld)",
             (long)status);
        task_destroy(task);
        return NULL;
    }
    if (brk_base == 0) {
        klog(KLOG_ERROR, "bsd: cannot derive the initial program break");
        task_destroy(task);
        return NULL;
    }

    bsd_proc_t *proc = bsd_proc_create(task, 1, 0, "init");
    if (proc == NULL) {
        klog(KLOG_ERROR, "bsd: cannot allocate PID 1 in the process table");
        task_destroy(task);
        return NULL;
    }

    proc->brk_base = brk_base;
    proc->brk_current = brk_base;
    proc->pgid = 1; /* PID 1 is its own process group leader. */

    /* stdin is read-only; stdout and stderr are write-only. Each descriptor
     * gets its own open file description, matching BSD descriptor semantics. */
    static const uint32_t init_fd_flags[3] = {
        BSD_O_RDONLY,
        BSD_O_WRONLY,
        BSD_O_WRONLY,
    };

    for (int32_t fd = 0; fd < 3; fd++) {
        file_t *console = NULL;

        status = vfs_open(NULL, "/dev/console", init_fd_flags[fd], 0,
                          &console);
        if (status != SYS_OK) {
            klog(KLOG_ERROR,
                 "bsd: cannot open /dev/console for fd %d (%ld)", (int)fd,
                 (long)status);
            discard_init(task, proc);
            return NULL;
        }

        int32_t installed = bsd_fd_alloc(proc, console);

        if (installed != fd) {
            vfs_file_unref(console);
            klog(KLOG_ERROR,
                 "bsd: cannot bind /dev/console to fd %d (%ld)", (int)fd,
                 (long)installed);
            discard_init(task, proc);
            return NULL;
        }
    }

    console_set_foreground(1);
    console_set_foreground_pgid(1);

    if (thread_create(task, bsd_user_entry, (void *)(uintptr_t)entry) == NULL) {
        console_set_foreground(0);
        klog(KLOG_ERROR, "bsd: cannot create PID 1 thread");
        discard_init(task, proc);
        return NULL;
    }

    klog(KLOG_INFO,
         "bsd: PID 1 spawned from /bin/init (entry 0x%016lx, brk 0x%016lx)",
         entry, brk_base);

    return proc;
}

static void bsd_init_reaper(void *arg)
{
    bsd_init_job_t *job = (bsd_init_job_t *)arg;
    bsd_proc_t *proc = NULL;

    for (;;) {
        proc = bsd_proc_lookup(job->pid);
        if (proc == NULL || proc->state == PROC_ZOMBIE) {
            break;
        }
        thread_sleep_ms(5);
    }

    bool clean = false;
    size_t regions = 0;
    bool brk_restored = false;

    if (proc != NULL) {
        int32_t status = proc->exit_status;
        task_t *task = proc->mk_task;
        int32_t pid = proc->pid;

        clean = status == 0;
        klog(KLOG_INFO, "bsd: PID %d wait status %d", (int)pid, (int)status);

        for (size_t i = 0; i < BSD_MAX_MMAPS; i++) {
            if (proc->mmap_regions[i].length != 0) {
                regions++;
            }
        }
        brk_restored = proc->brk_current == proc->brk_base;

        /* A BSD_SYS_EXIT handler marks the process a zombie before its thread
         * leaves Ring 0. Wait until the task owns no threads, then let one
         * more context switch reclaim the dead thread's stack. */
        uint64_t deadline = sched_tick_count() + 2000;

        while (task != NULL && task->thread_count != 0) {
            if (sched_tick_count() > deadline) {
                break;
            }
            thread_sleep_ms(2);
        }

        thread_sleep_ms(20);

        if (task != NULL && task->thread_count == 0) {
            task->personality_data = NULL;
            proc->mk_task = NULL;
            task_destroy(task);
            bsd_proc_reap(proc);
        } else {
            clean = false;
            klog(KLOG_ERROR, "bsd: PID %d did not quiesce before destruction",
                 (int)pid);
        }
    }

    size_t frames_after = pmm_free_frame_count();
    uint64_t objects_after = mk_object_live_count();
    uint32_t pipes_after = vfs_live_pipe_count();

    if (clean && frames_after == job->frames_before && objects_after == 0 &&
        pipes_after == 0 && regions == 0 && brk_restored) {
        klog(KLOG_INFO,
             "bsd: Phase 2 acceptance: mmap/brk, getdents, stat/chdir/getcwd, "
             "pipe/dup2/ioctl/kill verified; 0 leaked PMM frames (%lu free), "
             "0 leaked mk_object handles, 0 live pipes",
             (unsigned long)frames_after);
    } else {
        klog(KLOG_ERROR,
             "bsd: Phase 2 acceptance failed (clean=%d, frames %lu -> %lu, "
             "objects %lu, pipes %lu, regions %lu, brk_restored=%d)",
             clean ? 1 : 0, (unsigned long)job->frames_before,
             (unsigned long)frames_after, (unsigned long)objects_after,
             (unsigned long)pipes_after, (unsigned long)regions,
             brk_restored ? 1 : 0);
    }

    thread_exit();
}

void bsd_init(const bsd_module_t *modules, size_t module_count)
{
    /* Reserve heap capacity once so personality startup does not grow the heap
     * and skew the frame accounting above. */
    void *reserve = kalloc((size_t)BSD_INIT_HEAP_RESERVE);
    if (reserve == NULL) {
        kpanic("bsd: cannot reserve kernel heap for the personality layer");
    }
    kfree(reserve);

    size_t frames_before = pmm_free_frame_count();

    bsd_proc_init();
    vfs_init();

    if (vfs_root() == NULL) {
        const bsd_module_t *initrd =
            module_find(modules, module_count, "initrd.tar");

        if (initrd == NULL) {
            kpanic("bsd: no ext2 root and Limine module initrd.tar was not provided");
        }
        if (initrd_mount(initrd->address, initrd->size) != SYS_OK) {
            kpanic("bsd: initrd.tar is not a valid USTAR archive");
        }
    } else {
        klog(KLOG_INFO, "bsd: ext2 root filesystem active");
    }

    console_init();

    if (vfs_mount("/dev", vfs_dev_node()) != SYS_OK) {
        kpanic("bsd: cannot mount /dev");
    }
    if (vfs_mount("/dev/console", console_vnode()) != SYS_OK) {
        kpanic("bsd: cannot mount /dev/console");
    }

    mk_register_hooks(bsd_proc_get_hooks());
    klog(KLOG_INFO, "bsd: personality hooks registered");

    bsd_proc_t *init = spawn_init();
    if (init == NULL) {
        kpanic("bsd: cannot start PID 1 (/bin/init)");
    }

    bsd_init_job.pid = 1;
    bsd_init_job.frames_before = frames_before;

    if (thread_create(sched_boot_task(), bsd_init_reaper, &bsd_init_job) ==
        NULL) {
        kpanic("bsd: cannot create the PID 1 reaper thread");
    }
}
