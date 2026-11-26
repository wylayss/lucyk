#ifndef LUCYK_OBJECT_H
#define LUCYK_OBJECT_H

#include <types.h>

typedef struct mk_object mk_object_t;
typedef void (*mk_object_release_fn)(mk_object_t *object);

/* Every kernel object that can be named by a capability begins with this
 * header. It carries the object kind, a reference count, and the destructor
 * used when the final reference is released. */
struct mk_object {
    uint32_t             type;
    uint32_t             ref_count;
    mk_object_release_fn release;
};

void mk_object_init(mk_object_t *object, uint32_t type,
                    mk_object_release_fn release);

/* Retains/releases one reference. The release callback runs when the count
 * reaches zero; it is responsible for freeing the containing object. */
void mk_object_retain(mk_object_t *object);
void mk_object_release(mk_object_t *object);

/* Number of live capability-managed objects. Used by teardown self-tests. */
uint64_t mk_object_live_count(void);

#endif /* LUCYK_OBJECT_H */
