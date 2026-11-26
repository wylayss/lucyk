#include <io.h>
#include <klog.h>
#include <object.h>
#include <types.h>

/* Reference counting is the one kernel object operation that is inherently
 * cross-CPU: an endpoint referenced by a client on one core is retained and
 * released by a server on another. Plain per-CPU interrupt masking cannot
 * serialise those accesses, so every counter here is updated atomically. */
static uint64_t live_objects;

void mk_object_init(mk_object_t *object, uint32_t type,
                    mk_object_release_fn release)
{
    if (object == NULL) {
        return;
    }

    object->type = type;
    object->ref_count = 1;
    object->release = release;

    __atomic_fetch_add(&live_objects, 1ULL, __ATOMIC_RELAXED);
}

void mk_object_retain(mk_object_t *object)
{
    if (object == NULL) {
        return;
    }

    uint32_t previous =
        __atomic_fetch_add(&object->ref_count, 1U, __ATOMIC_ACQ_REL);

    if (previous == 0) {
        kpanic("mk_object_retain: object already released");
    }
    if (previous == UINT32_MAX) {
        kpanic("mk_object_retain: reference count overflow");
    }
}

void mk_object_release(mk_object_t *object)
{
    if (object == NULL) {
        return;
    }

    /* The decrement that reaches zero owns the destructor, so exactly one CPU
     * runs it and it observes every earlier access. */
    uint32_t previous =
        __atomic_fetch_sub(&object->ref_count, 1U, __ATOMIC_ACQ_REL);

    if (previous == 0) {
        kpanic("mk_object_release: object already released");
    }

    if (previous != 1) {
        return;
    }

    if (object->release != NULL) {
        object->release(object);
    }

    __atomic_fetch_sub(&live_objects, 1ULL, __ATOMIC_RELAXED);
}

uint64_t mk_object_live_count(void)
{
    return __atomic_load_n(&live_objects, __ATOMIC_RELAXED);
}
