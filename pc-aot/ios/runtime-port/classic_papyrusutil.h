/* SPDX-License-Identifier: GPL-3.0-only
 * PapyrusUtil 3.3 JsonUtil natives during play (plan P3-2). The declarations
 * cover Wearable Lanterns 4.0.2's configuration and startup compatibility
 * check from ebf678e, and the selected static Keyword used by StorageUtil.
 *
 * A call's result and its settings file change are published only in the VM
 * generation that started the call (classic_vm_epoch.h). The commit holds the
 * generation lock until the VM result is written, so a revert waits for it.
 * Include after the runtime's x86.h.
 */
#pragma once
#include "classic_papyrus_values.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { SKYRIM_PAPYRUSUTIL_FUNCTIONS = 12 };

/* What the provider needs from the settings scope; production binds
 * skse_classic_settings (SKYRIM_CLASSIC_SETTINGS_BACKEND), tests an authored one.
 * method: row number in SkseSettingsMethod. */
typedef struct {
    bool (*invoke)(void *context, unsigned method, const ClassicValue *args, size_t count, ClassicValue *result,
                   char *text, size_t capacity, uint64_t *ticket);
    bool (*commit)(void *context, uint64_t ticket);
    void (*discard)(void *context, uint64_t ticket);
    bool (*read_guest)(void *context, CPU *, uint32_t address, void *bytes, size_t length);
    bool (*copy_string)(void *context, CPU *, uint32_t address, char *text, size_t capacity);
    void *context;
    bool (*copy_form_id)(void *context, CPU *, const ClassicValueFrame *, uint32_t address, uint32_t *identity);
} SkyrimPapyrusUtilBackend;

typedef struct {
    uint64_t published, calls, refused_stale, commits, discards, errors;
} SkyrimPapyrusUtilStats;

/* Before guest start, once. values_epoch identifies the publication for
 * skse_classic_values_retire. Returns 0 when the request was accepted. */
int skyrim_papyrusutil_publish(const SkyrimPapyrusUtilBackend *, uint64_t values_epoch);
const ClassicValueFunction *skyrim_papyrusutil_functions(void);
void skyrim_papyrusutil_stats(SkyrimPapyrusUtilStats *);
void skyrim_papyrusutil_report(void); /* IOS_PAPYRUSUTIL */

#ifdef SKYRIM_CLASSIC_SETTINGS_BACKEND
#include "classic_settings.h"
#include "classic_vm_epoch.h"
/* Production backend: one open settings scope, its epoch, the launch uptime. */
typedef struct {
    SkseClassicSettings *settings;
    uint64_t epoch;
    uint32_t uptime_ms;
    uint8_t *storage_pex;
    size_t storage_pex_size;
    int game_fd;
    uint64_t keyword_world;
    uint32_t keyword_id;
} SkyrimPapyrusUtilSettings;
enum SkyrimLanternDiagnostic {
    SKYRIM_LANTERN_ASSETS, SKYRIM_LANTERN_CATALOG, SKYRIM_LANTERN_GET, SKYRIM_LANTERN_SET
};
void skyrim_papyrusutil_report_lantern(enum SkyrimLanternDiagnostic, bool accepted);
void skyrim_papyrusutil_settings_backend(SkyrimPapyrusUtilSettings *, SkyrimPapyrusUtilBackend *out);
bool skyrim_papyrusutil_needs_initial_catalog(void);
bool skyrim_papyrusutil_initial_catalog(const SkyrimVmScope *, const SkseLoadedMod *, size_t count);
bool skyrim_papyrusutil_capture_storage(const SkyrimVmScope *, const SkseLoadedMod *, size_t count,
                                        SksePluginRecordSet **out);
/* After authentication of a completed base load, a NULL input runs only the
 * previous-world reset. A non-NULL input was verified against that base. */
bool skyrim_papyrusutil_apply_storage(const SkyrimVmScope *, const SkseLoadedMod *, size_t count,
                                     const SksePluginRecordSet *input, const SkseSerialResolvers *);
#endif

#ifdef __cplusplus
}
#endif
