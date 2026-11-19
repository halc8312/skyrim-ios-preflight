/* SPDX-License-Identifier: GPL-3.0-only */
#include "registration_history.h"
#include "core_view.h"
#include "wire_read.h"

enum { MODS = 0x4d4f4453, REGS = 0x52454753, REGE = 0x52454745 };
typedef enum { BeforeGroup, InsideGroup, AfterGroup } Position;
typedef struct {
    SkseRegistrationFormat format;
    const SkseModMap *map;
    SkseRegistrationObserver observer;
    void *context;
    Position position;
    int saw_mods;
    SkseRegistrationResult result;
    SkseRegistrationSummary totals;
} RegistrationWalk;

int skse_registration_format(SkseRegistrationFamily family, SkseRegistrationFormat *out) {
    static const SkseRegistrationFormat formats[] = {
        [SKSE_REG_MENU_EVENTS]    = {0x4d454e52, SKSE_REG_NAMED_KEY, 0, 1},   /* MENR */
        [SKSE_REG_MOD_CALLBACKS]  = {0x4d434252, SKSE_REG_NAMED_KEY, 1, 1},   /* MCBR */
        [SKSE_REG_INPUT_KEYS]     = {0x4b455952, SKSE_REG_NUMERIC_KEY, 0, 1}, /* KEYR */
        [SKSE_REG_INPUT_CONTROLS] = {0x43544c52, SKSE_REG_NAMED_KEY, 0, 1},   /* CTLR */
        [SKSE_REG_ACTOR_ACTIONS]  = {0x41414354, SKSE_REG_NUMERIC_KEY, 0, 1}, /* AACT */
        [SKSE_REG_CROSSHAIR]      = {0x43485252, SKSE_REG_NO_KEY, 0, 1},      /* CHRR */
        [SKSE_REG_CAMERA]         = {0x43414d52, SKSE_REG_NO_KEY, 0, 1},      /* CAMR */
        [SKSE_REG_NODE_WRITER]    = {0x4e494e55, SKSE_REG_NO_KEY, 0, 0},      /* NINU */
        [SKSE_REG_NODE_READER]    = {0x4e694e55, SKSE_REG_NO_KEY, 0, 1}       /* NiNU */
    };
    if (!out) return 0;
    if ((unsigned)family >= sizeof formats / sizeof formats[0]) { *out = (SkseRegistrationFormat){0}; return 0; }
    *out = formats[family];
    return 1;
}
static SkseRegistrationResult group(RegistrationWalk *walk, const SksePairRecord *row) {
    SkseWire cursor = {row->data, row->length};
    SkseRegistrationKey event = {.type = walk->format.key_type};
    if (event.type == SKSE_REG_NAMED_KEY && !skse_wire_name16(&cursor, &event.name)) return SKSE_REG_INVALID;
    if (event.type == SKSE_REG_NUMERIC_KEY) {
        const uint8_t *key = skse_wire_take(&cursor, 4);
        if (!key) return SKSE_REG_INVALID;
        event.number = skse_wire_le32(key);
    }
    const uint8_t *length = skse_wire_take(&cursor, 4);
    if (!length) return SKSE_REG_INVALID;
    uint32_t count = skse_wire_le32(length);
    int named_callback = walk->format.named_callback;
    if (count > cursor.left / (named_callback ? 11u : 8u)) return SKSE_REG_INVALID;
    if (walk->totals.groups == SKSE_REG_MAX_GROUPS || count > SKSE_REG_MAX_HANDLES - walk->totals.handles)
        return SKSE_REG_LIMIT;
    ++walk->totals.groups;
    walk->totals.handles += count;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *handle = skse_wire_take(&cursor, 8);
        SkseRegistrationName callback = {0};
        if (!handle || (named_callback && !skse_wire_name16(&cursor, &callback))) return SKSE_REG_INVALID;
        uint64_t saved = skse_wire_le32(handle) | (uint64_t)skse_wire_le32(handle + 4) << 32, mapped;
        SkseModResolution status = skse_mod_resolve_handle(walk->map, saved, &mapped);
        ++walk->totals.resolutions[status];
        if (walk->observer) {
            const uint64_t *value = status == SKSE_MOD_RESOLVED || status == SKSE_MOD_DYNAMIC ? &mapped : NULL;
            walk->observer(walk->context, event, callback, saved, status, value);
        }
    }
    return cursor.left ? SKSE_REG_INVALID : SKSE_REG_OK;
}
static int inspect(void *arg, const SksePairRecord *record) {
    RegistrationWalk *walk = arg;
    if (record->type == walk->format.tag) {
        if (walk->position != BeforeGroup) walk->result = SKSE_REG_DUPLICATE;
        else if (!walk->saw_mods) walk->result = SKSE_REG_OUT_OF_ORDER;
        else if (record->version != 1) walk->result = SKSE_REG_UNSUPPORTED;
        else if (walk->format.key_type == SKSE_REG_NO_KEY) {
            walk->result = group(walk, record);
            walk->position = AfterGroup;
        } else if (record->length) walk->result = SKSE_REG_INVALID;
        else walk->position = InsideGroup;
    } else if (walk->position == InsideGroup) {
        if (record->type != REGS && record->type != REGE) walk->result = SKSE_REG_INVALID;
        else if (record->version != 1) walk->result = SKSE_REG_UNSUPPORTED;
        else if (record->type == REGS) walk->result = group(walk, record);
        else if (record->length) walk->result = SKSE_REG_INVALID;
        else walk->position = AfterGroup;
    } else if (record->type == MODS) walk->saw_mods = 1;
    return walk->result == SKSE_REG_OK;
}

static SkseRegistrationResult walk_family(const SkseOwnerView *core, const SkseModMap *map, SkseRegistrationFormat format,
                                          SkseRegistrationObserver observer, void *context, SkseRegistrationSummary *out) {
    RegistrationWalk walk = {.format = format, .map = map, .observer = observer, .context = context};
    skse_records_each(core, inspect, &walk);
    if (walk.result == SKSE_REG_OK) {
        if (walk.position == BeforeGroup) return SKSE_REG_MISSING;
        if (walk.position != AfterGroup) return SKSE_REG_INVALID;
        *out = walk.totals;
    }
    return walk.result;
}
SkseRegistrationResult skse_registration_view_open(SksePairStore *store, const SksePairGeneration *generation,
    const SkseLoadedMod *current, size_t count, SkseRegistrationView **out, SksePairResult *source_result,
    SkseModMapResult *map_result) {
    return (SkseRegistrationResult)skse_core_snapshot_open(store, generation, current, count, out, source_result, map_result);
}

void skse_registration_view_close(SkseRegistrationView *view) { skse_core_snapshot_close(view); }

SkseRegistrationResult skse_registration_view_read(const SkseRegistrationView *view, SkseRegistrationFamily family,
    SkseRegistrationSummary *out, SkseRegistrationObserver observer, void *context) {
    if (!out) return SKSE_REG_ARGUMENT;
    *out = (SkseRegistrationSummary){0};
    SkseRegistrationFormat format;
    if (!view || !skse_registration_format(family, &format)) return SKSE_REG_ARGUMENT;
    SkseRegistrationResult result = walk_family(&view->core, &view->map, format, NULL, NULL, out);
    if (result == SKSE_REG_OK && observer) result = walk_family(&view->core, &view->map, format, observer, context, out);
    return result;
}

SkseRegistrationResult skse_registration_history_load(SksePairStore *store, const SksePairGeneration *generation,
    const SkseLoadedMod *current, size_t count, SkseRegistrationFamily family, SkseRegistrationSummary *out,
    SkseRegistrationObserver observer, void *context, SksePairResult *source_result, SkseModMapResult *map_result) {
    if (source_result) *source_result = SKSE_PAIR_ARGUMENT;
    if (map_result) *map_result = SKSE_MAP_ARGUMENT;
    if (!out) return SKSE_REG_ARGUMENT;
    *out = (SkseRegistrationSummary){0};
    SkseRegistrationFormat format;
    if (!skse_registration_format(family, &format)) return SKSE_REG_ARGUMENT;
    SkseRegistrationView *view;
    SkseRegistrationResult result = skse_registration_view_open(store, generation, current, count, &view, source_result, map_result);
    if (result == SKSE_REG_OK) result = skse_registration_view_read(view, family, out, observer, context);
    skse_registration_view_close(view);
    return result;
}
