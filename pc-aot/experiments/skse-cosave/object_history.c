/* SPDX-License-Identifier: GPL-3.0-only */
#include "object_history.h"
#include "core_view.h"
#include "wire_read.h"
#include "spawn_wire.h"
#include <string.h>

enum { MODS = 0x4d4f4453, OBMG = 0x4f424d47, OBJE = 0x4f424a45 };
typedef enum { BeforeStorage, InStorage, FinishedStorage, PastStorage } StoragePosition;
typedef struct {
    SkseObjectObserver object_observer;
    SkseArgumentObserver argument_observer;
    SkseUICallbackObserver ui_observer;
    SkseUIArgumentObserver ui_argument;
    SkseUIFormObserver ui_form;
    SkseSpawnTaskObserver spawn_task;
    SkseSpawnEntryObserver spawn_entry;
    void *context;
} ObjectHooks;
typedef struct {
    const SkseModMap *map;
    ObjectHooks hooks;
    uint8_t occupied[(SKSE_OBJECT_MAX_SLOTS + 7) / 8];
    SkseObjectSummary summary;
    SkseUIObjectSummary ui;
    SkseObjectCatalogSummary catalog;
    uint32_t seen;
    int saw_mods, include_ui, full_catalog;
    StoragePosition position;
    SkseObjectResult result;
} ObjectWalk;

static uint32_t remaining_values(const ObjectWalk *w) {
    return SKSE_OBJECT_MAX_ARGUMENTS - w->summary.arguments - w->ui.ui_arguments - w->catalog.spawn_entries;
}

static SkseObjectResult read_argument(ObjectWalk *w, SkseWire *cursor, SkseStoredArgument *out) {
    const uint8_t *type = skse_wire_take(cursor, 4);
    if (!type) return SKSE_OBJECT_INVALID;
    uint32_t kind = skse_wire_le32(type);
    if (kind > SKSE_ARG_FORM) return SKSE_OBJECT_UNKNOWN_ARGUMENT;
    *out = (SkseStoredArgument){.kind = (SkseStoredArgumentKind)kind, .form_resolution = SKSE_MOD_NOT_READY};
    if (kind == SKSE_ARG_NONE) return SKSE_OBJECT_OK;
    const uint8_t *value = skse_wire_take(cursor, kind == SKSE_ARG_BOOL ? 1u : 4u);
    if (!value) return SKSE_OBJECT_INVALID;
    if (kind == SKSE_ARG_PROCESS_LOCAL_STRING) {
        ++w->summary.process_local_strings;
        return SKSE_OBJECT_OK;
    }
    if (kind == SKSE_ARG_BOOL) {
        if (*value > 1) return SKSE_OBJECT_INVALID;
        out->bits = *value;
    } else {
        out->bits = skse_wire_le32(value);
        if (kind == SKSE_ARG_FORM) {
            out->form_resolution = skse_mod_resolve_form(w->map, out->bits, &out->mapped_form);
            if (out->form_resolution != SKSE_MOD_RESOLVED && out->form_resolution != SKSE_MOD_DYNAMIC)
                ++w->summary.unresolved_forms;
        }
    }
    return SKSE_OBJECT_OK;
}

static SkseObjectResult read_ui(ObjectWalk *w, SkseWire *cursor, uint32_t slot, uint32_t stack) {
    SkseStoredUICallback event = {.index = slot, .owning_stack = stack};
    if (!skse_wire_name16(cursor, &event.menu) || !skse_wire_name16(cursor, &event.target)) return SKSE_OBJECT_INVALID;
    const uint8_t *count = skse_wire_take(cursor, 4);
    if (!count) return SKSE_OBJECT_INVALID;
    event.argument_count = skse_wire_le32(count);
    if (event.argument_count > cursor->left / 4) return SKSE_OBJECT_INVALID;
    if (event.argument_count > remaining_values(w)) return SKSE_OBJECT_LIMIT;
    w->ui.ui_arguments += event.argument_count;
    w->ui.callbacks++;
    if (w->hooks.ui_observer) w->hooks.ui_observer(w->hooks.context, &event);
    for (uint32_t i = 0; i < event.argument_count; ++i) {
        const uint8_t *type = skse_wire_take(cursor, 4);
        if (!type) return SKSE_OBJECT_INVALID;
        uint32_t kind = skse_wire_le32(type);
        size_t n;
        switch (kind) {
            case SKSE_UI_BOOL: n = 1; break;
            case SKSE_UI_NUMBER: n = 8; break;
            case SKSE_UI_PROCESS_LOCAL_STRING: n = 4; break;
            default: return SKSE_OBJECT_UNKNOWN_ARGUMENT;
        }
        const uint8_t *bytes = skse_wire_take(cursor, n);
        if (!bytes) return SKSE_OBJECT_INVALID;
        SkseStoredUIArgument argument = {.kind = (SkseUIArgumentKind)kind};
        if (kind == SKSE_UI_BOOL) {
            if (*bytes > 1) return SKSE_OBJECT_INVALID;
            argument.bits = *bytes;
        } else if (kind == SKSE_UI_NUMBER) {
            argument.bits = skse_wire_le64(bytes);
        } else w->ui.process_local_strings++;
        if (w->hooks.ui_argument) w->hooks.ui_argument(w->hooks.context, &event, i, &argument);
    }
    return cursor->left ? SKSE_OBJECT_INVALID : SKSE_OBJECT_OK;
}

static SkseObjectResult read_mod_event(ObjectWalk *w, SkseWire *cursor, uint32_t slot, uint32_t stack) {
    SkseStoredModEvent event = {.owning_stack = stack, .index = slot};
    if (!skse_wire_name16(cursor, &event.event_name)) return SKSE_OBJECT_INVALID;
    const uint8_t *count = skse_wire_take(cursor, 4);
    if (!count) return SKSE_OBJECT_INVALID;
    event.argument_count = skse_wire_le32(count);
    if (event.argument_count > cursor->left / 4) return SKSE_OBJECT_INVALID;
    if (event.argument_count > remaining_values(w)) return SKSE_OBJECT_LIMIT;
    w->summary.arguments += event.argument_count;
    w->ui.mod_events++;
    if (w->hooks.object_observer) w->hooks.object_observer(w->hooks.context, &event);
    for (uint32_t i = 0; i < event.argument_count; ++i) {
        SkseStoredArgument value;
        SkseObjectResult result = read_argument(w, cursor, &value);
        if (result != SKSE_OBJECT_OK) return result;
        if (w->hooks.argument_observer) w->hooks.argument_observer(w->hooks.context, &event, i, &value);
    }
    return cursor->left ? SKSE_OBJECT_INVALID : SKSE_OBJECT_OK;
}

static SkseObjectResult read_form_ui(ObjectWalk *w, SkseWire *cursor, uint32_t slot, uint32_t stack) {
    SkseStoredUIForm form = {.index = slot, .owning_stack = stack};
    if (!skse_wire_name16(cursor, &form.menu) || !skse_wire_name16(cursor, &form.target)) return SKSE_OBJECT_INVALID;
    const uint8_t *p = skse_wire_take(cursor, 12);
    if (!p || cursor->left) return SKSE_OBJECT_INVALID;
    form.form_type = skse_wire_le32(p);
    form.saved_handle = skse_wire_le64(p + 4);
    form.resolution = skse_mod_resolve_handle(w->map, form.saved_handle, &form.mapped_handle);
    if (form.resolution != SKSE_MOD_RESOLVED && form.resolution != SKSE_MOD_DYNAMIC) w->catalog.unresolved_ui_handles++;
    w->catalog.ui_forms++;
    if (w->hooks.ui_form) w->hooks.ui_form(w->hooks.context, &form);
    return SKSE_OBJECT_OK;
}
static SkseObjectResult read_spawn(ObjectWalk *w, SkseWire *cursor, uint32_t slot, uint32_t stack) {
    SkseSpawnBody body;
    SkseObjectResult result = skse_spawn_body(cursor, &body);
    if (result != SKSE_OBJECT_OK) return result;
    if (cursor->left) return SKSE_OBJECT_INVALID;
    if (body.count > remaining_values(w)) return SKSE_OBJECT_LIMIT;
    SkseStoredSpawnTask task = {.location = SKSE_SPAWN_STORAGE, .index = slot, .owning_stack = stack};
    w->catalog.unresolved_spawn_forms += skse_spawn_observe(&body, &task, w->map, w->hooks.spawn_task,
                                                           w->hooks.spawn_entry, w->hooks.context);
    w->catalog.spawn_tasks++;
    w->catalog.spawn_entries += body.count;
    return SKSE_OBJECT_OK;
}

typedef enum { UnknownClass, ModEventClass, UICallbackClass, UIFormClass, SpawnClass } StoredClass;
static StoredClass stored_class(const ObjectWalk *w, const uint8_t *name, size_t size) {
    if (size == sizeof("PapyrusModEvent") - 1 && !memcmp(name, "PapyrusModEvent", size)) return ModEventClass;
    if (w->include_ui && size == sizeof("UIInvokeDelegate") - 1 && !memcmp(name, "UIInvokeDelegate", size))
        return UICallbackClass;
    if (w->full_catalog) {
        if (size == sizeof("UIInvokeFormDelegate") - 1 && !memcmp(name, "UIInvokeFormDelegate", size)) return UIFormClass;
        if (size == sizeof("SpawnerTaskFunctor") - 1 && !memcmp(name, "SpawnerTaskFunctor", size)) return SpawnClass;
    }
    return UnknownClass;
}
static SkseObjectResult read_object(ObjectWalk *w, const SksePairRecord *r) {
    SkseWire cursor = {r->data, r->length};
    const uint8_t *length = skse_wire_take(&cursor, 4);
    if (!length) return SKSE_OBJECT_INVALID;
    uint32_t size = skse_wire_le32(length);
    if (!size || size > 1024) return SKSE_OBJECT_INVALID;
    const uint8_t *name = skse_wire_take(&cursor, size);
    if (!name || memchr(name, 0, size)) return SKSE_OBJECT_INVALID;
    StoredClass kind = stored_class(w, name, size);
    if (kind == UnknownClass) return SKSE_OBJECT_UNKNOWN_CLASS;
    if (r->version != 1) return SKSE_OBJECT_UNSUPPORTED_VERSION;

    /* This known class writes its complete body in the current OBJE record.
     * Its caller appends owningStackId and storage index to that same record. */
    if (cursor.left < 8) return SKSE_OBJECT_INVALID;
    cursor.left -= 8;
    const uint8_t *footer = cursor.p + cursor.left;
    uint32_t stack = skse_wire_le32(footer), index = skse_wire_le32(footer + 4);
    if (index >= w->summary.slots) return SKSE_OBJECT_INVALID;
    uint8_t *slot = &w->occupied[index / 8];
    uint8_t bit = (uint8_t)(1u << (index % 8));
    if (*slot & bit) return SKSE_OBJECT_DUPLICATE;
    *slot |= bit;
    switch (kind) {
        case ModEventClass: return read_mod_event(w, &cursor, index, stack);
        case UICallbackClass: return read_ui(w, &cursor, index, stack);
        case UIFormClass: return read_form_ui(w, &cursor, index, stack);
        case SpawnClass: return read_spawn(w, &cursor, index, stack);
        default: return SKSE_OBJECT_UNKNOWN_CLASS;
    }
}

static int inspect(void *context, const SksePairRecord *r) {
    ObjectWalk *w = context;
    if (r->type == OBMG) {
        if (w->position != BeforeStorage) w->result = SKSE_OBJECT_DUPLICATE;
        else if (!w->saw_mods) w->result = SKSE_OBJECT_OUT_OF_ORDER;
        else if (r->version != 1) w->result = SKSE_OBJECT_UNSUPPORTED_VERSION;
        else if (r->length != 8) w->result = SKSE_OBJECT_INVALID;
        else {
            const uint8_t *p = r->data;
            w->summary.slots = skse_wire_le32(p);
            w->summary.filled = skse_wire_le32(p + 4);
            if (w->summary.filled > w->summary.slots) w->result = SKSE_OBJECT_INVALID;
            else if (w->summary.slots > SKSE_OBJECT_MAX_SLOTS) w->result = SKSE_OBJECT_LIMIT;
            else w->position = w->summary.filled ? InStorage : FinishedStorage;
        }
    } else if (w->position == InStorage) {
        w->result = r->type == OBJE ? read_object(w, r) : SKSE_OBJECT_INVALID;
        if (w->result == SKSE_OBJECT_OK && ++w->seen == w->summary.filled) w->position = FinishedStorage;
    } else if (w->position == FinishedStorage) {
        if (r->type == OBJE) w->result = SKSE_OBJECT_INVALID;
        else w->position = PastStorage;
    } else if (r->type == MODS) w->saw_mods = 1;
    return w->result == SKSE_OBJECT_OK;
}

static SkseObjectResult walk(const SkseCoreSnapshot *snapshot, SkseObjectSummary *out, SkseUIObjectSummary *ui,
                             SkseObjectCatalogSummary *catalog, ObjectHooks hooks) {
    ObjectWalk w = {.map = &snapshot->map, .hooks = hooks, .include_ui = ui || catalog, .full_catalog = catalog != NULL};
    skse_records_each(&snapshot->core, inspect, &w);
    if (w.result != SKSE_OBJECT_OK) return w.result;
    if (w.position == BeforeStorage) return SKSE_OBJECT_MISSING;
    if (w.position == InStorage) return SKSE_OBJECT_INVALID;
    if (out) *out = w.summary;
    if (ui) {
        w.ui.slots = w.summary.slots;
        w.ui.filled = w.summary.filled;
        *ui = w.ui;
    }
    if (catalog) {
        w.catalog.slots = w.summary.slots;
        w.catalog.filled = w.summary.filled;
        w.catalog.mod_events = w.ui.mod_events;
        w.catalog.ui_callbacks = w.ui.callbacks;
        w.catalog.mod_arguments = w.summary.arguments;
        w.catalog.ui_arguments = w.ui.ui_arguments;
        w.catalog.process_local_strings = w.summary.process_local_strings + w.ui.process_local_strings;
        w.catalog.unresolved_mod_forms = w.summary.unresolved_forms;
        *catalog = w.catalog;
    }
    return SKSE_OBJECT_OK;
}

SkseObjectResult skse_mod_event_objects(const SkseCoreSnapshot *snapshot, SkseObjectSummary *out,
    SkseObjectObserver objects, SkseArgumentObserver arguments, void *context) {
    if (!out) return SKSE_OBJECT_ARGUMENT;
    *out = (SkseObjectSummary){0};
    if (!snapshot) return SKSE_OBJECT_ARGUMENT;
    SkseObjectResult result = walk(snapshot, out, NULL, NULL, (ObjectHooks){0});
    if (result == SKSE_OBJECT_OK && (objects || arguments)) {
        ObjectHooks hooks = {.object_observer = objects, .argument_observer = arguments, .context = context};
        result = walk(snapshot, out, NULL, NULL, hooks);
    }
    return result;
}
SkseObjectResult skse_ui_callback_objects(const SkseCoreSnapshot *snapshot, SkseUIObjectSummary *out,
    SkseUICallbackObserver callbacks, SkseUIArgumentObserver arguments, void *context) {
    if (!out) return SKSE_OBJECT_ARGUMENT;
    *out = (SkseUIObjectSummary){0};
    if (!snapshot) return SKSE_OBJECT_ARGUMENT;
    SkseObjectResult result = walk(snapshot, NULL, out, NULL, (ObjectHooks){0});
    if (result == SKSE_OBJECT_OK && (callbacks || arguments)) {
        ObjectHooks hooks = {.ui_observer = callbacks, .ui_argument = arguments, .context = context};
        result = walk(snapshot, NULL, out, NULL, hooks);
    }
    return result;
}

SkseObjectResult skse_core_storage_objects(const SkseCoreSnapshot *snapshot, SkseObjectCatalogSummary *out,
                                          const SkseStoredObjectObservers *observers, void *context) {
    if (!out) return SKSE_OBJECT_ARGUMENT;
    *out = (SkseObjectCatalogSummary){0};
    if (!snapshot) return SKSE_OBJECT_ARGUMENT;
    SkseObjectResult result = walk(snapshot, NULL, NULL, out, (ObjectHooks){0});
    if (result == SKSE_OBJECT_OK && observers) {
        ObjectHooks hooks = {observers->mod_event, observers->mod_argument, observers->ui_callback,
                             observers->ui_argument, observers->ui_form, observers->spawn_task, observers->spawn_entry,
                             context};
        result = walk(snapshot, NULL, NULL, out, hooks);
    }
    return result;
}
