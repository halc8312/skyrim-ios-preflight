/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Classic 1.7.3, 32-bit VMClassRegistry and VMValue::ArrayData. The VM
 * allocator creates its reference count and constructs the elements. */
enum {
    CLASSIC_CREATE_ARRAY_SLOT = 0x15, CLASSIC_ARRAY_HEADER = 0x14,
    CLASSIC_VM_VALUE_SIZE = 8, CLASSIC_VM_STRING = 2, CLASSIC_VM_INT = 3,
    CLASSIC_VM_FLOAT = 4, CLASSIC_VM_BOOL = 5
};

static int skse_classic_array_fits(uint32_t address, uint32_t count) {
    uint64_t end = (uint64_t)address + CLASSIC_ARRAY_HEADER + (uint64_t)count * CLASSIC_VM_VALUE_SIZE;
    return end <= UINT64_C(0x100000000);
}

static void skse_classic_array_none(CPU *cpu, uint32_t result) {
    vm_clear(cpu, result);
    M32(result) = 0;
    M32(result + 4) = 0;
}

/* Scratch holds a lifetime-managed result, the CreateArray output pointer,
 * and one borrowed scalar for repeated string fills. Publication happens
 * after copying the arguments so result can also be the input VMValue. */
static uint32_t skse_classic_new_array(CPU *cpu, uint32_t registry, unsigned type, uint32_t count) {
    if (!registry || !count || !skse_classic_array_fits(1, count)) return 0;
    uint32_t scratch = gmalloc(20);
    if (!scratch) return 0;
    memset(GPTR(scratch), 0, 20);
    uint32_t call[] = {scratch, count, scratch + 8};
    skse_virtual(cpu, registry, CLASSIC_CREATE_ARRAY_SLOT, 3, call);
    uint32_t data = M32(scratch + 8);
    if (data) {
        M32(scratch) = type;
        M32(scratch + 4) = data;
        if (skse_classic_array_fits(data, count) && M32(data + 8) == count) return scratch;
    }
    vm_clear(cpu, scratch);
    gfree(scratch);
    return 0;
}

static void skse_classic_array_publish(CPU *cpu, uint32_t result, uint32_t scratch) {
    skse_classic_array_none(cpu, result);
    if (scratch) {
        M32(result) = M32(scratch);
        M32(result + 4) = M32(scratch + 4);
        /* Transfer the VM's one reference. The borrowed scalar owns none. */
        gfree(scratch);
    }
}

static uint32_t skse_classic_array_numeric(unsigned target, unsigned source, uint32_t bits) {
    if (target == source) return bits;
    if (source == CLASSIC_VM_BOOL) {
        bits = (uint8_t)bits != 0;
        source = CLASSIC_VM_INT;
    }
    if (target == CLASSIC_VM_FLOAT && source == CLASSIC_VM_INT)
        return float_bits((float)(int32_t)bits);
    if (target == CLASSIC_VM_INT && source == CLASSIC_VM_INT) return bits;
    if (target == CLASSIC_VM_INT && source == CLASSIC_VM_FLOAT) {
        float number = as_float(bits);
        if (!(number >= -0x1p31f && number < 0x1p31f)) return UINT32_C(0x80000000);
        return (uint32_t)(int32_t)number;
    }
    return 0;
}

static uint32_t skse_classic_fill_array(CPU *cpu, uint32_t registry, const SkseNative *fn,
        const uint32_t *args) {
    int resize = fn->operation == S_ARRAY_RESIZE;
    uint32_t count = args[resize ? 1 : 0];
    uint32_t source = resize ? args[0] : 0;
    uint32_t retained = 0;
    if (source) {
        if (!skse_classic_array_fits(source, 0)) return 0;
        uint32_t available = M32(source + 8);
        retained = available < count ? available : count;
        if (!skse_classic_array_fits(source, retained)) return 0;
    }
    uint32_t scratch = skse_classic_new_array(cpu, registry, fn->result, count);
    if (!scratch) return 0;
    unsigned element = fn->result - 10;
    uint32_t fallback = scratch + 12;
    M32(fallback) = element;
    M32(fallback + 4) = args[resize ? 2 : 1];
    uint32_t data = M32(scratch + 4) + CLASSIC_ARRAY_HEADER;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t value = i < retained ? source + CLASSIC_ARRAY_HEADER + i * CLASSIC_VM_VALUE_SIZE : fallback;
        uint32_t out = data + i * CLASSIC_VM_VALUE_SIZE;
        unsigned source_type = M32(value);
        if (element == CLASSIC_VM_STRING && source_type == CLASSIC_VM_STRING) {
            vm_copy(cpu, out, value); /* VM retains each interned string. */
        } else {
            if (M32(out)) vm_clear(cpu, out);
            M32(out) = element;
            M32(out + 4) = skse_classic_array_numeric(element, source_type, M32(value + 4));
        }
    }
    return scratch;
}

static unsigned skse_classic_split_fold(unsigned c) {
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static const unsigned char *skse_classic_token(const unsigned char **cursor,
        const unsigned char separators[256], size_t *length) {
    const unsigned char *start = *cursor;
    while (*start && separators[skse_classic_split_fold(*start)]) ++start;
    const unsigned char *end = start;
    while (*end && !separators[skse_classic_split_fold(*end)]) ++end;
    *cursor = end;
    *length = (size_t)(end - start);
    return start;
}

static uint32_t skse_classic_split_array(CPU *cpu, uint32_t registry, unsigned type, const uint32_t *args) {
    if (!args[0] || !args[1]) return 0;
    const unsigned char *input = (const unsigned char *)str(args[0]);
    const unsigned char *delimiter = (const unsigned char *)str(args[1]);
    unsigned char separators[256] = {0};
    for (; *delimiter; ++delimiter) separators[skse_classic_split_fold(*delimiter)] = 1;
    const unsigned char *cursor = input;
    uint32_t count = 0;
    size_t length = 0, longest = 0;
    while (*cursor) {
        skse_classic_token(&cursor, separators, &length);
        if (length) {
            ++count;
            if (length > longest) longest = length;
        }
    }
    if (!count || longest == SIZE_MAX) return 0;
    char *text = malloc(longest + 1);
    if (!text) return 0;
    uint32_t scratch = skse_classic_new_array(cpu, registry, type, count);
    if (scratch) {
        uint32_t out = M32(scratch + 4) + CLASSIC_ARRAY_HEADER;
        cursor = input;
        for (uint32_t i = 0; i < count; ++i) {
            const unsigned char *token = skse_classic_token(&cursor, separators, &length);
            memcpy(text, token, length);
            text[length] = '\0';
            vm_string(cpu, out + i * CLASSIC_VM_VALUE_SIZE, text);
        }
    }
    free(text);
    return scratch;
}

static void skse_classic_array_run(CPU *cpu, const SkseNative *fn, const uint32_t *args,
        uint32_t registry, uint32_t result) {
    uint32_t scratch = fn->operation == S_STRING_SPLIT
        ? skse_classic_split_array(cpu, registry, fn->result, args)
        : skse_classic_fill_array(cpu, registry, fn, args);
    skse_classic_array_publish(cpu, result, scratch);
}
