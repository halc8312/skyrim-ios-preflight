/* Plan X5 catalog: known sites, kinds, function lookup and both languages. */
#include "mod_hook_catalog.h"
#include <assert.h>
#include <string.h>

int main(void) {
    const uint32_t starts[] = {0x00401000u, 0x00886930u, 0x00886B50u, 0x00A00000u};
    assert(skyrim_mod_patch_function(0x00886ACBu, starts, 4) == 0x00886930u);
    assert(skyrim_mod_patch_function(0x00886B50u, starts, 4) == 0x00886B50u);
    assert(!skyrim_mod_patch_function(0x00400000u, starts, 4) && !skyrim_mod_patch_function(1, NULL, 0));
    assert(skyrim_mod_patch_function(0xFFFFFFFFu, starts, 4) == 0x00A00000u);

    /* A call replacement at the precache warm call: target, catalog, guest group. */
    const uint8_t call[5] = {0xe8, 0x10, 0x00, 0x00, 0x00};
    SkyrimPatchDescription d;
    skyrim_mod_patch_describe(0x00886ACBu, 5, call, 0, starts, 4, &d);
    assert(d.kind == SKYRIM_PATCH_CALL && d.target == 0x00886ACBu + 5 + 0x10 && d.function == 0x00886930u);
    assert(d.catalog >= 0 && d.status == SKYRIM_PATCH_NEEDS_GUEST && d.group == 8);
    skyrim_mod_patch_describe(0x00886ACBu, 5, call, 8, starts, 4, &d);
    assert(d.status == SKYRIM_PATCH_ADAPTER_AVAILABLE);
    /* A patch starting at the following instruction still overlaps the entry. */
    skyrim_mod_patch_describe(0x00886AD0u, 2, (const uint8_t[]){0x90, 0x90}, 8, starts, 4, &d);
    assert(d.kind == SKYRIM_PATCH_OTHER && d.status == SKYRIM_PATCH_ADAPTER_AVAILABLE);

    /* PapyrusUtil's sites are known from experiments only. */
    const uint8_t jump[5] = {0xe9, 0xf0, 0xff, 0xff, 0xff};
    skyrim_mod_patch_describe(0x006A9E90u, 5, jump, 15, starts, 4, &d);
    assert(d.kind == SKYRIM_PATCH_JUMP && d.target == 0x006A9E90u + 5 - 0x10 && d.status == SKYRIM_PATCH_EXPERIMENT_ONLY);
    /* Anything else is unsupported; adjacent ranges do not match. */
    skyrim_mod_patch_describe(0x006A9E95u, 5, jump, 15, starts, 4, &d);
    assert(d.catalog == -1 && d.status == SKYRIM_PATCH_UNSUPPORTED && !d.adapter);
    skyrim_mod_patch_describe(0x006A9E8Bu, 5, jump, 15, starts, 4, &d);
    assert(d.catalog == -1);
    skyrim_mod_patch_describe(0x00C00000u, 3, (const uint8_t[]){0xe9, 0, 0}, 0, NULL, 0, &d);
    assert(d.kind == SKYRIM_PATCH_OTHER && !d.function && !d.target);

    char line[256];
    skyrim_mod_patch_describe(0x00A00010u, 5, jump, 0, starts, 4, &d);
    assert(skyrim_mod_patch_text(&d, 1, line, sizeof line) > 0);
    assert(!strcmp(line, "このDLLはゲーム本体の 0x00A00010（関数 0x00A00000）を変更しようとしました（ジャンプの差し込み、5 バイト）。iOS版では未対応です。"));
    assert(skyrim_mod_patch_text(&d, 0, line, sizeof line) > 0);
    assert(!strcmp(line, "This DLL tried to change the game at 0x00A00010 (function 0x00A00000) (jump, 5 bytes). Not supported on iOS."));
    skyrim_mod_patch_describe(0x00886ACBu, 5, call, 0, starts, 4, &d);
    skyrim_mod_patch_text(&d, 1, line, sizeof line);
    assert(strstr(line, "ゲストの再ビルドが必要です") && strstr(line, "character menu first-open precache"));
    assert(skyrim_mod_patch_catalog_count() == 4);
    return 0;
}
