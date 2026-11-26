#include <bsd/bsd.h>
#include <bsd/errno.h>
#include <bsd/vfs.h>
#include <kalloc.h>
#include <klog.h>
#include <string.h>
#include <types.h>

#define TAR_BLOCK        512U
#define TAR_NAME_OFF     0U
#define TAR_NAME_LEN     100U
#define TAR_MODE_OFF     100U
#define TAR_MODE_LEN     8U
#define TAR_SIZE_OFF     124U
#define TAR_SIZE_LEN     12U
#define TAR_CHKSUM_OFF   148U
#define TAR_CHKSUM_LEN   8U
#define TAR_TYPEFLAG_OFF 156U
#define TAR_MAGIC_OFF    257U
#define TAR_MAGIC_LEN    6U
#define TAR_PREFIX_OFF   345U
#define TAR_PREFIX_LEN   155U

typedef struct initrd_node {
    char         path[BSD_PATH_MAX];
    const uint8_t *data;
    uint64_t     size;
    uint64_t     ino;
    uint32_t     mode;
    vnode_type_t type;
} initrd_node_t;

static const uint8_t *initrd_image;
static size_t initrd_image_size;

static sys_status_t initrd_read(vnode_t *vnode, void *buf, size_t len,
                                uint64_t offset, size_t *done);
static sys_status_t initrd_lookup(vnode_t *dir, const char *name,
                                  vnode_t **out);
static sys_status_t initrd_readdir(vnode_t *dir, uint64_t index,
                                   bsd_dirent_t *entry);
static void initrd_release(vnode_t *vnode);

static vnode_ops_t initrd_vnode_ops = {
    .read = initrd_read,
    .write = NULL,
    .lookup = initrd_lookup,
    .readdir = initrd_readdir,
    .close = NULL,
    .release = initrd_release,
};

static bool tar_block_zero(const uint8_t *header)
{
    for (size_t i = 0; i < TAR_BLOCK; i++) {
        if (header[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool tar_parse_octal(const uint8_t *field, size_t len, uint64_t *out)
{
    size_t i = 0;
    bool any = false;
    uint64_t value = 0;

    while (i < len && field[i] == ' ') {
        i++;
    }
    if (i == len || field[i] == '\0') {
        *out = 0;
        return true;
    }

    for (; i < len && field[i] != '\0' && field[i] != ' '; i++) {
        if (field[i] < '0' || field[i] > '7') {
            return false;
        }
        if (value > (UINT64_MAX >> 3)) {
            return false;
        }
        value = (value << 3) | (uint64_t)(field[i] - '0');
        any = true;
    }

    if (!any) {
        *out = 0;
        return true;
    }

    *out = value;
    return true;
}

static bool tar_checksum_ok(const uint8_t *header)
{
    uint64_t stored = 0;

    if (!tar_parse_octal(header + TAR_CHKSUM_OFF, TAR_CHKSUM_LEN, &stored)) {
        return false;
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++) {
        uint8_t byte = header[i];

        if (i >= TAR_CHKSUM_OFF && i < TAR_CHKSUM_OFF + TAR_CHKSUM_LEN) {
            byte = (uint8_t)' ';
        }
        sum += byte;
    }

    return sum == stored;
}

static bool tar_field_copy(char *out, size_t cap, const uint8_t *header,
                           size_t offset, size_t length)
{
    size_t i = 0;

    while (i < length && header[offset + i] != '\0') {
        if (i + 1 >= cap || i + 1 >= length) {
            return false;
        }
        out[i] = (char)header[offset + i];
        i++;
    }
    out[i] = '\0';
    return true;
}

/* POSIX ustar stores "ustar\0" plus version "00"; the older GNU form stores
 * "ustar  \0". Reject everything else so a checksum-valid non-tar blob cannot
 * be parsed as an archive. */
static bool tar_magic_valid(const uint8_t *header)
{
    if (memcmp(header + TAR_MAGIC_OFF, "ustar", TAR_MAGIC_LEN - 1) != 0) {
        return false;
    }

    uint8_t trailer = header[TAR_MAGIC_OFF + TAR_MAGIC_LEN - 1];
    return trailer == '\0' || trailer == ' ';
}

/* Copies the member name (prefix + '/' + name for ustar) and normalizes the
 * leading "./" / "/" plus a trailing directory slash. */
static bool tar_entry_path(const uint8_t *header, char *out, size_t cap)
{
    char name[TAR_NAME_LEN + 1];
    char prefix[TAR_PREFIX_LEN + 1];

    if (!tar_field_copy(name, sizeof(name), header, TAR_NAME_OFF,
                        TAR_NAME_LEN)) {
        return false;
    }

    prefix[0] = '\0';
    if (tar_magic_valid(header) &&
        !tar_field_copy(prefix, sizeof(prefix), header, TAR_PREFIX_OFF,
                        TAR_PREFIX_LEN)) {
        return false;
    }

    size_t out_len = 0;

    if (prefix[0] != '\0') {
        size_t prefix_len = strlen(prefix);
        size_t name_len = strlen(name);

        if (prefix_len + 1 + name_len >= cap) {
            return false;
        }
        memcpy(out, prefix, prefix_len);
        out[prefix_len] = '/';
        memcpy(out + prefix_len + 1, name, name_len + 1);
        out_len = prefix_len + 1 + name_len;
    } else {
        size_t name_len = strlen(name);

        if (name_len >= cap) {
            return false;
        }
        memcpy(out, name, name_len + 1);
        out_len = name_len;
    }

    size_t skip = 0;
    while (skip < out_len && out[skip] == '/') {
        skip++;
    }
    while (skip + 1 < out_len && out[skip] == '.' && out[skip + 1] == '/') {
        skip += 2;
    }
    if (skip > 0) {
        memmove(out, out + skip, out_len - skip + 1);
        out_len -= skip;
    }

    while (out_len > 0 && out[out_len - 1] == '/') {
        out_len--;
    }
    out[out_len] = '\0';

    return out_len > 0;
}

static uint64_t initrd_ino(const char *path)
{
    uint64_t hash = 1469598103934665603ULL;

    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        hash ^= (uint64_t)(uint8_t)*cursor;
        hash *= 1099511628211ULL;
    }

    return hash;
}

static bool compose_child_path(const char *dir_path, const char *name,
                               char *out, size_t cap)
{
    size_t dir_len = strlen(dir_path);
    size_t name_len = strlen(name);

    if (dir_len == 0) {
        if (name_len + 1 > cap) {
            return false;
        }
        memcpy(out, name, name_len + 1);
        return true;
    }

    if (dir_len + 1 + name_len + 1 > cap) {
        return false;
    }

    memcpy(out, dir_path, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, name, name_len);
    out[dir_len + 1 + name_len] = '\0';
    return true;
}

typedef struct tar_child {
    char name[BSD_NAME_MAX + 1];
    bool is_dir;
    bool supported;
} tar_child_t;

/* Extracts the first path component below dir_path from a member. */
static bool tar_direct_child(const uint8_t *header, const char *dir_path,
                             tar_child_t *child)
{
    char path[BSD_PATH_MAX];

    if (!tar_entry_path(header, path, sizeof(path))) {
        return false;
    }

    const char *relative;

    if (dir_path[0] == '\0') {
        relative = path;
    } else {
        size_t dir_len = strlen(dir_path);

        if (strncmp(path, dir_path, dir_len) != 0 || path[dir_len] != '/') {
            return false;
        }
        relative = path + dir_len + 1;
    }

    if (relative[0] == '\0') {
        return false;
    }

    const char *slash = NULL;
    for (const char *cursor = relative; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            slash = cursor;
            break;
        }
    }

    size_t name_len =
        slash != NULL ? (size_t)(slash - relative) : strlen(relative);

    if (name_len == 0 || name_len > BSD_NAME_MAX) {
        return false;
    }

    memcpy(child->name, relative, name_len);
    child->name[name_len] = '\0';

    if (slash != NULL) {
        child->is_dir = true;
        child->supported = true;
        return true;
    }

    uint8_t typeflag = header[TAR_TYPEFLAG_OFF];

    child->is_dir = typeflag == '5';
    child->supported =
        typeflag == '5' || typeflag == '0' || typeflag == '\0';
    return true;
}

static bool initrd_child_seen(const char *dir_path, const char *name,
                              size_t before_offset)
{
    size_t offset = 0;

    while (offset + TAR_BLOCK <= initrd_image_size && offset < before_offset) {
        const uint8_t *header = initrd_image + offset;

        if (tar_block_zero(header)) {
            break;
        }
        if (!tar_checksum_ok(header) || !tar_magic_valid(header)) {
            break;
        }

        tar_child_t child;
        if (tar_direct_child(header, dir_path, &child) && child.supported &&
            strcmp(child.name, name) == 0) {
            return true;
        }

        uint64_t member_size = 0;
        if (!tar_parse_octal(header + TAR_SIZE_OFF, TAR_SIZE_LEN,
                             &member_size)) {
            break;
        }

        size_t data_offset = offset + TAR_BLOCK;
        if (member_size > initrd_image_size - data_offset) {
            break;
        }

        size_t advance = (size_t)ALIGN_UP(member_size, TAR_BLOCK);
        if (advance > initrd_image_size - data_offset) {
            break;
        }
        offset = data_offset + advance;
    }

    return false;
}

static sys_status_t initrd_readdir(vnode_t *dir, uint64_t index,
                                   bsd_dirent_t *entry)
{
    if (dir == NULL || entry == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    initrd_node_t *dir_node = (initrd_node_t *)dir->fs_data;
    if (dir_node == NULL || dir_node->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }

    size_t offset = 0;
    uint64_t seen = 0;

    while (offset + TAR_BLOCK <= initrd_image_size) {
        const uint8_t *header = initrd_image + offset;

        if (tar_block_zero(header)) {
            break;
        }
        if (!tar_checksum_ok(header) || !tar_magic_valid(header)) {
            return BSD_ERR(BSD_EIO);
        }

        tar_child_t child;
        if (tar_direct_child(header, dir_node->path, &child) &&
            child.supported &&
            !initrd_child_seen(dir_node->path, child.name, offset)) {
            if (seen == index) {
                char full_path[BSD_PATH_MAX];

                if (!compose_child_path(dir_node->path, child.name, full_path,
                                        sizeof(full_path))) {
                    return BSD_ERR(BSD_EIO);
                }

                memset(entry, 0, sizeof(*entry));
                size_t name_len = strlen(child.name);
                memcpy(entry->d_name, child.name, name_len);
                entry->d_name[name_len] = '\0';
                entry->d_namlen = (uint8_t)name_len;
                entry->d_ino = initrd_ino(full_path);
                entry->d_type = child.is_dir ? BSD_DT_DIR : BSD_DT_REG;
                entry->d_reclen = (uint16_t)sizeof(*entry);
                return SYS_OK;
            }
            seen++;
        }

        uint64_t member_size = 0;
        if (!tar_parse_octal(header + TAR_SIZE_OFF, TAR_SIZE_LEN,
                             &member_size)) {
            return BSD_ERR(BSD_EIO);
        }

        size_t data_offset = offset + TAR_BLOCK;
        if (member_size > initrd_image_size - data_offset) {
            return BSD_ERR(BSD_EIO);
        }

        size_t advance = (size_t)ALIGN_UP(member_size, TAR_BLOCK);
        if (advance > initrd_image_size - data_offset) {
            return BSD_ERR(BSD_EIO);
        }
        offset = data_offset + advance;
    }

    return BSD_ERR(BSD_ENOENT);
}

static bool entry_matches(const char *entry_path, const char *dir_path,
                          const char *name, bool *exact)
{
    const char *relative;

    if (dir_path[0] == '\0') {
        relative = entry_path;
    } else {
        size_t dir_len = strlen(dir_path);

        if (strncmp(entry_path, dir_path, dir_len) != 0 ||
            entry_path[dir_len] != '/') {
            return false;
        }
        relative = entry_path + dir_len + 1;
    }

    size_t name_len = strlen(name);

    if (strncmp(relative, name, name_len) != 0) {
        return false;
    }
    if (relative[name_len] == '\0') {
        *exact = true;
        return true;
    }
    if (relative[name_len] == '/') {
        *exact = false;
        return true;
    }

    return false;
}

typedef struct initrd_match {
    bool         found;
    vnode_type_t type;
    uint32_t     mode;
    uint64_t     size;
    uint64_t     ino;
    const uint8_t *data;
    char         path[BSD_PATH_MAX];
} initrd_match_t;

static bool initrd_scan(const char *dir_path, const char *name,
                        initrd_match_t *match)
{
    size_t offset = 0;

    while (offset + TAR_BLOCK <= initrd_image_size) {
        const uint8_t *header = initrd_image + offset;

        if (tar_block_zero(header)) {
            break;
        }
        if (!tar_checksum_ok(header)) {
            return false;
        }
        if (!tar_magic_valid(header)) {
            return false;
        }

        char entry_path[BSD_PATH_MAX];
        bool path_ok = tar_entry_path(header, entry_path, sizeof(entry_path));

        uint64_t member_size = 0;
        if (!tar_parse_octal(header + TAR_SIZE_OFF, TAR_SIZE_LEN,
                             &member_size)) {
            return false;
        }

        size_t data_offset = offset + TAR_BLOCK;
        if (member_size > initrd_image_size - data_offset) {
            return false;
        }

        bool exact = false;
        if (path_ok && entry_matches(entry_path, dir_path, name, &exact)) {
            uint8_t typeflag = header[TAR_TYPEFLAG_OFF];
            uint64_t mode_bits = 0;
            if (!tar_parse_octal(header + TAR_MODE_OFF, TAR_MODE_LEN,
                                 &mode_bits)) {
                return false;
            }

            bool supported = true;

            if (!exact) {
                match->type = VNODE_DIR;
                match->mode = BSD_S_IFDIR | 0555U;
                match->size = 0;
                match->data = NULL;
            } else if (typeflag == '5') {
                match->type = VNODE_DIR;
                match->mode = BSD_S_IFDIR | (uint32_t)(mode_bits & 0777U);
                match->size = 0;
                match->data = NULL;
            } else if (typeflag == '0' || typeflag == '\0') {
                match->type = VNODE_REG;
                match->mode = BSD_S_IFREG | (uint32_t)(mode_bits & 0777U);
                match->size = member_size;
                match->data = initrd_image + data_offset;
            } else {
                /* Metadata entries (PAX/GNU long names, links) do not name
                 * directories or regular files in this filesystem. */
                supported = false;
            }

            if (supported) {
                if (!compose_child_path(dir_path, name, match->path,
                                        sizeof(match->path))) {
                    return false;
                }
                match->ino = initrd_ino(match->path);
                match->found = true;
                return true;
            }
        }

        size_t advance = (size_t)ALIGN_UP(member_size, TAR_BLOCK);
        if (advance > initrd_image_size - data_offset) {
            return false;
        }
        offset = data_offset + advance;
    }

    return false;
}

static sys_status_t initrd_lookup(vnode_t *dir, const char *name,
                                  vnode_t **out)
{
    if (dir == NULL || out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    initrd_node_t *dir_node = (initrd_node_t *)dir->fs_data;
    if (dir_node == NULL || dir_node->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }
    if (name == NULL || name[0] == '\0') {
        return BSD_ERR(BSD_ENOENT);
    }
    if (strlen(name) > BSD_NAME_MAX) {
        return BSD_ERR(BSD_ENAMETOOLONG);
    }
    for (const char *cursor = name; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            return BSD_ERR(BSD_EINVAL);
        }
    }

    initrd_match_t match;
    memset(&match, 0, sizeof(match));

    if (!initrd_scan(dir_node->path, name, &match) || !match.found) {
        return BSD_ERR(BSD_ENOENT);
    }

    vnode_t *vnode = (vnode_t *)kalloc(sizeof(vnode_t));
    initrd_node_t *node = (initrd_node_t *)kalloc(sizeof(initrd_node_t));

    if (vnode == NULL || node == NULL) {
        if (vnode != NULL) {
            kfree(vnode);
        }
        if (node != NULL) {
            kfree(node);
        }
        return BSD_ERR(BSD_ENOMEM);
    }

    memset(vnode, 0, sizeof(*vnode));
    memset(node, 0, sizeof(*node));

    memcpy(node->path, match.path, sizeof(node->path));
    node->data = match.data;
    node->size = match.size;
    node->ino = match.ino;
    node->mode = match.mode;
    node->type = match.type;

    vnode->type = match.type;
    vnode->mode = match.mode;
    vnode->size = match.size;
    vnode->ino = match.ino;
    vnode->ref_count = 1;
    vnode->ops = &initrd_vnode_ops;
    vnode->fs_data = node;
    vnode->parent = vfs_vnode_ref(dir);

    const char *base = node->path;
    for (const char *cursor = node->path; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            base = cursor + 1;
        }
    }
    vnode->name = base;

    *out = vnode;
    return SYS_OK;
}

static sys_status_t initrd_read(vnode_t *vnode, void *buf, size_t len,
                                uint64_t offset, size_t *done)
{
    if (vnode == NULL || done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    *done = 0;

    initrd_node_t *node = (initrd_node_t *)vnode->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (node->type != VNODE_REG) {
        return node->type == VNODE_DIR ? BSD_ERR(BSD_EISDIR)
                                       : BSD_ERR(BSD_EINVAL);
    }
    if (offset >= node->size || len == 0) {
        return SYS_OK;
    }

    size_t available = (size_t)(node->size - offset);
    size_t count = len < available ? len : available;

    memcpy(buf, node->data + offset, count);
    *done = count;

    return SYS_OK;
}

static void initrd_release(vnode_t *vnode)
{
    vnode_t *parent = vnode->parent;

    kfree(vnode->fs_data);
    vnode->fs_data = NULL;
    vnode->parent = NULL;

    if (parent != NULL) {
        vfs_vnode_unref(parent);
    }
    kfree(vnode);
}

sys_status_t initrd_mount(const void *image, size_t size)
{
    if (image == NULL || size < TAR_BLOCK) {
        return BSD_ERR(BSD_EINVAL);
    }

    const uint8_t *bytes = (const uint8_t *)image;

    if (tar_block_zero(bytes) || !tar_magic_valid(bytes) ||
        !tar_checksum_ok(bytes)) {
        return BSD_ERR(BSD_EINVAL);
    }

    initrd_image = bytes;
    initrd_image_size = size;

    vnode_t *root = (vnode_t *)kalloc(sizeof(vnode_t));
    initrd_node_t *node = (initrd_node_t *)kalloc(sizeof(initrd_node_t));

    if (root == NULL || node == NULL) {
        if (root != NULL) {
            kfree(root);
        }
        if (node != NULL) {
            kfree(node);
        }
        initrd_image = NULL;
        initrd_image_size = 0;
        return BSD_ERR(BSD_ENOMEM);
    }

    memset(root, 0, sizeof(*root));
    memset(node, 0, sizeof(*node));

    node->path[0] = '\0';
    node->data = NULL;
    node->size = 0;
    node->ino = 1;
    node->mode = BSD_S_IFDIR | 0555U;
    node->type = VNODE_DIR;

    root->type = VNODE_DIR;
    root->mode = node->mode;
    root->size = 0;
    root->ino = node->ino;
    root->ref_count = 1;
    root->ops = &initrd_vnode_ops;
    root->fs_data = node;
    root->parent = NULL;
    root->name = "";

    sys_status_t status = vfs_set_root(root);
    if (status != SYS_OK) {
        kfree(node);
        kfree(root);
        initrd_image = NULL;
        initrd_image_size = 0;
        return status;
    }

    klog(KLOG_INFO, "bsd: initrd mounted at / (USTAR, %lu bytes)",
         (unsigned long)size);

    return SYS_OK;
}
