#ifndef LUCYK_BSD_VFS_H
#define LUCYK_BSD_VFS_H

#include <bsd/bsd.h>
#include <bsd/errno.h>
#include <spinlock.h>
#include <types.h>

typedef enum vnode_type {
    VNODE_REG = 0,
    VNODE_DIR,
    VNODE_CHR
} vnode_type_t;

typedef struct vnode vnode_t;
typedef struct file file_t;

/* Filesystem/device operations. A node leaves a slot NULL when it does not
 * provide that operation; callers report -BSD_EINVAL instead of calling it. */
typedef struct vnode_ops {
    sys_status_t (*read)(vnode_t *vnode, void *buf, size_t len,
                         uint64_t offset, size_t *done);
    sys_status_t (*write)(vnode_t *vnode, const void *buf, size_t len,
                          uint64_t offset, size_t *done);
    sys_status_t (*lookup)(vnode_t *dir, const char *name, vnode_t **out);
    sys_status_t (*readdir)(vnode_t *dir, uint64_t index,
                            bsd_dirent_t *entry);
    void (*close)(vnode_t *vnode, file_t *file);
    void (*release)(vnode_t *vnode);
    sys_status_t (*create)(vnode_t *dir, const char *name, uint32_t mode,
                           vnode_t **out);
    sys_status_t (*mkdir)(vnode_t *dir, const char *name, uint32_t mode,
                          vnode_t **out);
    sys_status_t (*unlink)(vnode_t *dir, const char *name);
    sys_status_t (*rmdir)(vnode_t *dir, const char *name);
    sys_status_t (*truncate)(vnode_t *vnode);
    sys_status_t (*get_size)(vnode_t *vnode, uint64_t *size);
} vnode_ops_t;

struct vnode {
    vnode_type_t type;
    uint32_t     mode;      /* BSD_S_IF* | permission bits */
    uint64_t     size;
    uint64_t     ino;
    uint32_t     ref_count;
    vnode_ops_t *ops;
    void        *fs_data;
    vnode_t     *parent;
    const char  *name;      /* Final path component; NULL when unlinked */
};

/* Open file description shared by dup()/fork() in later phases. */
struct file {
    spinlock_t lock;        /* serializes offset + regular-file I/O */
    vnode_t *vnode;
    uint64_t offset;
    uint32_t flags;
    uint32_t ref_count;
};

struct bsd_proc;
struct task;

void vfs_init(void);

/* Installs the root vnode; the caller transfers one reference. */
sys_status_t vfs_set_root(vnode_t *root);

/* Binds mount_vnode at an absolute path. The mount table borrows both the
 * parent and the mounted vnode, so mount points must be permanent nodes. */
sys_status_t vfs_mount(const char *path, vnode_t *mount_vnode);

vnode_t *vfs_root(void);
vnode_t *vfs_dev_node(void);

vnode_t *vfs_vnode_ref(vnode_t *vnode);
void vfs_vnode_unref(vnode_t *vnode);
void vfs_file_ref(file_t *file);
void vfs_file_unref(file_t *file);

sys_status_t vfs_lookup(struct bsd_proc *proc, const char *path,
                        vnode_t **out);
sys_status_t vfs_open(struct bsd_proc *proc, const char *path, uint32_t flags,
                      uint32_t mode, file_t **out);
sys_status_t vfs_mkdir(struct bsd_proc *proc, const char *path, uint32_t mode);
sys_status_t vfs_rmdir(struct bsd_proc *proc, const char *path);
sys_status_t vfs_unlink(struct bsd_proc *proc, const char *path);
sys_status_t vfs_read(file_t *file, void *buf, size_t len, size_t *done);
sys_status_t vfs_write(file_t *file, const void *buf, size_t len,
                       size_t *done);
sys_status_t vfs_lseek(file_t *file, int64_t offset, uint32_t whence,
                       uint64_t *result);
sys_status_t vfs_fstat(file_t *file, bsd_stat_t *st);
sys_status_t vfs_stat_path(struct bsd_proc *proc, const char *path,
                           bsd_stat_t *st);

/* Directory iteration writes whole bsd_dirent_t records and advances the
 * directory file offset. */
sys_status_t vfs_getdents(file_t *file, void *buf, size_t len, size_t *done);

sys_status_t vfs_chdir(struct bsd_proc *proc, const char *path);
sys_status_t vfs_getcwd(struct bsd_proc *proc, char *buf, size_t size,
                        size_t *len_out);

/* Descriptor duplication and pipes. */
int32_t vfs_dup(struct bsd_proc *proc, int32_t oldfd);
int32_t vfs_dup2(struct bsd_proc *proc, int32_t oldfd, int32_t newfd);
sys_status_t vfs_pipe(struct bsd_proc *proc, int32_t fds_out[2]);
uint32_t vfs_live_pipe_count(void);

/* Removes every waiter owned by task from all pipe wait queues. Called before
 * a process's descriptors are closed so no dangling thread_t remains. */
void vfs_pipe_abort_task(struct task *task);

/* Per-process descriptor table helpers. bsd_fd_alloc() consumes the caller's
 * file reference on success and leaves it untouched on failure. */
int32_t bsd_fd_alloc(struct bsd_proc *proc, file_t *file);
sys_status_t bsd_fd_get(struct bsd_proc *proc, int32_t fd, file_t **out);
sys_status_t bsd_fd_close(struct bsd_proc *proc, int32_t fd);
void bsd_fd_close_all(struct bsd_proc *proc);

/* Mounts the virtual /proc and /sys trees (src/bsd/procfs.c) beside /dev.
 * Requires the root filesystem to be in place. */
sys_status_t vfs_mount_introspection(void);

/* Permanent root directory vnodes of the introspection filesystems. */
vnode_t *procfs_root(void);
vnode_t *sysfs_root(void);

/* Read-only USTAR root filesystem. */
sys_status_t initrd_mount(const void *image, size_t size);

#endif /* LUCYK_BSD_VFS_H */
