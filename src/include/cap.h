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

/* Removes a capability and drops its object reference. */
sys_status_t cap_delete(cap_table_t *table, cap_t cap);

/* Reference-counted wrappers for the remaining capability object kinds. The
 * caller owns the returned reference and must release it with
 * mk_object_release() after inserting it into a table. */
typedef struct cap_vspace_object {
    mk_object_t header;
    vspace_t   *vspace;
} cap_vspace_object_t;

typedef struct cap_frame_object {
    mk_object_t header;
    paddr_t     phys;
    bool        transferred; /* Ownership moved to a vspace mapping */
} cap_frame_object_t;

mk_object_t *cap_vspace_object_create(vspace_t *vspace);
mk_object_t *cap_frame_object_create(paddr_t phys);

#endif /* LUCYK_CAP_H */
