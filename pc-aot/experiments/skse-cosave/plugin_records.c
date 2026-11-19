/* SPDX-License-Identifier: GPL-3.0-only */
#include "plugin_stream.h"
#include "form_delete.h"
#include "core_view.h"
#include "wire_read.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { FILE_HEADER = 20, SECTION_HEADER = 12 };
typedef struct { uint32_t uid; size_t index; } OwnedUID;
struct SksePluginRegistry {
    SkseRegisteredPlugin *plugins;
    OwnedUID *lookup;
    size_t count, owned, savers, loaders;
    SkseFormDeleteCallback *form_delete;
    SkseDeletionSource deletion_source;
    SkseDeletePosition observed;
    atomic_int busy;
};

static int compare_u32(uint32_t a, uint32_t b) { return (a > b) - (a < b); }
static int compare_handle(const void *a, const void *b) {
    return compare_u32(((const SkseRegisteredPlugin *)a)->handle, ((const SkseRegisteredPlugin *)b)->handle);
}
static int compare_uid(const void *a, const void *b) {
    return compare_u32(((const OwnedUID *)a)->uid, ((const OwnedUID *)b)->uid);
}
static const OwnedUID *owner_of(const SksePluginRegistry *r, uint32_t uid) {
    OwnedUID wanted = {uid, 0};
    return r->owned ? bsearch(&wanted, r->lookup, r->owned, sizeof wanted, compare_uid) : NULL;
}

static SksePairResult index_plugins(SksePluginRegistry *r) {
    if (r->count > 1) qsort(r->plugins, r->count, sizeof *r->plugins, compare_handle);
    for (size_t i = 0; i < r->count; ++i) {
        const SkseRegisteredPlugin *p = r->plugins + i;
        if (!p->handle || p->handle == UINT32_MAX || (i && r->plugins[i - 1].handle == p->handle) ||
            p->has_uid > 1 || (p->has_uid != (p->uid != 0)) || (p->load && !p->reset_scratch))
            return SKSE_PAIR_ARGUMENT;
        if (p->has_uid) r->lookup[r->owned++] = (OwnedUID){p->uid, i};
        r->savers += p->has_uid && p->save;
        r->loaders += p->load != NULL;
    }
    if (r->owned > 1) qsort(r->lookup, r->owned, sizeof *r->lookup, compare_uid);
    for (size_t i = 1; i < r->owned; ++i)
        if (r->lookup[i - 1].uid == r->lookup[i].uid) return SKSE_PAIR_DUPLICATE_UID;
    return SKSE_PAIR_OK;
}
SksePairResult skse_plugins_open(const SkseRegisteredPlugin *plugins, size_t count, SksePluginRegistry **out) {
    if (!out) return SKSE_PAIR_ARGUMENT;
    *out = NULL;
    if (count > SKSE_PAIR_MAX_HEADERS) return SKSE_PAIR_LIMIT;
    if (count && !plugins) return SKSE_PAIR_ARGUMENT;
    SksePluginRegistry *r = calloc(1, sizeof *r);
    if (!r) return SKSE_PAIR_STORAGE;
    atomic_init(&r->busy, 0);
    r->count = count;
    if (count) {
        r->plugins = malloc(count * sizeof *r->plugins);
        r->lookup = malloc(count * sizeof *r->lookup);
        if (!r->plugins || !r->lookup) { skse_plugins_close(r); return SKSE_PAIR_STORAGE; }
        memcpy(r->plugins, plugins, count * sizeof *r->plugins);
    }
    SksePairResult result = index_plugins(r);
    if (result == SKSE_PAIR_OK) *out = r;
    else skse_plugins_close(r);
    return result;
}
static int enter(SksePluginRegistry *r) {
    return !atomic_exchange_explicit(&r->busy, 1, memory_order_acquire);
}
static void leave(SksePluginRegistry *r) { atomic_store_explicit(&r->busy, 0, memory_order_release); }
SksePairResult skse_plugins_try_close(SksePluginRegistry **ptr) {
    if (!ptr) return SKSE_PAIR_ARGUMENT;
    SksePluginRegistry *r = *ptr;
    if (!r) return SKSE_PAIR_OK;
    if (!enter(r)) return SKSE_PAIR_BUSY;
    free(r->form_delete);
    free(r->lookup);
    free(r->plugins);
    free(r);
    *ptr = NULL;
    return SKSE_PAIR_OK;
}
void skse_plugins_close(SksePluginRegistry *r) { (void)skse_plugins_try_close(&r); }

SksePairResult skse_plugins_set_form_delete(SksePluginRegistry *r, uint32_t handle, SkseFormDeleteCallback callback) {
    if (!r || !handle || handle == UINT32_MAX) return SKSE_PAIR_ARGUMENT;
    if (!enter(r)) return SKSE_PAIR_BUSY;
    SkseRegisteredPlugin wanted = {.handle = handle};
    const SkseRegisteredPlugin *found = r->count ? bsearch(&wanted, r->plugins, r->count, sizeof wanted, compare_handle) : NULL;
    SksePairResult result = SKSE_PAIR_ARGUMENT;
    if (found) {
        if (callback && !r->form_delete) r->form_delete = calloc(r->count, sizeof *r->form_delete);
        if (callback && !r->form_delete) result = SKSE_PAIR_STORAGE;
        else {
            if (r->form_delete) r->form_delete[found - r->plugins] = callback;
            result = SKSE_PAIR_OK;
        }
    }
    leave(r);
    return result;
}

static int have_epoch(const SkseDeletePosition *position) {
    static const uint8_t zero[16] = {0};
    return memcmp(position->vm_epoch, zero, sizeof zero) != 0;
}
SkseDeletionResult skse_plugins_begin_deletions(SksePluginRegistry *r, const SkseDeletionSource *source) {
    if (!r || !source || !source->position || !source->event) return SKSE_DELETION_ARGUMENT;
    if (!enter(r)) return SKSE_DELETION_BUSY;
    SkseDeletionResult status = SKSE_DELETION_OCCUPIED;
    if (!r->deletion_source.position) {
        SkseDeletePosition current = {{0}, 0};
        if (source->position(source->context, &current) && have_epoch(&current)) {
            r->deletion_source = *source;
            r->observed = current;
            status = SKSE_DELETION_OK;
        } else status = SKSE_DELETION_SOURCE;
    }
    leave(r);
    return status;
}
SkseDeletionResult skse_plugins_stop_deletions(SksePluginRegistry *r) {
    if (!r) return SKSE_DELETION_ARGUMENT;
    if (!enter(r)) return SKSE_DELETION_BUSY;
    r->deletion_source = (SkseDeletionSource){0};
    r->observed = (SkseDeletePosition){{0}, 0};
    leave(r);
    return SKSE_DELETION_OK;
}

static SkseDeletionResult deliver_next(SksePluginRegistry *r, size_t *notified) {
    const SkseDeletionSource *s = &r->deletion_source;
    if (!s->position) return SKSE_DELETION_NOT_ARMED;
    SkseDeletePosition current = {{0}, 0};
    if (!s->position(s->context, &current) || !have_epoch(&current)) return SKSE_DELETION_SOURCE;
    if (memcmp(current.vm_epoch, r->observed.vm_epoch, sizeof current.vm_epoch)) return SKSE_DELETION_WRONG_WORLD;
    if (current.sequence == r->observed.sequence) return SKSE_DELETION_PENDING;
    if (current.sequence < r->observed.sequence) return SKSE_DELETION_GAP;
    uint64_t expected = r->observed.sequence + 1;
    SkseFormDeletion event = {0};
    if (!s->event(s->context, expected, &event)) return SKSE_DELETION_SOURCE;
    if (memcmp(event.position.vm_epoch, current.vm_epoch, sizeof current.vm_epoch)) return SKSE_DELETION_WRONG_WORLD;
    if (event.position.sequence != expected) return SKSE_DELETION_GAP;
    if (event.phase != SKSE_DELETE_CORE_CLEARED) return SKSE_DELETION_INCOMPLETE;
    size_t called = 0;
    if (r->form_delete) {
        for (size_t i = 0; i < r->count; ++i) {
            if (!r->form_delete[i]) continue;
            r->form_delete[i](r->plugins[i].state, event.handle);
            ++called;
        }
    }
    r->observed.sequence = expected;
    if (notified) *notified = called;
    return SKSE_DELETION_OK;
}
SkseDeletionResult skse_plugins_next_deletion(SksePluginRegistry *r, size_t *notified) {
    if (notified) *notified = 0;
    if (!r) return SKSE_DELETION_ARGUMENT;
    if (!enter(r)) return SKSE_DELETION_BUSY;
    SkseDeletionResult status = deliver_next(r, notified);
    leave(r);
    return status;
}
static void reset_plugins(SksePluginRegistry *r) {
    for (size_t i = 0; i < r->count; ++i)
        if (r->plugins[i].reset_scratch) r->plugins[i].reset_scratch(r->plugins[i].state);
}
SksePairResult skse_plugins_reset_scratch(SksePluginRegistry *r) {
    if (!r) return SKSE_PAIR_ARGUMENT;
    if (!enter(r)) return SKSE_PAIR_BUSY;
    reset_plugins(r);
    leave(r);
    return SKSE_PAIR_OK;
}

/* All section walkers below receive a private immutable image whose entire
 * header, chunks, sizes and unique owner IDs have already been validated. */
static size_t section_size(const uint8_t *p) { return (size_t)skse_wire_le32(p + 8) + SECTION_HEADER; }
static SkseOwnerView section_records(const uint8_t *p) {
    return (SkseOwnerView){p + SECTION_HEADER, skse_wire_le32(p + 4)};
}

typedef struct {
    SksePluginRegistry *registry;
    SkseNewSection *updates;
    SkseEncodedRecords all;
} SavePlan;

static SksePairResult collect_records(SavePlan *plan) {
    for (size_t i = 0; i < plan->registry->count; ++i) {
        const SkseRegisteredPlugin *p = plan->registry->plugins + i;
        if (!p->has_uid || !p->save) continue;
        SkseNewSection *update = plan->updates + i;
        update->includes = 1;
        SkseRecordWriter writer = {&plan->all, update, p->uid, 0, SKSE_PAIR_OK};
        int completed = p->save(p->state, &writer);
        if (writer.error != SKSE_PAIR_OK) return writer.error;
        if (!completed) return SKSE_PAIR_STOPPED;
        if (update->count) {
            uint8_t *header = plan->all.data + update->offset;
            skse_stream_put32(header + 4, update->count);
            skse_stream_put32(header + 8, (uint32_t)(update->length - SECTION_HEADER));
        }
    }
    return SKSE_PAIR_OK;
}
static SkseNewSection *replacement(SavePlan *plan, const uint8_t *section) {
    const OwnedUID *owner = owner_of(plan->registry, skse_wire_le32(section));
    if (!owner || !plan->updates[owner->index].includes) return NULL;
    return plan->updates + owner->index;
}
static size_t copy_update(const SavePlan *plan, const SkseNewSection *update, uint8_t *dst) {
    if (update->length) memcpy(dst, plan->all.data + update->offset, update->length);
    return update->length;
}
static SksePairResult merge_records(SksePairImage *image, SavePlan *plan) {
    size_t size = FILE_HEADER + plan->all.used;
    uint32_t headers = plan->all.chunks, owners = 0;
    const uint8_t *end = image->data + image->size;
    for (size_t i = 0; i < plan->registry->count; ++i) owners += plan->updates[i].count != 0;
    for (const uint8_t *p = image->data + FILE_HEADER; p != end; p += section_size(p)) {
        SkseNewSection *update = replacement(plan, p);
        if (update) update->existed = 1;
        else {
            size += section_size(p);
            headers += skse_wire_le32(p + 4) + 1;
            owners++;
        }
    }
    if (size > SKSE_PAIR_MAX_COMPANION || headers > SKSE_PAIR_MAX_HEADERS) return SKSE_PAIR_LIMIT;
    uint8_t *bytes = malloc(size);
    if (!bytes) return SKSE_PAIR_STORAGE;
    memcpy(bytes, image->data, FILE_HEADER);
    skse_stream_put32(bytes + 16, owners);
    uint8_t *dst = bytes + FILE_HEADER;
    for (const uint8_t *p = image->data + FILE_HEADER; p != end; p += section_size(p)) {
        SkseNewSection *update = replacement(plan, p);
        if (update) dst += copy_update(plan, update, dst);
        else { size_t n = section_size(p); memcpy(dst, p, n); dst += n; }
    }
    for (size_t i = 0; i < plan->registry->count; ++i)
        if (!plan->updates[i].existed) dst += copy_update(plan, plan->updates + i, dst);
    free(image->data);
    *image = (SksePairImage){bytes, size};
    return SKSE_PAIR_OK;
}
static SksePairResult edit(SksePairImage *image, void *context) {
    SksePluginRegistry *r = context;
    if (!r->savers) return SKSE_PAIR_OK;
    SavePlan plan = {r, calloc(r->count, sizeof *plan.updates), {0}};
    if (!plan.updates) return SKSE_PAIR_STORAGE;
    SksePairResult result = collect_records(&plan);
    if (result == SKSE_PAIR_OK) result = merge_records(image, &plan);
    free(plan.all.data);
    free(plan.updates);
    return result;
}
SksePairResult skse_plugins_stage(SksePluginRegistry *r, SksePairStage *stage, size_t *participants) {
    if (participants) *participants = 0;
    if (!r || !stage) return SKSE_PAIR_ARGUMENT;
    if (!enter(r)) return SKSE_PAIR_BUSY;
    SksePairResult result = skse_pair_edit_image(stage, edit, r);
    if (result == SKSE_PAIR_OK && participants) *participants = r->savers;
    leave(r);
    return result;
}

static SkseModMapResult prepare_map(const uint8_t *start, const uint8_t *end, const SkseLoadedMod *current,
                                   size_t count, SkseModMap *map) {
    SkseOwnerView core = {0};
    for (const uint8_t *p = start; p != end; p += section_size(p)) {
        if (!skse_wire_le32(p)) { core = section_records(p); break; }
    }
    return skse_mod_map_decode_core(&core, current, count, map);
}
static SksePairResult deliver(const SkseRegisteredPlugin *p, SkseOwnerView records, const SkseModMap *map) {
    SkseRecordReader reader = {records, NULL, 0, map, SKSE_PAIR_OK};
    int completed = p->load(p->state, &reader);
    if (reader.error != SKSE_PAIR_OK) return reader.error;
    return completed ? SKSE_PAIR_OK : SKSE_PAIR_STOPPED;
}
static SksePairResult dispatch(SksePluginRegistry *r, const uint8_t *start, const uint8_t *end,
                               const SkseModMap *map) {
    if (!r->count) return SKSE_PAIR_OWNER_MISSING;
    uint8_t *present = calloc(r->count, 1);
    if (!present) return SKSE_PAIR_STORAGE;
    SksePairResult result = SKSE_PAIR_OK;
    for (const uint8_t *p = start; result == SKSE_PAIR_OK && p != end; p += section_size(p)) {
        const OwnedUID *member = owner_of(r, skse_wire_le32(p));
        if (member) {
            present[member->index] = 1;
            const SkseRegisteredPlugin *plugin = r->plugins + member->index;
            if (plugin->load) result = deliver(plugin, section_records(p), map);
        }
    }
    for (size_t i = 0; result == SKSE_PAIR_OK && i < r->count; ++i) {
        if (!present[i] && r->plugins[i].load)
            result = deliver(r->plugins + i, (SkseOwnerView){0}, map);
    }
    free(present);
    return result;
}
SksePairResult skse_plugins_load(SksePluginRegistry *r, SksePairStore *store, const SksePairGeneration *g,
                                const SkseLoadedMod *current, size_t count, SkseModMapResult *map_status) {
    if (map_status) *map_status = SKSE_MAP_SOURCE_ERROR;
    if (!r || !store || !g || (count && !current)) return SKSE_PAIR_ARGUMENT;
    if (!enter(r)) return SKSE_PAIR_BUSY;
    reset_plugins(r);
    void *raw = NULL;
    size_t size = 0;
    SksePairResult result = r->loaders ? skse_pair_export(store, g, &raw, &size) : SKSE_PAIR_OWNER_MISSING;
    if (result == SKSE_PAIR_OK) {
        const uint8_t *start = (const uint8_t *)raw + FILE_HEADER, *end = (const uint8_t *)raw + size;
        SkseModMap map = {0};
        SkseModMapResult status = prepare_map(start, end, current, count, &map);
        if (map_status) *map_status = status;
        if (status == SKSE_MAP_ARGUMENT || status == SKSE_MAP_INVALID_CURRENT) result = SKSE_PAIR_ARGUMENT;
        else {
            result = dispatch(r, start, end, &map);
            if (result != SKSE_PAIR_OK) reset_plugins(r);
        }
    }
    free(raw);
    leave(r);
    return result;
}
