/* SPDX-License-Identifier: GPL-3.0-only */
#include "skse_compat.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct { unsigned lookups; } CPU;
static uint32_t memory[16];
#define M32(address) memory[(address)/4]
#define M8(address) (((unsigned char *)memory)[address])
enum { SETTING = 8, RESULT = 32, QUERY = 100, NAME, VALUE, INTERNED };
static const char *stored_name, *stored_text, *returned_text;
static int found;

static const char *str(uint32_t address) {
    switch (address) {
    case QUERY: return "bLookupName:Fixture";
    case NAME: return stored_name;
    case VALUE: return stored_text;
    default: assert(0); return NULL;
    }
}

static uint32_t skse_ini_setting(CPU *cpu, const char *key) {
    assert(strcmp(key, "bLookupName:Fixture") == 0);
    cpu->lookups++;
    return found ? SETTING : 0;
}

static void vm_string(CPU *cpu, uint32_t result, const char *text) {
    assert(cpu && result == RESULT && text);
    returned_text = text;
    M32(RESULT + 4) = INTERNED;
}
#include "classic_skse_ini.h"

static CPU cpu;
static uint32_t run(const char *function, unsigned result_type, uint32_t value) {
    unsigned count = 0;
    const SkseNative *table = skse_native_table(&count), *fn = NULL;
    for (unsigned i = 0; i < count; i++) {
        if (!strcmp(table[i].class_name, "Utility") && !strcmp(table[i].name, function)) {
            assert(!fn);
            fn = &table[i];
        }
    }
    assert(fn && fn->result == result_type && fn->args[0] == 2);
    assert(fn->argc == (result_type ? 1 : 2));
    uint32_t arguments[] = {QUERY, value};
    M32(RESULT) = fn->result;
    M32(RESULT + 4) = 0;
    returned_text = NULL;
    unsigned lookups = cpu.lookups;
    skse_classic_ini_dispatch(&cpu, fn->operation, arguments, RESULT);
    assert(cpu.lookups == lookups + 1 && M32(RESULT) == result_type);
    return M32(RESULT + 4);
}

int main(void) {
    found = 1;
    M32(SETTING + 8) = NAME;
    stored_name = "bEnabled";
    const unsigned char bool_bytes[] = {0, 1, 2, 255};
    for (unsigned i = 0; i < sizeof bool_bytes; i++) {
        M32(SETTING + 4) = UINT32_MAX;
        M8(SETTING + 4) = bool_bytes[i];
        assert(run("GetINIBool", 5, 0) == (bool_bytes[i] != 0));
        assert(run("GetINIInt", 3, 0) == 0 && run("GetINIFloat", 4, 0) == 0);
    }
    stored_name = "iSigned";
    M32(SETTING + 4) = (uint32_t)(int32_t)-23;
    assert(run("GetINIInt", 3, 0) == (uint32_t)(int32_t)-23);
    assert(run("GetINIBool", 5, 0) == 0 && run("GetINIFloat", 4, 0) == 0);
    stored_name = "fFraction";
    M32(SETTING + 4) = UINT32_C(0x40200000); /* 2.5 */
    assert(run("GetINIFloat", 4, 0) == UINT32_C(0x40200000));
    assert(run("GetINIInt", 3, 0) == 0 && run("GetINIBool", 5, 0) == 0);
    run("SetINIFloat", 0, UINT32_C(0x80000000));
    assert(run("GetINIFloat", 4, 0) == UINT32_C(0x80000000));
    const char *names[] = {"sLanguage", "SPath"};
    const char *values[] = {"English", "日本語", ""};
    for (unsigned n = 0; n < 2; ++n) {
        stored_name = names[n];
        for (unsigned i = 0; i < 3; ++i) {
            stored_text = values[i];
            M32(SETTING + 4) = VALUE;
            assert(run("GetINIString", 2, 0) == INTERNED && !strcmp(returned_text, values[i]));
        }
        M32(SETTING + 4) = 0;
        assert(run("GetINIString", 2, 0) == 0 && !returned_text);
    }
    const char *bad[] = {"BWrongCase", "cUnknown", "uUnsigned", "", "rID"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; ++i) {
        stored_name = bad[i];
        M32(SETTING + 4) = UINT32_MAX;
        assert(run("GetINIBool", 5, 0) == 0 && run("GetINIString", 2, 0) == 0);
        assert(run("GetINIInt", 3, 0) == 0 && run("GetINIFloat", 4, 0) == 0);
        run("SetINIFloat", 0, 123);
        assert(M32(SETTING + 4) == UINT32_MAX);
    }
    M32(SETTING + 8) = 0;
    assert(run("GetINIBool", 5, 0) == 0 && run("GetINIString", 2, 0) == 0);
    found = 0;
    assert(run("GetINIBool", 5, 0) == 0 && run("GetINIString", 2, 0) == 0);
    return 0;
}
