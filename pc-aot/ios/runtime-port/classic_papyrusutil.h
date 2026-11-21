/* SPDX-License-Identifier: GPL-3.0-only
 * PapyrusUtil 3.3 JsonUtil natives during play (plan P3-2). The seven
 * functions are the ones Wearable Lanterns 4.0.2 calls in
 * _WL_SkyUIConfigPanelScript.psc (ebf678e): GetIntValue 3, GetFloatValue 1,
 * GetStringValue 2, SetIntValue 55, SetFloatValue 23, SetStringValue 3 and
 * Save 10 calls; no StorageUtil and no array functions. They match the seven
 * methods of experiments/skse-plugin-vm/classic_settings.
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

enum { SKYRIM_PAPYRUSUTIL_FUNCTIONS = 7 };

/* What the provider needs from the settings scope; production binds
 * skse_classic_settings (SKYRIM_CLASSIC_SETTINGS_BACKEND), tests an authored one.
 * method: 1..7 in the order of SkseSettingsMethod. */
typedef struct {
    bool (*invoke)(void *context, unsigned method, const ClassicValue *args, size_t count, ClassicValue *result,
                   char *text, size_t capacity, uint64_t *ticket);
    bool (*commit)(void *context, uint64_t ticket);
    void (*discard)(void *context, uint64_t ticket);
    bool (*read_guest)(void *context, CPU *, uint32_t address, void *bytes, size_t length);
    bool (*copy_string)(void *context, CPU *, uint32_t address, char *text, size_t capacity);
    void *context;
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
} SkyrimPapyrusUtilSettings;
void skyrim_papyrusutil_settings_backend(SkyrimPapyrusUtilSettings *, SkyrimPapyrusUtilBackend *out);
bool skyrim_papyrusutil_capture_storage(const SkyrimVmScope *, const SkseLoadedMod *, size_t count,
                                        SksePluginRecordSet **out);
#endif

#ifdef __cplusplus
}
#endif
