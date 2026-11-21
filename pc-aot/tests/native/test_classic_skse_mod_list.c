/* SPDX-License-Identifier: GPL-3.0-only */
#include "skse_compat.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

typedef struct { const char *value; char interned[0x104]; bool name_query; } CPU;
static CPU cpu;
static uint32_t memory[0x17000 / 4], handler_slot;
static unsigned handler_reads;
enum {
    RESULT = 0x10010, QUERY = 0x10030, HANDLER = 0x10400, NODES = 0x10c00, FILE_A = 0x12000,
    FILE_B = 0x12500, FILE_C = 0x12a00, MASTERS = 0x13000, AUTHOR = 0x13100, DESC = 0x13200
};
static uint32_t *word_at(uint32_t address) {
    if (address == 0x012e2cb8) {
        ++handler_reads;
        return &handler_slot;
    }
    assert(address && address % 4 == 0 && address <= sizeof memory - 4);
    return &memory[address / 4];
}
#define M32(address) (*word_at(address))
#define M8(address) (((uint8_t *)memory)[(address)])
static const char *str(uint32_t address) {
    assert(address < sizeof memory);
    return address ? (const char *)memory + address : "";
}
static void vm_string(CPU *state, uint32_t result, const char *text) {
    assert(state == &cpu && result == RESULT && text);
    if (state->name_query)
        assert((uintptr_t)text < (uintptr_t)memory || (uintptr_t)text >= (uintptr_t)memory + sizeof memory);
    assert(strlen(text) < sizeof state->interned);
    strcpy(state->interned, text);
    state->value = state->interned;
    M32(result + 4) = 1;
}
#include "classic_skse_mod_list.h"

static void put(uint32_t at, const char *text) {
    assert(at + strlen(text) < sizeof memory);
    memcpy((char *)memory + at, text, strlen(text) + 1);
}
static uint32_t call(const char *name, uint32_t a, uint32_t b, int success) {
    unsigned count;
    const SkseNative *table = skse_native_table(&count), *fn = NULL;
    for (unsigned i = 0; i < count; ++i) {
        if (!strcmp(table[i].class_name, "Game") && !strcmp(table[i].name, name)) {
            assert(!fn);
            fn = &table[i];
        }
    }
    unsigned expected_args = !strcmp(name, "GetModCount") ? 0 : !strcmp(name, "GetNthModDependency") ? 2 : 1;
    unsigned string_result = !strcmp(name, "GetModName") || !strcmp(name, "GetModAuthor") || !strcmp(name, "GetModDescription");
    assert(fn && fn->argc == expected_args && fn->result == (string_result ? 2 : 3));
    if (expected_args) assert(fn->args[0] == (!strcmp(name, "GetModByName") ? 2 : 3));
    if (expected_args == 2) assert(fn->args[1] == 3);
    uint32_t args[4] = {a, b};
    cpu.value = NULL;
    cpu.name_query = !strcmp(name, "GetModName");
    M32(RESULT) = fn->result;
    M32(RESULT + 4) = 0;
    assert(skse_classic_mod_query(&cpu, fn->operation, args, RESULT) == success);
    assert(M32(RESULT) == fn->result);
    return M32(RESULT + 4);
}

int main(void) {
    handler_slot = HANDLER;
    uint32_t head = HANDLER + CLASSIC_MOD_LIST;
    M32(head) = FILE_A;
    M32(head + 4) = NODES;
    M32(NODES) = FILE_B;
    M32(NODES + 4) = NODES + 8;
    M32(NODES + 8) = FILE_C;
    M32(HANDLER + CLASSIC_LOADED_COUNT) = 2;
    M32(HANDLER + CLASSIC_LOADED_MODS) = FILE_A;
    M32(HANDLER + CLASSIC_LOADED_MODS + 4) = FILE_B;
    put(FILE_A + CLASSIC_FILE_NAME, "Skyrim.esm");
    put(FILE_B + CLASSIC_FILE_NAME, "Fixture_A.esp");
    put(FILE_C + CLASSIC_FILE_NAME, "Fixture_B.esp");
    put(AUTHOR, "試験用");
    put(DESC, "Second fixture");
    M32(FILE_B + CLASSIC_FILE_AUTHOR) = AUTHOR;
    M32(FILE_B + CLASSIC_FILE_DESCRIPTION) = DESC;
    M8(FILE_A + CLASSIC_FILE_INDEX) = 0;
    M8(FILE_B + CLASSIC_FILE_INDEX) = 1;
    M8(FILE_C + CLASSIC_FILE_INDEX) = UINT8_MAX;
    M32(FILE_C + CLASSIC_FILE_MASTER_COUNT) = 2;
    M32(FILE_C + CLASSIC_FILE_MASTERS) = MASTERS;
    M32(MASTERS) = FILE_B;
    M32(MASTERS + 4) = FILE_A;

    assert(call("GetModCount", 0, 0, 1) == 2);
    put(QUERY, "fixture_b.ESP");
    assert(call("GetModByName", QUERY, 0, 1) == 2);
    assert(call("GetModName", 0, 0, 1) && !strcmp(cpu.value, "Skyrim.esm"));
    /* This file lives only in the SKSE linked-list namespace. */
    assert(call("GetModName", 2, 0, 1) && !strcmp(cpu.value, "Fixture_B.esp"));
    assert(call("GetModAuthor", 1, 0, 1) && !strcmp(cpu.value, "試験用"));
    assert(call("GetModDescription", 1, 0, 1) && !strcmp(cpu.value, "Second fixture"));
    assert(call("GetModDescription", 0, 0, 1) && cpu.value && !cpu.value[0]);
    assert(call("GetModName", 3, 0, 1) == 0 && !cpu.value);
    assert(call("GetModAuthor", UINT32_MAX, 0, 1) == 0 && !cpu.value);
    assert(call("GetModDependencyCount", 2, 0, 1) == 2);
    assert(call("GetNthModDependency", 2, 0, 1) == 1);
    assert(call("GetNthModDependency", 2, 1, 1) == 0);
    assert(call("GetNthModDependency", 2, UINT32_MAX, 1) == 0);
    M32(FILE_C + CLASSIC_FILE_MASTERS) = UINT32_MAX;
    call("GetNthModDependency", 2, 0, 0);
    M32(FILE_C + CLASSIC_FILE_MASTERS) = MASTERS;
    M32(MASTERS) = 0;
    call("GetNthModDependency", 2, 0, 0);
    put(QUERY, "missing");
    assert(call("GetModByName", QUERY, 0, 1) == UINT8_MAX);
    assert(call("GetModByName", 0, 0, 1) == UINT8_MAX);
    /* A Classic list stops on the first empty item, even with a following node. */
    M32(NODES) = 0;
    assert(call("GetModName", 2, 0, 1) == 0);
    put(QUERY, "fixture_b.esp");
    assert(call("GetModByName", QUERY, 0, 1) == UINT8_MAX);

    /* The PC lookup narrows a list position to UInt8, including beyond 255.
     * Direct property queries independently reject indices greater than 255. */
    M32(head) = FILE_A;
    for (unsigned i = 0; i < 256; ++i) {
        M32(NODES + 8 * i) = i == 255 ? FILE_C : FILE_B;
        M32(NODES + 8 * i + 4) = i == 255 ? 0 : NODES + 8 * (i + 1);
    }
    assert(call("GetModByName", QUERY, 0, 1) == 0);
    assert(call("GetModName", 255, 0, 1) && !strcmp(cpu.value, "Fixture_A.esp"));
    assert(call("GetModName", 256, 0, 1) == 0);
    M32(NODES + 4) = NODES;
    call("GetModByName", QUERY, 0, 0);
    M32(NODES + 4) = 0;
    memset((uint8_t *)memory + FILE_A + CLASSIC_FILE_NAME, 'A', CLASSIC_FILE_NAME_BYTES);
    call("GetModName", 0, 0, 0);
    call("GetModByName", QUERY, 0, 0);
    M32(head) = 0;
    assert(call("GetModName", 0, 0, 1) == 0 && !cpu.value);
    M32(head) = 4; // Not a mapped TESFile.
    assert(call("GetModName", 0, 0, 0) == 0 && !cpu.value);
    handler_slot = 0;
    handler_reads = 0;
    assert(call("GetModName", UINT32_MAX, 0, 1) == 0 && !cpu.value && !handler_reads);
    call("GetModCount", 0, 0, 0);
    return 0;
}
