/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "mod_history.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SksePluginRegistry SksePluginRegistry;
typedef struct SkseRecordWriter SkseRecordWriter;
typedef struct SkseRecordReader SkseRecordReader;

/* These callbacks apply only to caller-owned reversible state. A zero
 * return explicitly aborts the entire operation. The cursor is borrowed
 * only for the duration of this callback. */
typedef int (*SksePluginSave)(void *state, SkseRecordWriter *records);
typedef int (*SksePluginLoad)(void *state, SkseRecordReader *records);
typedef struct {
    uint32_t handle, has_uid, uid;
    void *state;
    SksePluginSave save;
    SksePluginLoad load;
    SksePairRevert reset_scratch;
} SkseRegisteredPlugin;

/* The registry copies callback descriptors. The caller keeps the code and
 * state alive until close. A plugin without a UID may receive a load with
 * an empty cursor; it may not own or produce a file section. UID 0 belongs
 * to the core. The handle order is the actual guest registration order. */
SksePairResult skse_plugins_open(const SkseRegisteredPlugin *registered, size_t count, SksePluginRegistry **out);
void skse_plugins_close(SksePluginRegistry *registry);
/* The owner stops new calls before destroying the registry. Inside a running
 * callback this returns BUSY and leaves *registry valid for a later retry. */
SksePairResult skse_plugins_try_close(SksePluginRegistry **registry);
SksePairResult skse_plugins_reset_scratch(SksePluginRegistry *registry);

int skse_record_open(SkseRecordWriter *writer, uint32_t type, uint32_t version);
int skse_record_append(SkseRecordWriter *writer, const void *data, uint32_t length);
int skse_record_write(SkseRecordWriter *writer, uint32_t type, uint32_t version,
                      const void *data, uint32_t length);
int skse_record_next(SkseRecordReader *reader, uint32_t *type, uint32_t *version, uint32_t *length);
uint32_t skse_record_read(SkseRecordReader *reader, void *data, uint32_t capacity);
SkseModResolution skse_record_resolve_form(SkseRecordReader *reader, uint32_t saved, uint32_t *current);
SkseModResolution skse_record_resolve_handle(SkseRecordReader *reader, uint64_t saved, uint64_t *current);

/* A failed callback or stream poisons this stage. Any number of owners is
 * merged with one pass that copies surviving original sections verbatim.
 * "participants" counts owners whose save callback completed, including
 * owners who deliberately emitted no records. It is zero on failure. */
SksePairResult skse_plugins_stage(SksePluginRegistry *registry, SksePairStage *stage, size_t *participants);
/* Reset all private receivers on entry. A valid generation delivers present
 * owners in wire order then absent receivers in handle order. A missing or
 * rejected generation delivers no load callbacks. Any callback or cursor
 * error resets every receiver again. The file and its digest are read once.
 * Optional map_status describes the actual MODS history; unresolved IDs
 * leave the caller's destination untouched. */
SksePairResult skse_plugins_load(SksePluginRegistry *registry, SksePairStore *store,
                                const SksePairGeneration *generation, const SkseLoadedMod *current,
                                size_t current_count, SkseModMapResult *map_status);

#ifdef __cplusplus
}
#endif
