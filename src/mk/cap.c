#include <cap.h>
#include <io.h>
#include <kalloc.h>
#include <klog.h>
#include <object.h>
#include <pmm.h>
#include <string.h>
#include <types.h>
#include <vspace.h>

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

    uint64_t flags = irq_save();

    for (size_t i = 0; i < CAP_TABLE_SLOTS; i++) {
        cap_slot_t *slot = &table->slots[i];

        if (slot->object != NULL) {
            mk_object_t *object = slot->object;

            slot->object = NULL;
            slot->type = CAP_OBJ_NONE;
            slot->rights = 0;
            slot->badge = 0;
            if (table->used > 0) {
                table->used--;
            }

            mk_object_release(object);
        }
    }

    irq_restore(flags);
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

    uint64_t flags = irq_save();

    int index = find_free_slot(table);
    if (index < 0) {
        irq_restore(flags);
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

    irq_restore(flags);
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

    uint64_t flags = irq_save();
    cap_slot_t *slot = slot_for(table, cap);

    if (slot_is_empty(slot) || slot->type != expected_type) {
        irq_restore(flags);
        return SYS_ERR_CAP_INVALID;
    }
    if ((slot->rights & required_rights) != required_rights) {
        irq_restore(flags);
        return SYS_ERR_CAP_RIGHTS;
    }

    if (retain) {
        mk_object_retain(slot->object);
    }

    *obj_out = slot->object;
    if (badge_out != NULL) {
        *badge_out = slot->badge;
    }

    irq_restore(flags);
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

    uint64_t flags = irq_save();

    cap_slot_t *source = slot_for(table, src);
    if (slot_is_empty(source)) {
        irq_restore(flags);
        return SYS_ERR_CAP_INVALID;
    }

    int index = find_free_slot(table);
    if (index < 0) {
        irq_restore(flags);
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

    irq_restore(flags);
    return SYS_OK;
}

sys_status_t cap_grant(cap_table_t *src_table, cap_t src, cap_table_t *dst_table,
                       cap_t *dst_out)
{
    if (src_table == NULL || dst_table == NULL || dst_out == NULL) {
        return SYS_ERR_INVAL;
    }
    if (!cap_handle_valid(src)) {
        return SYS_ERR_CAP_INVALID;
    }

    uint64_t flags = irq_save();

    cap_slot_t *source = slot_for(src_table, src);
    if (slot_is_empty(source)) {
        irq_restore(flags);
        return SYS_ERR_CAP_INVALID;
    }
    if ((source->rights & CAP_RIGHT_GRANT) == 0) {
        irq_restore(flags);
        return SYS_ERR_CAP_RIGHTS;
    }

    int index = find_free_slot(dst_table);
    if (index < 0) {
        irq_restore(flags);
        return SYS_ERR_NOMEM;
    }

    mk_object_retain(source->object);

    cap_slot_t *slot = &dst_table->slots[index];
    slot->object = source->object;
    slot->type = source->type;
    slot->rights = source->rights;
    slot->badge = source->badge;
    dst_table->used++;

    *dst_out = (cap_t)(index + 1);

    irq_restore(flags);
    return SYS_OK;
}

sys_status_t cap_delete(cap_table_t *table, cap_t cap)
{
    if (table == NULL || !cap_handle_valid(cap)) {
        return SYS_ERR_CAP_INVALID;
    }

    uint64_t flags = irq_save();
    cap_slot_t *slot = slot_for(table, cap);

    if (slot_is_empty(slot)) {
        irq_restore(flags);
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

    irq_restore(flags);

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

    if (!wrapper->transferred && wrapper->phys != 0) {
        pmm_free_frame(wrapper->phys);
    }
    kfree(wrapper);
}

mk_object_t *cap_frame_object_create(paddr_t phys)
{
    cap_frame_object_t *wrapper =
        (cap_frame_object_t *)kalloc(sizeof(cap_frame_object_t));

    if (wrapper == NULL) {
        return NULL;
    }

    memset(wrapper, 0, sizeof(*wrapper));
    wrapper->phys = phys;
    mk_object_init(&wrapper->header, CAP_OBJ_FRAME, frame_object_release);

    return &wrapper->header;
}
