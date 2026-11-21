/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_hook_catalog.h"
#include <stdio.h>
#include <string.h>

/* PcGuestModHooksABI.h bits. */
enum { GROUP_CHARGEN_PRECACHE = 8u };

/* [begin, end) in the pinned 2011 image; a request overlapping it matches. */
static const struct {
    uint32_t begin, end, group;
    SkyrimPatchStatus without_group;
    const char *adapter;
} catalog[] = {
    /* experiments/classic-package-decision: PapyrusUtil package hooks (call at 0x6A9E90, return 0x6A9E95). */
    {0x006A9E90u, 0x006A9E95u, 0, SKYRIM_PATCH_EXPERIMENT_ONLY, "PapyrusUtil package decision"},
    /* Same experiment: the package change call at 0x8CAB54 (return 0x8CAB59). */
    {0x008CAB54u, 0x008CAB59u, 0, SKYRIM_PATCH_EXPERIMENT_ONLY, "PapyrusUtil package change"},
    /* Same experiment: the idle event test at label 0x6F02FF. */
    {0x006F02FFu, 0x006F0304u, 0, SKYRIM_PATCH_EXPERIMENT_ONLY, "PapyrusUtil idle event"},
    /* experiments/classic-chargen-precache: the first-open warm call (call at 0x886ACB,
     * return 0x886AD0) and the instruction after it. */
    {0x00886ACBu, 0x00886AD5u, GROUP_CHARGEN_PRECACHE, SKYRIM_PATCH_NEEDS_GUEST, "character menu first-open precache"},
};

size_t skyrim_mod_patch_catalog_count(void) { return sizeof catalog / sizeof catalog[0]; }

uint32_t skyrim_mod_patch_function(uint32_t address, const uint32_t *starts, size_t count) {
    if (!starts || !count || address < starts[0]) return 0;
    size_t low = 0, high = count;  /* starts[low] <= address < starts[high] */
    while (high - low > 1) {
        const size_t middle = low + (high - low) / 2;
        if (starts[middle] <= address) low = middle; else high = middle;
    }
    return starts[low];
}

void skyrim_mod_patch_describe(uint32_t site, uint32_t length, const uint8_t *bytes, uint32_t compiled_groups,
                               const uint32_t *starts, size_t count, SkyrimPatchDescription *out) {
    *out = (SkyrimPatchDescription){.site = site, .length = length, .catalog = -1,
                                    .function = skyrim_mod_patch_function(site, starts, count)};
    if (bytes && length >= 5 && (bytes[0] == 0xe8 || bytes[0] == 0xe9)) {
        uint32_t relative;
        memcpy(&relative, bytes + 1, 4);
        out->kind = bytes[0] == 0xe8 ? SKYRIM_PATCH_CALL : SKYRIM_PATCH_JUMP;
        out->target = site + 5 + relative;
    }
    const uint64_t end = (uint64_t)site + length;
    for (size_t i = 0; i < sizeof catalog / sizeof catalog[0]; ++i) {
        if (end <= catalog[i].begin || site >= catalog[i].end) continue;
        out->catalog = (int)i;
        out->group = catalog[i].group;
        out->adapter = catalog[i].adapter;
        out->status = catalog[i].group && (compiled_groups & catalog[i].group) ? SKYRIM_PATCH_ADAPTER_AVAILABLE
                                                                               : catalog[i].without_group;
        return;
    }
    out->status = SKYRIM_PATCH_UNSUPPORTED;
}

int skyrim_mod_patch_text(const SkyrimPatchDescription *d, int japanese, char *out, size_t capacity) {
    char function[40] = "";
    if (d->function)
        snprintf(function, sizeof function, japanese ? "（関数 0x%08X）" : " (function 0x%08X)", (unsigned)d->function);
    const char *kind = d->kind == SKYRIM_PATCH_CALL ? (japanese ? "呼び出しの置き換え" : "call")
                     : d->kind == SKYRIM_PATCH_JUMP ? (japanese ? "ジャンプの差し込み" : "jump")
                     : (japanese ? "書き換え" : "byte change");
    switch (d->status) {
    case SKYRIM_PATCH_ADAPTER_AVAILABLE:
        return snprintf(out, capacity, japanese
            ? "このDLLはゲーム本体の 0x%08X%sを変更しようとしました（%s、%u バイト）。iOS版の代替処理（%s）で扱います。"
            : "This DLL tried to change the game at 0x%08X%s (%s, %u bytes). The iOS replacement (%s) handles it.",
            (unsigned)d->site, function, kind, (unsigned)d->length, d->adapter);
    case SKYRIM_PATCH_NEEDS_GUEST:
        return snprintf(out, capacity, japanese
            ? "このDLLはゲーム本体の 0x%08X%sを変更しようとしました（%s、%u バイト）。代替処理（%s）にはゲストの再ビルドが必要です。"
            : "This DLL tried to change the game at 0x%08X%s (%s, %u bytes). Its replacement (%s) needs a rebuilt guest.",
            (unsigned)d->site, function, kind, (unsigned)d->length, d->adapter);
    case SKYRIM_PATCH_EXPERIMENT_ONLY:
        return snprintf(out, capacity, japanese
            ? "このDLLはゲーム本体の 0x%08X%sを変更しようとしました（%s、%u バイト）。代替処理（%s）は試作段階で、iOS版では未対応です。"
            : "This DLL tried to change the game at 0x%08X%s (%s, %u bytes). Its replacement (%s) is still an experiment; not supported on iOS.",
            (unsigned)d->site, function, kind, (unsigned)d->length, d->adapter);
    default:
        return snprintf(out, capacity, japanese
            ? "このDLLはゲーム本体の 0x%08X%sを変更しようとしました（%s、%u バイト）。iOS版では未対応です。"
            : "This DLL tried to change the game at 0x%08X%s (%s, %u bytes). Not supported on iOS.",
            (unsigned)d->site, function, kind, (unsigned)d->length);
    }
}
