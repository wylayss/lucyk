#include <bsd/bsd.h>
#include <bsd/errno.h>
#include <bsd/ext2.h>
#include <bsd/proc.h>
#include <bsd/vfs.h>
#include <io.h>
#include <io/blkdev.h>
#include <kalloc.h>
#include <klog.h>
#include <sched.h>
#include <string.h>
#include <types.h>

#define VFS_MAX_MOUNTS 8

typedef struct vfs_mount_entry {
    bool     used;
    vnode_t *parent;
    vnode_t *child;
    char     name[BSD_NAME_MAX + 1];
} vfs_mount_entry_t;

static vnode_t vfs_dev_node_storage;
static vnode_t *vfs_root_node;
static vfs_mount_entry_t vfs_mounts[VFS_MAX_MOUNTS];

static void dev_node_init(void)
{
    memset(&vfs_dev_node_storage, 0, sizeof(vfs_dev_node_storage));
    vfs_dev_node_storage.type = VNODE_DIR;
    vfs_dev_node_storage.mode = BSD_S_IFDIR | 0555U;
    vfs_dev_node_storage.ino = 2;
    vfs_dev_node_storage.ref_count = 1;
    vfs_dev_node_storage.ops = NULL;
    vfs_dev_node_storage.fs_data = NULL;
    vfs_dev_node_storage.parent = NULL;
    vfs_dev_node_storage.name = "dev";
}

void vfs_init(void)
{
    memset(vfs_mounts, 0, sizeof(vfs_mounts));
    vfs_root_node = NULL;
    dev_node_init();

    if (!blk_present()) {
        (void)blk_init();
    }

    blkdev_t *disk = blkdev_primary();

    if (disk != NULL) {
        sys_status_t status = ext2_mount(disk);

        if (status != SYS_OK) {
            klog(KLOG_WARN,
                 "vfs: ext2 root mount failed (%ld); falling back to initrd",
                 (long)status);
        }
    }
}

sys_status_t vfs_set_root(vnode_t *root)
{
    if (root == NULL || root->type != VNODE_DIR) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint64_t flags = irq_save();
    vfs_root_node = root;
    vfs_dev_node_storage.parent = root;
    irq_restore(flags);

    return SYS_OK;
}

vnode_t *vfs_root(void)
{
    uint64_t flags = irq_save();
    vnode_t *root = vfs_root_node;
    irq_restore(flags);
    return root;
}

vnode_t *vfs_dev_node(void)
{
    return &vfs_dev_node_storage;
}

vnode_t *vfs_vnode_ref(vnode_t *vnode)
{
    if (vnode == NULL) {
        return NULL;
    }

    __atomic_add_fetch(&vnode->ref_count, 1U, __ATOMIC_RELAXED);
    return vnode;
}

void vfs_vnode_unref(vnode_t *vnode)
{
    if (vnode == NULL) {
        return;
    }

    uint32_t old = __atomic_load_n(&vnode->ref_count, __ATOMIC_RELAXED);

    for (;;) {
        if (old == 0) {
            return;
        }
        if (__atomic_compare_exchange_n(&vnode->ref_count, &old, old - 1U,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break;
        }
    }

    if (old != 1U) {
        return;
    }

    vnode_ops_t *ops = vnode->ops;

    if (ops != NULL && ops->release != NULL) {
        ops->release(vnode);
    }
}

void vfs_file_ref(file_t *file)
{
    if (file == NULL) {
        return;
    }

    __atomic_add_fetch(&file->ref_count, 1U, __ATOMIC_RELAXED);
}

void vfs_file_unref(file_t *file)
{
    if (file == NULL) {
        return;
    }

    uint32_t old = __atomic_load_n(&file->ref_count, __ATOMIC_RELAXED);

    for (;;) {
        if (old == 0) {
            return;
        }
        if (__atomic_compare_exchange_n(&file->ref_count, &old, old - 1U,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break;
        }
    }

    if (old != 1U) {
        return;
    }

    vnode_t *vnode = file->vnode;
    file->vnode = NULL;

    if (vnode != NULL) {
        if (vnode->ops != NULL && vnode->ops->close != NULL) {
            vnode->ops->close(vnode, file);
        }
        vfs_vnode_unref(vnode);
    }
    kfree(file);
}

/* Caller holds no locks. The mount table borrows both nodes, so a matched
 * child is permanent and can be referenced after the table lock is dropped. */
static sys_status_t mount_lookup(vnode_t *parent, const char *name,
                                 vnode_t **out)
{
    vnode_t *child = NULL;

    uint64_t flags = irq_save();
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        vfs_mount_entry_t *entry = &vfs_mounts[i];

        if (entry->used && entry->parent == parent &&
            strcmp(entry->name, name) == 0) {
            child = entry->child;
            break;
        }
    }
    irq_restore(flags);

    if (child == NULL) {
        return BSD_ERR(BSD_ENOENT);
    }

    *out = vfs_vnode_ref(child);
    return SYS_OK;
}

static sys_status_t lookup_component(vnode_t *dir, const char *name,
                                     vnode_t **out)
{
    if (dir->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }

    sys_status_t status = mount_lookup(dir, name, out);
    if (status != BSD_ERR(BSD_ENOENT)) {
        return status;
    }

    if (dir->ops != NULL && dir->ops->lookup != NULL) {
        return dir->ops->lookup(dir, name, out);
    }

    return BSD_ERR(BSD_ENOENT);
}

static sys_status_t lookup_path(struct bsd_proc *proc, const char *path,
                                vnode_t **out)
{
    if (path == NULL || out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    size_t length = strlen(path);
    if (length == 0) {
        return BSD_ERR(BSD_ENOENT);
    }
    if (length >= BSD_PATH_MAX) {
        return BSD_ERR(BSD_ENAMETOOLONG);
    }

    vnode_t *current = NULL;

    if (path[0] == '/') {
        vnode_t *root = vfs_root();

        if (root == NULL) {
            return BSD_ERR(BSD_ENOENT);
        }
        current = vfs_vnode_ref(root);
    } else {
        if (proc == NULL) {
            return BSD_ERR(BSD_ENOENT);
        }

        uint64_t flags = irq_save();
        vnode_t *cwd = proc->cwd;

        if (cwd == NULL) {
            irq_restore(flags);
            return BSD_ERR(BSD_ENOENT);
        }
        current = vfs_vnode_ref(cwd);
        irq_restore(flags);
    }

    const char *cursor = path;

    while (*cursor != '\0') {
        while (*cursor == '/') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        const char *start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }

        size_t component_len = (size_t)(cursor - start);
        if (component_len > BSD_NAME_MAX) {
            vfs_vnode_unref(current);
            return BSD_ERR(BSD_ENAMETOOLONG);
        }

        char component[BSD_NAME_MAX + 1];
        memcpy(component, start, component_len);
        component[component_len] = '\0';

        if (current->type != VNODE_DIR) {
            vfs_vnode_unref(current);
            return BSD_ERR(BSD_ENOTDIR);
        }

        if (strcmp(component, ".") == 0) {
            continue;
        }

        vnode_t *next = NULL;

        if (strcmp(component, "..") == 0) {
            vnode_t *parent = current->parent;

            next = parent != NULL ? vfs_vnode_ref(parent)
                                  : vfs_vnode_ref(current);
        } else {
            sys_status_t status = lookup_component(current, component, &next);

            if (status != SYS_OK) {
                vfs_vnode_unref(current);
                return status;
            }
        }

        vfs_vnode_unref(current);
        current = next;
    }

    /* A trailing slash requires the resolved node to be a directory. */
    if (path[length - 1] == '/' && current->type != VNODE_DIR) {
        vfs_vnode_unref(current);
        return BSD_ERR(BSD_ENOTDIR);
    }

    *out = current;
    return SYS_OK;
}

static sys_status_t lookup_parent(struct bsd_proc *proc, const char *path,
                                  vnode_t **parent_out, char *name_out)
{
    if (path == NULL || parent_out == NULL || name_out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    size_t length = strlen(path);

    if (length == 0) {
        return BSD_ERR(BSD_ENOENT);
    }
    if (length >= BSD_PATH_MAX) {
        return BSD_ERR(BSD_ENAMETOOLONG);
    }
    if (path[length - 1] == '/') {
        return BSD_ERR(BSD_EINVAL);
    }

    const char *slash = NULL;

    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            slash = cursor;
        }
    }

    vnode_t *parent = NULL;

    if (slash == NULL) {
        if (proc == NULL) {
            return BSD_ERR(BSD_ENOENT);
        }

        uint64_t flags = irq_save();
        vnode_t *cwd = proc->cwd;

        if (cwd == NULL) {
            irq_restore(flags);
            return BSD_ERR(BSD_ENOENT);
        }
        parent = vfs_vnode_ref(cwd);
        irq_restore(flags);
    } else {
        char directory[BSD_PATH_MAX];
        size_t directory_len = (size_t)(slash - path);

        if (directory_len == 0) {
            directory[0] = '/';
            directory[1] = '\0';
        } else {
            memcpy(directory, path, directory_len);
            directory[directory_len] = '\0';
        }

        sys_status_t status = lookup_path(proc, directory, &parent);

        if (status != SYS_OK) {
            return status;
        }
    }

    if (parent->type != VNODE_DIR) {
        vfs_vnode_unref(parent);
        return BSD_ERR(BSD_ENOTDIR);
    }

    const char *name = slash != NULL ? slash + 1 : path;
    size_t name_len = strlen(name);

    if (name_len == 0 || name_len > BSD_NAME_MAX) {
        vfs_vnode_unref(parent);
        return BSD_ERR(BSD_ENAMETOOLONG);
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        vfs_vnode_unref(parent);
        return BSD_ERR(BSD_EINVAL);
    }

    memcpy(name_out, name, name_len + 1);
    *parent_out = parent;
    return SYS_OK;
}

sys_status_t vfs_lookup(struct bsd_proc *proc, const char *path,
                        vnode_t **out)
{
    return lookup_path(proc, path, out);
}

sys_status_t vfs_mount(const char *path, vnode_t *mount_vnode)
{
    if (path == NULL || mount_vnode == NULL || path[0] != '/') {
        return BSD_ERR(BSD_EINVAL);
    }

    size_t length = strlen(path);
    if (length >= BSD_PATH_MAX) {
        return BSD_ERR(BSD_ENAMETOOLONG);
    }

    if (strcmp(path, "/") == 0) {
        return vfs_set_root(mount_vnode);
    }
    if (path[length - 1] == '/') {
        return BSD_ERR(BSD_EINVAL);
    }

    const char *slash = NULL;
    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            slash = cursor;
        }
    }
    if (slash == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    const char *name = slash + 1;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > BSD_NAME_MAX) {
        return BSD_ERR(BSD_EINVAL);
    }

    char parent_path[BSD_PATH_MAX];
    size_t parent_len = (size_t)(slash - path);

    if (parent_len == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
    }

    vnode_t *parent = NULL;
    sys_status_t status = lookup_path(NULL, parent_path, &parent);
    if (status != SYS_OK) {
        return status;
    }
    if (parent->type != VNODE_DIR) {
        vfs_vnode_unref(parent);
        return BSD_ERR(BSD_ENOTDIR);
    }

    vfs_mount_entry_t *slot = NULL;

    uint64_t flags = irq_save();
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        vfs_mount_entry_t *entry = &vfs_mounts[i];

        if (entry->used && entry->parent == parent &&
            strcmp(entry->name, name) == 0) {
            irq_restore(flags);
            vfs_vnode_unref(parent);
            return BSD_ERR(BSD_EEXIST);
        }
        if (!entry->used && slot == NULL) {
            slot = entry;
        }
    }

    if (slot != NULL) {
        slot->used = true;
        slot->parent = parent;
        slot->child = mount_vnode;
        memcpy(slot->name, name, name_len + 1);

        if (mount_vnode->name == NULL) {
            mount_vnode->name = slot->name;
        }
    }
    irq_restore(flags);

    if (slot == NULL) {
        vfs_vnode_unref(parent);
        return BSD_ERR(BSD_ENOSPC);
    }

    /* Mount tables require permanent parent/mounted nodes, so the borrowed
     * parent link for ".." traversal is safe. */
    if (mount_vnode->parent == NULL) {
        mount_vnode->parent = parent;
    }
    vfs_vnode_unref(parent);

    return SYS_OK;
}

sys_status_t vfs_open(struct bsd_proc *proc, const char *path, uint32_t flags,
                      uint32_t mode, file_t **out)
{
    if (path == NULL || out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if ((flags & BSD_O_ACCMODE) > BSD_O_RDWR) {
        return BSD_ERR(BSD_EINVAL);
    }
    if ((flags & (BSD_O_CREAT | BSD_O_DIRECTORY)) ==
        (BSD_O_CREAT | BSD_O_DIRECTORY)) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint32_t access = flags & BSD_O_ACCMODE;
    vnode_t *vnode = NULL;

    sys_status_t status = lookup_path(proc, path, &vnode);

    if (status == BSD_ERR(BSD_ENOENT) && (flags & BSD_O_CREAT) != 0) {
        vnode_t *parent = NULL;
        char name[BSD_NAME_MAX + 1];

        status = lookup_parent(proc, path, &parent, name);
        if (status != SYS_OK) {
            return status;
        }

        if (parent->ops == NULL || parent->ops->create == NULL) {
            vfs_vnode_unref(parent);
            return BSD_ERR(BSD_EACCES);
        }

        status = parent->ops->create(parent, name, mode, &vnode);
        vfs_vnode_unref(parent);

        if (status == BSD_ERR(BSD_EEXIST)) {
            /* Another CPU won the create race; open the winner. */
            status = lookup_path(proc, path, &vnode);
        }
        if (status != SYS_OK) {
            return status;
        }
    } else if (status != SYS_OK) {
        return status;
    }

    if ((flags & BSD_O_TRUNC) != 0 && access != BSD_O_RDONLY &&
        vnode->type != VNODE_DIR) {
        if (vnode->ops == NULL || vnode->ops->truncate == NULL) {
            vfs_vnode_unref(vnode);
            return BSD_ERR(BSD_EACCES);
        }

        status = vnode->ops->truncate(vnode);
        if (status != SYS_OK) {
            vfs_vnode_unref(vnode);
            return status;
        }
    }

    if (vnode->type == VNODE_DIR && access != BSD_O_RDONLY) {
        vfs_vnode_unref(vnode);
        return BSD_ERR(BSD_EISDIR);
    }
    if ((flags & BSD_O_DIRECTORY) != 0 && vnode->type != VNODE_DIR) {
        vfs_vnode_unref(vnode);
        return BSD_ERR(BSD_ENOTDIR);
    }

    file_t *file = (file_t *)kalloc(sizeof(file_t));
    if (file == NULL) {
        vfs_vnode_unref(vnode);
        return BSD_ERR(BSD_ENOMEM);
    }

    spinlock_init(&file->lock);
    file->vnode = vnode;
    file->offset = 0;
    file->flags = flags;
    file->ref_count = 1;

    *out = file;
    return SYS_OK;
}

sys_status_t vfs_mkdir(struct bsd_proc *proc, const char *path, uint32_t mode)
{
    vnode_t *parent = NULL;
    char name[BSD_NAME_MAX + 1];

    sys_status_t status = lookup_parent(proc, path, &parent, name);

    if (status != SYS_OK) {
        return status;
    }
    if (parent->ops == NULL || parent->ops->mkdir == NULL) {
        vfs_vnode_unref(parent);
        return BSD_ERR(BSD_EACCES);
    }

    vnode_t *created = NULL;
    status = parent->ops->mkdir(parent, name, mode, &created);

    if (created != NULL) {
        vfs_vnode_unref(created);
    }
    vfs_vnode_unref(parent);
    return status;
}

sys_status_t vfs_rmdir(struct bsd_proc *proc, const char *path)
{
    vnode_t *parent = NULL;
    char name[BSD_NAME_MAX + 1];

    sys_status_t status = lookup_parent(proc, path, &parent, name);

    if (status != SYS_OK) {
        return status;
    }
    if (parent->ops == NULL || parent->ops->rmdir == NULL) {
        vfs_vnode_unref(parent);
        return BSD_ERR(BSD_EACCES);
    }

    status = parent->ops->rmdir(parent, name);
    vfs_vnode_unref(parent);
    return status;
}

sys_status_t vfs_unlink(struct bsd_proc *proc, const char *path)
{
    vnode_t *parent = NULL;
    char name[BSD_NAME_MAX + 1];

    sys_status_t status = lookup_parent(proc, path, &parent, name);

    if (status != SYS_OK) {
        return status;
    }
    if (parent->ops == NULL || parent->ops->unlink == NULL) {
        vfs_vnode_unref(parent);
        return BSD_ERR(BSD_EACCES);
    }

    status = parent->ops->unlink(parent, name);
    vfs_vnode_unref(parent);
    return status;
}

sys_status_t vfs_read(file_t *file, void *buf, size_t len, size_t *done)
{
    if (file == NULL || done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    *done = 0;

    if ((file->flags & BSD_O_ACCMODE) == BSD_O_WRONLY) {
        return BSD_ERR(BSD_EBADF);
    }

    vnode_t *vnode = file->vnode;
    if (vnode == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (vnode->type == VNODE_DIR) {
        return BSD_ERR(BSD_EISDIR);
    }
    if (vnode->ops == NULL || vnode->ops->read == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    bool serialized = vnode->type == VNODE_REG;

    if (serialized) {
        spin_lock(&file->lock);
    }

    uint64_t offset = file->offset;
    size_t transferred = 0;
    sys_status_t status =
        vnode->ops->read(vnode, buf, len, offset, &transferred);

    if (status == SYS_OK && transferred > len) {
        status = BSD_ERR(BSD_EIO);
    }
    if (status == SYS_OK && serialized) {
        if (offset > UINT64_MAX - transferred) {
            status = BSD_ERR(BSD_ERANGE);
        } else {
            file->offset = offset + transferred;
        }
    }

    if (serialized) {
        spin_unlock(&file->lock);
    }

    if (status != SYS_OK) {
        return status;
    }

    *done = transferred;
    return SYS_OK;
}

sys_status_t vfs_write(file_t *file, const void *buf, size_t len,
                       size_t *done)
{
    if (file == NULL || done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    *done = 0;

    if ((file->flags & BSD_O_ACCMODE) == BSD_O_RDONLY) {
        return BSD_ERR(BSD_EBADF);
    }

    vnode_t *vnode = file->vnode;
    if (vnode == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (vnode->type == VNODE_DIR) {
        return BSD_ERR(BSD_EISDIR);
    }
    if (vnode->ops == NULL || vnode->ops->write == NULL) {
        return BSD_ERR(BSD_EACCES);
    }

    bool serialized = vnode->type == VNODE_REG;

    if (serialized) {
        spin_lock(&file->lock);
    }

    uint64_t offset = file->offset;
    sys_status_t status = SYS_OK;

    if (serialized && (file->flags & BSD_O_APPEND) != 0) {
        if (vnode->ops->get_size != NULL) {
            uint64_t size = 0;

            status = vnode->ops->get_size(vnode, &size);
            if (status == SYS_OK) {
                offset = size;
                vnode->size = size;
            }
        } else {
            offset = vnode->size;
        }
    }

    size_t transferred = 0;

    if (status == SYS_OK) {
        status = vnode->ops->write(vnode, buf, len, offset, &transferred);
    }

    if (status == SYS_OK && transferred > len) {
        status = BSD_ERR(BSD_EIO);
    }
    if (status == SYS_OK && serialized) {
        if (offset > UINT64_MAX - transferred) {
            status = BSD_ERR(BSD_ERANGE);
        } else {
            file->offset = offset + transferred;
        }
    }

    if (serialized) {
        spin_unlock(&file->lock);
    }

    if (status != SYS_OK) {
        return status;
    }

    *done = transferred;
    return SYS_OK;
}

sys_status_t vfs_lseek(file_t *file, int64_t offset, uint32_t whence,
                       uint64_t *result)
{
    if (file == NULL || result == NULL || file->vnode == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (file->vnode->type != VNODE_REG) {
        return BSD_ERR(BSD_ESPIPE);
    }

    vnode_t *vnode = file->vnode;
    spin_lock(&file->lock);

    uint64_t current = file->offset;
    uint64_t size = vnode->size;
    sys_status_t status = SYS_OK;

    if (whence == BSD_SEEK_END && vnode->ops != NULL &&
        vnode->ops->get_size != NULL) {
        status = vnode->ops->get_size(vnode, &size);
        if (status == SYS_OK) {
            vnode->size = size;
        }
    }

    int64_t base;

    if (status == SYS_OK) {
        switch (whence) {
        case BSD_SEEK_SET:
            base = 0;
            break;
        case BSD_SEEK_CUR:
            base = (int64_t)current;
            break;
        case BSD_SEEK_END:
            base = (int64_t)size;
            break;
        default:
            status = BSD_ERR(BSD_EINVAL);
            base = 0;
            break;
        }
    } else {
        base = 0;
    }

    int64_t target = 0;

    if (status == SYS_OK) {
        if (offset > 0 && base > INT64_MAX - offset) {
            status = BSD_ERR(BSD_ERANGE);
        } else {
            target = base + offset;
            if (target < 0) {
                status = BSD_ERR(BSD_EINVAL);
            }
        }
    }

    if (status == SYS_OK) {
        file->offset = (uint64_t)target;
        *result = (uint64_t)target;
    }

    spin_unlock(&file->lock);

    return status;
}

static void vfs_fill_stat(vnode_t *vnode, bsd_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = vnode->mode;
    st->st_nlink = vnode->type == VNODE_DIR ? 2U : 1U;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_ino = vnode->ino;
    st->st_size = vnode->size;
    st->st_blksize = 512;
    st->st_blocks = (vnode->size + 511ULL) / 512ULL;
}

sys_status_t vfs_fstat(file_t *file, bsd_stat_t *st)
{
    if (file == NULL || st == NULL || file->vnode == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    vfs_fill_stat(file->vnode, st);
    return SYS_OK;
}

sys_status_t vfs_stat_path(struct bsd_proc *proc, const char *path,
                           bsd_stat_t *st)
{
    if (st == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    vnode_t *vnode = NULL;
    sys_status_t status = lookup_path(proc, path, &vnode);

    if (status != SYS_OK) {
        return status;
    }

    vfs_fill_stat(vnode, st);
    vfs_vnode_unref(vnode);
    return SYS_OK;
}

static uint8_t vfs_dirent_type(vnode_type_t type)
{
    switch (type) {
    case VNODE_DIR:
        return BSD_DT_DIR;
    case VNODE_REG:
        return BSD_DT_REG;
    default:
        return BSD_DT_CHR;
    }
}

static void vfs_dirent_set(bsd_dirent_t *entry, const char *name, uint64_t ino,
                           uint8_t type)
{
    memset(entry, 0, sizeof(*entry));

    size_t length = strlen(name);
    if (length > BSD_NAME_MAX) {
        length = BSD_NAME_MAX;
    }

    memcpy(entry->d_name, name, length);
    entry->d_name[length] = '\0';
    entry->d_namlen = (uint8_t)length;
    entry->d_ino = ino;
    entry->d_type = type;
    entry->d_reclen = (uint16_t)sizeof(*entry);
}

static bool vfs_mount_has_name(vnode_t *dir, const char *name)
{
    bool found = false;

    uint64_t flags = irq_save();
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        vfs_mount_entry_t *entry = &vfs_mounts[i];

        if (entry->used && entry->parent == dir &&
            strcmp(entry->name, name) == 0) {
            found = true;
            break;
        }
    }
    irq_restore(flags);

    return found;
}

/* Enumerates "." and "..", then mount-table children, then filesystem
 * children. The filesystem index excludes the two synthetic entries. */
static sys_status_t vfs_readdir_entry(vnode_t *dir, uint64_t index,
                                      bsd_dirent_t *entry)
{
    if (dir == NULL || entry == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (dir->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }

    if (index == 0) {
        vfs_dirent_set(entry, ".", dir->ino, vfs_dirent_type(dir->type));
        return SYS_OK;
    }
    if (index == 1) {
        vnode_t *parent = dir->parent != NULL ? dir->parent : dir;

        vfs_dirent_set(entry, "..", parent->ino,
                       vfs_dirent_type(parent->type));
        return SYS_OK;
    }

    index -= 2;

    uint64_t flags = irq_save();
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        vfs_mount_entry_t *mount = &vfs_mounts[i];

        if (!mount->used || mount->parent != dir) {
            continue;
        }

        if (index == 0) {
            char name[BSD_NAME_MAX + 1];
            memcpy(name, mount->name, sizeof(name));

            uint64_t ino = mount->child->ino;
            uint8_t type = vfs_dirent_type(mount->child->type);

            irq_restore(flags);
            vfs_dirent_set(entry, name, ino, type);
            return SYS_OK;
        }
        index--;
    }
    irq_restore(flags);

    if (dir->ops != NULL && dir->ops->readdir != NULL) {
        for (uint32_t guard = 0; guard < 1024U; guard++) {
            sys_status_t status = dir->ops->readdir(dir, index, entry);

            if (status != SYS_OK) {
                return status;
            }
            if (!vfs_mount_has_name(dir, entry->d_name)) {
                return SYS_OK;
            }
            index++;
        }
    }

    return BSD_ERR(BSD_ENOENT);
}

sys_status_t vfs_getdents(file_t *file, void *buf, size_t len, size_t *done)
{
    if (file == NULL || done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    *done = 0;

    if ((file->flags & BSD_O_ACCMODE) == BSD_O_WRONLY) {
        return BSD_ERR(BSD_EBADF);
    }

    vnode_t *vnode = file->vnode;
    if (vnode == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (vnode->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }
    if (len < sizeof(bsd_dirent_t)) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint8_t *out = (uint8_t *)buf;
    size_t used = 0;

    spin_lock(&file->lock);

    while (used + sizeof(bsd_dirent_t) <= len) {
        bsd_dirent_t entry;
        sys_status_t status = vfs_readdir_entry(vnode, file->offset, &entry);

        if (status == BSD_ERR(BSD_ENOENT)) {
            break;
        }
        if (status != SYS_OK) {
            spin_unlock(&file->lock);
            return status;
        }

        memcpy(out + used, &entry, sizeof(entry));
        used += sizeof(entry);
        file->offset++;
    }

    spin_unlock(&file->lock);

    *done = used;
    return SYS_OK;
}

sys_status_t vfs_chdir(struct bsd_proc *proc, const char *path)
{
    if (proc == NULL || path == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    vnode_t *dir = NULL;
    sys_status_t status = lookup_path(proc, path, &dir);

    if (status != SYS_OK) {
        return status;
    }
    if (dir->type != VNODE_DIR) {
        vfs_vnode_unref(dir);
        return BSD_ERR(BSD_ENOTDIR);
    }

    uint64_t flags = irq_save();
    vnode_t *previous = proc->cwd;
    proc->cwd = dir;
    irq_restore(flags);

    if (previous != NULL) {
        vfs_vnode_unref(previous);
    }
    return SYS_OK;
}

sys_status_t vfs_getcwd(struct bsd_proc *proc, char *buf, size_t size,
                        size_t *len_out)
{
    if (proc == NULL || buf == NULL || len_out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint64_t flags = irq_save();
    vnode_t *cwd = proc->cwd != NULL ? vfs_vnode_ref(proc->cwd) : NULL;
    irq_restore(flags);

    if (cwd == NULL) {
        return BSD_ERR(BSD_ENOENT);
    }

    char path[BSD_PATH_MAX];
    size_t position = sizeof(path);
    vnode_t *cursor = cwd;
    sys_status_t status = SYS_OK;

    while (cursor->parent != NULL) {
        const char *name = cursor->name;

        if (name == NULL || name[0] == '\0') {
            status = BSD_ERR(BSD_EINVAL);
            break;
        }

        size_t length = strlen(name);
        if (position < length + 1) {
            status = BSD_ERR(BSD_ENAMETOOLONG);
            break;
        }

        position -= length;
        memcpy(path + position, name, length);
        position--;
        path[position] = '/';
        cursor = cursor->parent;
    }

    if (status == SYS_OK) {
        if (position == sizeof(path)) {
            position = sizeof(path) - 1;
            path[position] = '/';
        }

        size_t length = sizeof(path) - position;
        if (length + 1 > size) {
            status = BSD_ERR(BSD_ERANGE);
        } else {
            memcpy(buf, path + position, length);
            buf[length] = '\0';
            *len_out = length;
        }
    }

    vfs_vnode_unref(cwd);
    return status;
}

int32_t bsd_fd_alloc(struct bsd_proc *proc, file_t *file)
{
    if (proc == NULL || file == NULL) {
        return (int32_t)BSD_ERR(BSD_EINVAL);
    }

    uint64_t flags = irq_save();

    for (int32_t fd = 0; fd < BSD_MAX_FDS; fd++) {
        if (proc->fd_table[fd] == NULL) {
            proc->fd_table[fd] = file;
            irq_restore(flags);
            return fd;
        }
    }

    irq_restore(flags);
    return (int32_t)BSD_ERR(BSD_EMFILE);
}

sys_status_t bsd_fd_get(struct bsd_proc *proc, int32_t fd, file_t **out)
{
    if (proc == NULL || out == NULL || fd < 0 || fd >= BSD_MAX_FDS) {
        return BSD_ERR(BSD_EBADF);
    }

    uint64_t flags = irq_save();
    file_t *file = proc->fd_table[fd];

    if (file == NULL) {
        irq_restore(flags);
        return BSD_ERR(BSD_EBADF);
    }

    vfs_file_ref(file);
    irq_restore(flags);

    *out = file;
    return SYS_OK;
}

sys_status_t bsd_fd_close(struct bsd_proc *proc, int32_t fd)
{
    if (proc == NULL || fd < 0 || fd >= BSD_MAX_FDS) {
        return BSD_ERR(BSD_EBADF);
    }

    uint64_t flags = irq_save();
    file_t *file = proc->fd_table[fd];

    if (file == NULL) {
        irq_restore(flags);
        return BSD_ERR(BSD_EBADF);
    }

    proc->fd_table[fd] = NULL;
    irq_restore(flags);

    vfs_file_unref(file);
    return SYS_OK;
}

void bsd_fd_close_all(struct bsd_proc *proc)
{
    if (proc == NULL) {
        return;
    }

    file_t *files[BSD_MAX_FDS];

    uint64_t flags = irq_save();
    for (size_t i = 0; i < BSD_MAX_FDS; i++) {
        files[i] = proc->fd_table[i];
        proc->fd_table[i] = NULL;
    }
    irq_restore(flags);

    for (size_t i = 0; i < BSD_MAX_FDS; i++) {
        if (files[i] != NULL) {
            vfs_file_unref(files[i]);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Pipes                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct bsd_pipe {
    vnode_t vnode;
    struct bsd_pipe *next;
    uint8_t buffer[BSD_PIPE_CAPACITY];
    size_t  head;
    size_t  tail;
    size_t  count;
    uint32_t readers;
    uint32_t writers;
    thread_t *read_waiters;
    thread_t *read_tail;
    thread_t *write_waiters;
    thread_t *write_tail;
} bsd_pipe_t;

_Static_assert(offsetof(bsd_pipe_t, vnode) == 0,
               "bsd_pipe_t must begin with its vnode");

static uint32_t pipe_live_count;
static uint64_t pipe_next_ino = 16;
static bsd_pipe_t *pipe_list;

static sys_status_t pipe_read(vnode_t *vnode, void *buf, size_t len,
                              uint64_t offset, size_t *done);
static sys_status_t pipe_write(vnode_t *vnode, const void *buf, size_t len,
                               uint64_t offset, size_t *done);
static void pipe_close(vnode_t *vnode, file_t *file);
static void pipe_release(vnode_t *vnode);

static vnode_ops_t pipe_vnode_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .lookup = NULL,
    .readdir = NULL,
    .close = pipe_close,
    .release = pipe_release,
};

static file_t *vfs_file_alloc(vnode_t *vnode, uint32_t flags)
{
    if (vnode == NULL) {
        return NULL;
    }

    file_t *file = (file_t *)kalloc(sizeof(file_t));
    if (file == NULL) {
        return NULL;
    }

    spinlock_init(&file->lock);
    file->vnode = vfs_vnode_ref(vnode);
    file->offset = 0;
    file->flags = flags;
    file->ref_count = 1;
    return file;
}

/* Wait-queue helpers run with the pipe lock held (interrupts disabled). */
static void pipe_enqueue(thread_t **head, thread_t **tail, thread_t *thread)
{
    thread->wait_next = NULL;

    if (*tail != NULL) {
        (*tail)->wait_next = thread;
    } else {
        *head = thread;
    }
    *tail = thread;
}

static thread_t *pipe_dequeue(thread_t **head, thread_t **tail)
{
    thread_t *thread = *head;

    if (thread == NULL) {
        return NULL;
    }

    *head = thread->wait_next;
    if (*head == NULL) {
        *tail = NULL;
    }
    thread->wait_next = NULL;
    return thread;
}

/* Remove a specific thread that may still be linked after a signal wakeup. */
static void pipe_remove_waiter(thread_t **head, thread_t **tail,
                               thread_t *thread)
{
    thread_t *previous = NULL;
    thread_t *current = *head;

    while (current != NULL) {
        if (current == thread) {
            if (previous != NULL) {
                previous->wait_next = current->wait_next;
            } else {
                *head = current->wait_next;
            }
            if (*tail == current) {
                *tail = previous;
            }
            current->wait_next = NULL;
            return;
        }
        previous = current;
        current = current->wait_next;
    }
}

/* Process teardown must not leave dying threads in any pipe wait queue. */
static void pipe_remove_task_waiters(thread_t **head, thread_t **tail,
                                     task_t *task)
{
    thread_t *previous = NULL;
    thread_t *current = *head;

    while (current != NULL) {
        thread_t *next = current->wait_next;

        if (current->owner_task == task) {
            if (previous != NULL) {
                previous->wait_next = next;
            } else {
                *head = next;
            }
            if (*tail == current) {
                *tail = previous;
            }
            current->wait_next = NULL;
        } else {
            previous = current;
        }
        current = next;
    }
}

static void pipe_wake_all(thread_t **head, thread_t **tail)
{
    thread_t *thread;

    while ((thread = pipe_dequeue(head, tail)) != NULL) {
        sched_wake(thread);
    }
}

static sys_status_t pipe_read(vnode_t *vnode, void *buf, size_t len,
                              uint64_t offset, size_t *done)
{
    UNUSED(offset);

    if (vnode == NULL || done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    bsd_pipe_t *pipe = (bsd_pipe_t *)vnode;
    uint8_t *out = (uint8_t *)buf;
    bsd_proc_t *proc = bsd_proc_current();

    *done = 0;
    if (len == 0) {
        return SYS_OK;
    }

    for (;;) {
        uint64_t flags = irq_save();

        if (pipe->count > 0) {
            size_t count = len < pipe->count ? len : pipe->count;

            for (size_t i = 0; i < count; i++) {
                out[i] = pipe->buffer[pipe->tail];
                pipe->tail = (pipe->tail + 1) % BSD_PIPE_CAPACITY;
            }
            pipe->count -= count;

            thread_t *writer =
                pipe_dequeue(&pipe->write_waiters, &pipe->write_tail);
            if (writer != NULL) {
                sched_wake(writer);
            }

            irq_restore(flags);
            *done = count;
            return SYS_OK;
        }

        if (pipe->writers == 0) {
            irq_restore(flags);
            return SYS_OK;
        }

        if (proc != NULL && bsd_proc_has_pending(proc)) {
            irq_restore(flags);
            return BSD_ERR(BSD_EINTR);
        }

        thread_t *self = sched_current_thread();
        if (proc != NULL) {
            proc->signal_waiter = self;
        }
        self->state = THREAD_BLOCKED_RECV;
        pipe_enqueue(&pipe->read_waiters, &pipe->read_tail, self);
        irq_restore(flags);
        sched_block_current();

        flags = irq_save();
        if (proc != NULL && proc->signal_waiter == self) {
            proc->signal_waiter = NULL;
        }
        pipe_remove_waiter(&pipe->read_waiters, &pipe->read_tail, self);
        irq_restore(flags);

        if (proc != NULL && bsd_proc_has_pending(proc)) {
            return BSD_ERR(BSD_EINTR);
        }
    }
}

static sys_status_t pipe_write(vnode_t *vnode, const void *buf, size_t len,
                               uint64_t offset, size_t *done)
{
    UNUSED(offset);

    if (vnode == NULL || done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    bsd_pipe_t *pipe = (bsd_pipe_t *)vnode;
    const uint8_t *in = (const uint8_t *)buf;
    bsd_proc_t *proc = bsd_proc_current();
    size_t written = 0;

    *done = 0;
    if (len == 0) {
        return SYS_OK;
    }

    while (written < len) {
        uint64_t flags = irq_save();

        if (pipe->readers == 0) {
            irq_restore(flags);
            *done = written;
            return written > 0 ? SYS_OK : BSD_ERR(BSD_EPIPE);
        }

        if (pipe->count < BSD_PIPE_CAPACITY) {
            size_t space = BSD_PIPE_CAPACITY - pipe->count;
            size_t remaining = len - written;
            size_t count = remaining < space ? remaining : space;

            for (size_t i = 0; i < count; i++) {
                pipe->buffer[pipe->head] = in[written + i];
                pipe->head = (pipe->head + 1) % BSD_PIPE_CAPACITY;
            }
            pipe->count += count;
            written += count;

            thread_t *reader =
                pipe_dequeue(&pipe->read_waiters, &pipe->read_tail);
            if (reader != NULL) {
                sched_wake(reader);
            }

            irq_restore(flags);
            continue;
        }

        if (proc != NULL && bsd_proc_has_pending(proc)) {
            irq_restore(flags);
            *done = written;
            return written > 0 ? SYS_OK : BSD_ERR(BSD_EINTR);
        }

        thread_t *self = sched_current_thread();
        if (proc != NULL) {
            proc->signal_waiter = self;
        }
        self->state = THREAD_BLOCKED_SEND;
        pipe_enqueue(&pipe->write_waiters, &pipe->write_tail, self);
        irq_restore(flags);
        sched_block_current();

        flags = irq_save();
        if (proc != NULL && proc->signal_waiter == self) {
            proc->signal_waiter = NULL;
        }
        pipe_remove_waiter(&pipe->write_waiters, &pipe->write_tail, self);
        irq_restore(flags);

        if (proc != NULL && bsd_proc_has_pending(proc)) {
            *done = written;
            return written > 0 ? SYS_OK : BSD_ERR(BSD_EINTR);
        }
    }

    *done = written;
    return SYS_OK;
}

static void pipe_close(vnode_t *vnode, file_t *file)
{
    bsd_pipe_t *pipe = (bsd_pipe_t *)vnode;
    uint32_t access = file->flags & BSD_O_ACCMODE;

    uint64_t flags = irq_save();

    if (access != BSD_O_WRONLY && pipe->readers > 0) {
        pipe->readers--;
    }
    if (access != BSD_O_RDONLY && pipe->writers > 0) {
        pipe->writers--;
    }

    pipe_wake_all(&pipe->read_waiters, &pipe->read_tail);
    pipe_wake_all(&pipe->write_waiters, &pipe->write_tail);

    irq_restore(flags);
}

static void pipe_release(vnode_t *vnode)
{
    bsd_pipe_t *pipe = (bsd_pipe_t *)vnode;

    uint64_t flags = irq_save();

    bsd_pipe_t **link = &pipe_list;
    while (*link != NULL && *link != pipe) {
        link = &(*link)->next;
    }
    if (*link == pipe) {
        *link = pipe->next;
    }
    pipe->next = NULL;

    pipe_wake_all(&pipe->read_waiters, &pipe->read_tail);
    pipe_wake_all(&pipe->write_waiters, &pipe->write_tail);
    if (pipe_live_count > 0) {
        pipe_live_count--;
    }
    irq_restore(flags);

    kfree(pipe);
}

int32_t vfs_dup(struct bsd_proc *proc, int32_t oldfd)
{
    file_t *file = NULL;
    sys_status_t status = bsd_fd_get(proc, oldfd, &file);

    if (status != SYS_OK) {
        return (int32_t)status;
    }

    int32_t newfd = bsd_fd_alloc(proc, file);
    if (newfd < 0) {
        vfs_file_unref(file);
    }

    return newfd;
}

int32_t vfs_dup2(struct bsd_proc *proc, int32_t oldfd, int32_t newfd)
{
    if (proc == NULL || newfd < 0 || newfd >= BSD_MAX_FDS) {
        return (int32_t)BSD_ERR(BSD_EBADF);
    }

    file_t *file = NULL;
    sys_status_t status = bsd_fd_get(proc, oldfd, &file);

    if (status != SYS_OK) {
        return (int32_t)status;
    }

    if (oldfd == newfd) {
        vfs_file_unref(file);
        return newfd;
    }

    uint64_t flags = irq_save();
    file_t *previous = proc->fd_table[newfd];
    proc->fd_table[newfd] = file;
    irq_restore(flags);

    if (previous != NULL) {
        vfs_file_unref(previous);
    }

    return newfd;
}

sys_status_t vfs_pipe(struct bsd_proc *proc, int32_t fds_out[2])
{
    if (proc == NULL || fds_out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    bsd_pipe_t *pipe = (bsd_pipe_t *)kalloc(sizeof(bsd_pipe_t));
    if (pipe == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }
    memset(pipe, 0, sizeof(*pipe));

    pipe->vnode.type = VNODE_CHR;
    pipe->vnode.mode = BSD_S_IFCHR | 0600U;
    pipe->vnode.size = 0;
    pipe->vnode.ref_count = 1;
    pipe->vnode.ops = &pipe_vnode_ops;
    pipe->vnode.fs_data = NULL;
    pipe->vnode.parent = NULL;
    pipe->vnode.name = NULL;

    uint64_t flags = irq_save();
    pipe->vnode.ino = ++pipe_next_ino;
    pipe->next = pipe_list;
    pipe_list = pipe;
    pipe_live_count++;
    irq_restore(flags);

    file_t *read_file = vfs_file_alloc(&pipe->vnode, BSD_O_RDONLY);
    file_t *write_file = vfs_file_alloc(&pipe->vnode, BSD_O_WRONLY);

    if (read_file == NULL || write_file == NULL) {
        if (read_file != NULL) {
            vfs_file_unref(read_file);
        }
        if (write_file != NULL) {
            vfs_file_unref(write_file);
        }
        vfs_vnode_unref(&pipe->vnode);
        return BSD_ERR(BSD_ENOMEM);
    }

    pipe->readers = 1;
    pipe->writers = 1;
    vfs_vnode_unref(&pipe->vnode); /* Drop the creator reference */

    int32_t read_fd = bsd_fd_alloc(proc, read_file);
    if (read_fd < 0) {
        vfs_file_unref(read_file);
        vfs_file_unref(write_file);
        return read_fd;
    }

    int32_t write_fd = bsd_fd_alloc(proc, write_file);
    if (write_fd < 0) {
        (void)bsd_fd_close(proc, read_fd);
        vfs_file_unref(write_file);
        return write_fd;
    }

    fds_out[0] = read_fd;
    fds_out[1] = write_fd;
    return SYS_OK;
}

uint32_t vfs_live_pipe_count(void)
{
    uint64_t flags = irq_save();
    uint32_t count = pipe_live_count;
    irq_restore(flags);
    return count;
}

void vfs_pipe_abort_task(struct task *task)
{
    if (task == NULL) {
        return;
    }

    task_t *owner = (task_t *)task;

    uint64_t flags = irq_save();

    for (bsd_pipe_t *pipe = pipe_list; pipe != NULL; pipe = pipe->next) {
        pipe_remove_task_waiters(&pipe->read_waiters, &pipe->read_tail,
                                 owner);
        pipe_remove_task_waiters(&pipe->write_waiters, &pipe->write_tail,
                                 owner);
    }

    irq_restore(flags);

    bsd_proc_t *proc = bsd_proc_from_task(owner);

    if (proc != NULL) {
        flags = irq_save();
        proc->signal_waiter = NULL;
        irq_restore(flags);
    }
}
