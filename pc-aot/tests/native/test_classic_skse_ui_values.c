/* SPDX-License-Identifier: GPL-3.0-only */
#include "skse_compat.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { RESULT = 16, MENU = 40, PATH = 64, VIEW = 100, VALUE = 128, TEXT = 192, MANAGED = 224 };
typedef struct {
    uint32_t view, success, allocations, reads, cleanups, frees, strings;
    uint32_t tagged;
    unsigned char payload[8];
} CPU;
static CPU cpu;
static uint32_t memory[128];
#define GPTR(at) ((unsigned char *)memory + (at))
#define M8(at) (*GPTR(at))
static uint32_t *word(uint32_t at) {
    assert(at && !(at % 4) && at <= sizeof memory - 4);
    return &memory[at / 4];
}
#define M32(at) (*word(at))
static double read_number(uint32_t at) { double n; memcpy(&n, GPTR(at), 8); return n; }
#define MF64(at) read_number(at)
static uint32_t float_bits(float f) { uint32_t n; memcpy(&n, &f, 4); return n; }
static const char *str(uint32_t at) { assert(at < sizeof memory); return (const char *)GPTR(at); }
static uint32_t skse_menu_view(const char *name) {
    assert(name == str(MENU));
    return cpu.view;
}
static uint32_t gmalloc(unsigned size) {
    assert(size == 16 && cpu.frees == cpu.allocations++);
    return VALUE;
}
static uint32_t skse_virtual(CPU *state, uint32_t view, unsigned slot, int count, const uint32_t *args) {
    assert(state == &cpu && view == VIEW && slot == 17 && count == 2);
    assert(args[0] == VALUE && args[1] == PATH && M32(VALUE + 4) == 0);
    ++cpu.reads;
    M32(VALUE + 4) = cpu.tagged;
    memcpy(GPTR(VALUE + 8), cpu.payload, 8);
    return cpu.success;
}
static void skse_gfx_clean(CPU *state, uint32_t value) {
    assert(state == &cpu && value == VALUE && cpu.allocations == cpu.cleanups + 1);
    ++cpu.cleanups;
    memset(GPTR(value), 0, 16);
}
static void gfree(uint32_t at) { assert(at == VALUE && ++cpu.frees == cpu.cleanups); }
static void vm_string(CPU *state, uint32_t result, const char *text) {
    assert(state == &cpu && result == RESULT && M32(result) == 2);
    assert(text == str(TEXT));
    ++cpu.strings;
    M32(result + 4) = TEXT;
}
#include "classic_skse_ui_values.h"

static uint32_t call(unsigned op, uint32_t menu, uint32_t path) {
    unsigned count;
    const SkseNative *table = skse_native_table(&count), *fn = NULL;
    for (unsigned i = 0; i < count; ++i) {
        if (table[i].operation != op) continue;
        assert(!fn);
        fn = &table[i];
    }
    assert(fn && !strcmp(fn->class_name, "UI") && fn->argc == 2 && fn->args[0] == 2 && fn->args[1] == 2);
    uint32_t args[4] = {menu, path};
    M32(RESULT) = fn->result;
    M32(RESULT + 4) = 0;
    skse_classic_ui_read(&cpu, op, args, RESULT);
    assert(M32(RESULT) == fn->result && cpu.reads == cpu.cleanups && cpu.cleanups == cpu.frees);
    return M32(RESULT + 4);
}
static void checks(uint32_t boolean, uint32_t integer, uint32_t real, uint32_t text) {
    assert(call(S_UI_GBOOL, MENU, PATH) == boolean);
    assert(call(S_UI_GINT, MENU, PATH) == integer);
    assert(call(S_UI_GFLOAT, MENU, PATH) == real);
    assert(call(S_UI_GSTRING, MENU, PATH) == text);
}

int main(void) {
    assert(call(S_UI_GSTRING, 0, PATH) == 0);
    assert(call(S_UI_GSTRING, MENU, 0) == 0);
    assert(call(S_UI_GSTRING, MENU, PATH) == 0 && cpu.reads == 0);
    cpu.view = VIEW;
    cpu.success = 0xab0001;
    cpu.tagged = 2;
    cpu.payload[0] = 1;
    checks(1, 0, 0, 0);
    cpu.payload[0] = 0;
    checks(0, 0, 0, 0);
    const struct { double number; uint32_t integer; } samples[] = {
        {0., 0}, {-0., 0}, {-.9, 0}, {1.75, 1}, {-1.75, UINT32_MAX},
        {2147483650., UINT32_C(0x80000002)}, {-2147483649., UINT32_C(0x7fffffff)},
        {4294967295.75, UINT32_MAX}, {4294967297., 1}, {-4294967297., UINT32_MAX},
        {0x1p63, 0}, {-0x1p63, 0}, {0x1p70, 0}, {INFINITY, 0}, {-INFINITY, 0}, {NAN, 0}
    };
    cpu.tagged = 3;
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; ++i) {
        memcpy(cpu.payload, &samples[i].number, 8);
        checks(0, samples[i].integer, float_bits((float)samples[i].number), 0);
    }
    uint32_t address = TEXT;
    strcpy((char *)GPTR(TEXT), "Fixture");
    memcpy(cpu.payload, &address, 4);
    cpu.tagged = 4;
    checks(0, 0, 0, TEXT); /* direct string */
    address = MANAGED;
    M32(MANAGED) = TEXT;
    memcpy(cpu.payload, &address, 4);
    cpu.tagged = 0x44;
    checks(0, 0, 0, TEXT); /* indirect string */
    cpu.success = 0x123400; /* GetVariable may leave a value on failure. */
    checks(0, 0, 0, 0);
    cpu.success = 1;
    M32(MANAGED) = 0;
    checks(0, 0, 0, 0);
    memset(cpu.payload, 0, 8);
    checks(0, 0, 0, 0);
    cpu.tagged = 4;
    checks(0, 0, 0, 0);
    const unsigned unrelated[] = {0, 1, 5, 6, 7, 8, 0x46, 0x83};
    for (unsigned i = 0; i < sizeof unrelated / sizeof unrelated[0]; ++i) {
        cpu.tagged = unrelated[i];
        checks(0, 0, 0, 0);
    }
    assert(cpu.strings == 2);
    return 0;
}
