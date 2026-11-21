/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_pex.h"

#include <assert.h>
#include <stdio.h>

typedef struct EffectNative {
    const char *name;
    const char *result;
    const char *arguments[2];
    uint8_t argc;
    bool global;
    unsigned static_calls;
} EffectNative;

static const char script[] = "hdtHighHeelEffect";

static void check_native(const SksePexInventory *inventory, const EffectNative *expected)
{
    const SksePexNative *native = NULL;
    for (size_t i = 0; i < inventory->native_count; ++i) {
        const SksePexNative *candidate = &inventory->natives[i];
        if (!skse_pex_text_equal(candidate->script, script) ||
            !skse_pex_text_equal(candidate->name, expected->name))
            continue;
        assert(!native);
        native = candidate;
    }
    assert(native && native->state.length == 0);
    assert(native->global == expected->global && native->count == expected->argc);
    assert(skse_pex_text_equal(native->result, expected->result));
    for (unsigned i = 0; i < expected->argc; ++i)
        assert(skse_pex_text_equal(native->arguments[i], expected->arguments[i]));

    unsigned calls = 0;
    for (size_t i = 0; i < inventory->call_count; ++i) {
        const SksePexStaticCall *call = &inventory->calls[i];
        if (!skse_pex_text_equal(call->script, script) || !skse_pex_text_equal(call->name, expected->name))
            continue;
        assert(call->count == expected->argc);
        ++calls;
    }
    assert(calls == expected->static_calls);
}

int main(void)
{
    uint8_t pex[8192];
    size_t length = fread(pex, 1, sizeof pex, stdin);
    assert(feof(stdin) && !ferror(stdin));
    SksePexNative natives[16];
    SksePexStaticCall calls[32];
    SksePexInventory inventory = {
        .natives = natives, .native_capacity = sizeof natives / sizeof natives[0],
        .calls = calls, .call_capacity = sizeof calls / sizeof calls[0],
    };
    assert(skse_pex_inspect(pex, length, &inventory) == SKSE_PEX_OK);
    static const EffectNative contract[] = {
        {"KeepUpdate", "None", {"ObjectReference", "Float"}, 2, true, 2},
        {"StopUpdate", "None", {"ObjectReference"}, 1, true, 1},
        {"GetMagnitude", "Float", {NULL}, 0, false, 0},
    };
    for (size_t i = 0; i < sizeof contract / sizeof contract[0]; ++i)
        check_native(&inventory, &contract[i]);
    assert(skse_pex_inspect(pex, length - 1, &inventory) != SKSE_PEX_OK);
    assert(inventory.native_count == 0 && inventory.call_count == 0);
    puts("Classic HighHeels effect PEX matches the typed declarations and update call arities");
    return 0;
}
