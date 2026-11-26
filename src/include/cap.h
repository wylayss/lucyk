#ifndef LUCYK_CAP_H
#define LUCYK_CAP_H

#include <object.h>
#include <types.h>
#include <vspace.h>

/* Opaque capability handle. Slot 0 never holds a capability. */
typedef uint32_t cap_t;
#define CAP_NULL ((cap_t)0)

#define CAP_TABLE_SLOTS 256

/* Kernel object kinds that may be named by a capability. */
typedef enum cap_obj_type {
    CAP_OBJ_NONE = 0,
    CAP_OBJ_ENDPOINT,
    CAP_OBJ_NOTIFICATION,
    CAP_OBJ_VSPACE,
    CAP_OBJ_IRQ,
    CAP_OBJ_FRAME
} cap_obj_type_t;

#define CAP_RIGHT_READ  (1ULL << 0)
#define CAP_RIGHT_WRITE (1ULL << 1)
#define CAP_RIGHT_CALL  (1ULL << 2)
#define CAP_RIGHT_GRANT (1ULL << 3)
#define CAP_RIGHT_ALL   (CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_CALL | CAP_RIGHT_GRANT)

typedef struct cap_slot {
    mk_object_t   *object;
    uint64_t       rights;
    uint64_t       badge;
    cap_obj_type_t type;
} cap_slot_t;

typedef struct cap_table {
    cap_slot_t slots[CAP_TABLE_SLOTS];
    uint32_t   used;
} cap_table_t;

cap_table_t *cap_table_create(void);

/* Releases every held capability (dropping object references) and frees the
 * table. Blocked IPC participants keep their own object references. */
void cap_table_destroy(cap_table_t *table);

/* Installs a capability and retains its own reference on obj. The caller's
 * reference is unchanged and must be released separately. */
sys_status_t cap_insert(cap_table_t *table, cap_obj_type_t type, void *obj,
                        uint64_t rights, uint64_t badge, cap_t *cap_out);

/* Resolves cap to its object, validating type and rights. The returned object
 * reference is borrowed from the table, not retained. */
sys_status_t cap_lookup(cap_table_t *table, cap_t cap, cap_obj_type_t expected_type,
                        uint64_t required_rights, void **obj_out, uint64_t *badge_out);

/* Like cap_lookup, but retains the object before releasing the table lock.
 * The caller must release it with mk_object_release(). Used by blocking IPC,
 * where the capability table may be mutated while the caller sleeps. */
sys_status_t cap_lookup_retained(cap_table_t *table, cap_t cap,
                                 cap_obj_type_t expected_type,
                                 uint64_t required_rights, void **obj_out,
                                 uint64_t *badge_out);

/* Creates an attenuated capability whose rights are masked by new_rights and
 * whose badge replaces the original. The derived slot shares the object. */
sys_status_t cap_derive(cap_table_t *table, cap_t src, uint64_t new_rights,
                        uint64_t badge, cap_t *dst_out);

/* Copies a capability into another table, requiring CAP_RIGHT_GRANT on src.
 * The badge is preserved so servers can identify the originating client. */
sys_status_t cap_grant(cap_table_t *src_table, cap_t src, cap_table_t *dst_table,
                       cap_t *dst_out);

/* Like cap_grant, but the copy receives only `rights & src.rights`: a grant
 * can keep or attenuate rights, never amplify them. src_table and dst_table
 * may belong to different tasks (or be the same table). */
sys_status_t cap_grant_rights(cap_table_t *src_table, cap_t src,
                              cap_table_t *dst_table, uint64_t rights,
                              cap_t *dst_out);

/* Removes a capability and drops its object reference. */
sys_status_t cap_delete(cap_table_t *table, cap_t cap);

/* Reference-counted wrappers for the remaining capability object kinds. The
 * caller owns the returned reference and must release it with
 * mk_object_release() after inserting it into a table. */
typedef struct cap_vspace_object {
    mk_object_t header;
    vspace_t   *vspace;
} cap_vspace_object_t;

/* How a CAP_OBJ_FRAME is backed and what mapping it means:
 *  - ANON:   one PMM frame whose ownership moves to the first vspace that maps
 *            it (mappable once; freed by the PMM when that mapping goes).
 *  - SHARED: one PMM frame the object keeps a reference on; every mapping
 *            takes its own frame reference, so it can be mapped any number of
 *            times and outlives the capability while still mapped.
 *  - DEVICE: a page-aligned physical range outside PMM management (MMIO, a
 *            linear framebuffer). Mapped non-executable with device cache
 *            attributes, never reference counted and never freed: unmapping
 *            or deleting the capability leaves the hardware range alone. */
typedef enum cap_frame_kind {
    CAP_FRAME_ANON = 0,
    CAP_FRAME_SHARED,
    CAP_FRAME_DEVICE
} cap_frame_kind_t;

typedef struct cap_frame_object {
    mk_object_t      header;
    paddr_t          phys;
    bool             transferred; /* ANON: ownership moved to a vspace mapping */
    cap_frame_kind_t kind;
    size_t           page_count;  /* Pages covered from phys (1 unless DEVICE) */
    bool             write_combining; /* DEVICE: map WC instead of UC */
} cap_frame_object_t;

mk_object_t *cap_vspace_object_create(vspace_t *vspace);
mk_object_t *cap_frame_object_create(paddr_t phys);

/* The object takes over the caller's PMM reference on phys. */
mk_object_t *cap_shared_frame_object_create(paddr_t phys);

/* phys must be page aligned; page_count pages starting there are described.
 * The PMM is never consulted for these frames. */
mk_object_t *cap_device_frame_object_create(paddr_t phys, size_t page_count,
                                            bool write_combining);

/* Maps every page of a frame object at vaddr in vs according to its kind.
 * prot is VSPACE_PROT_WRITE and/or VSPACE_PROT_EXEC; device frames reject
 * EXEC. Returns SYS_ERR_BUSY for an ANON frame already mapped once. On
 * failure no page of the range stays mapped. */
sys_status_t cap_frame_map(cap_frame_object_t *frame, vspace_t *vs,
                           vaddr_t vaddr, uint64_t prot);

/* Boot capabilities: objects the kernel hands to the first user task (for
 * example the Limine framebuffer). Registration retains the object; install
 * copies every registered capability, in registration order, into an empty
 * table, so the n-th registration always lands in handle n. */
#define CAP_BOOT_MAX 8U

sys_status_t cap_boot_register(cap_obj_type_t type, mk_object_t *object,
                               uint64_t rights, cap_t *handle_out);
sys_status_t cap_boot_install(cap_table_t *table, uint32_t *installed_out);

#endif /* LUCYK_CAP_H */
