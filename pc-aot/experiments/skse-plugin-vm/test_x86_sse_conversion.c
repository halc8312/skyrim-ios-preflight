/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef SKSE_NATIVE_SSE_REFERENCE
#include <xmmintrin.h>
#if defined(_WIN32) && !defined(_M_IX86)
#error The native Windows reference must be a 32-bit x86 process.
#endif
#else
#include "x86_call.h"
#endif

typedef enum { WIDEN, NARROW, PACKED } Conversion;

static const char *const mnemonics[] = {"CVTSS2SD", "CVTSD2SS", "CVTPD2PS"};
static const uint32_t initial[] = {0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff};

static const uint32_t singles[] = {
    0x3f800000, 0xbfc00000, 0x00000000, 0x80000000,
    0x00000001, 0x807fffff, 0x00800000, 0x7f7fffff,
    0x7f800000, 0xff800000,
    0x7fc12345, 0xffc12345, 0x7f801234, 0xff800001,
};

static const uint64_t doubles[] = {
    UINT64_C(0x3ff0000000000000), UINT64_C(0xbff8000000000000),
    UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
    UINT64_C(0x0000000000000001), UINT64_C(0x800fffffffffffff), // binary64 subnormals
    UINT64_C(0x3690000000000000), UINT64_C(0x3690000000000001), // tie and just above half of one binary32 subnormal
    UINT64_C(0xb690000000000000), UINT64_C(0x36a0000000000000), // negative zero tie and smallest binary32 subnormal
    UINT64_C(0x380fffffe0000000), UINT64_C(0x3810000000000000), // rounding into smallest binary32 normal
    UINT64_C(0x3ff000000fffffff), UINT64_C(0x3ff0000010000000), // just below tie and tie with an even lower result
    UINT64_C(0x3ff0000010000001), UINT64_C(0x3ff0000030000000), // just above tie and tie with an even upper result
    UINT64_C(0xbff0000030000000), UINT64_C(0x47efffffe0000000), // negative tie and largest finite binary32
    UINT64_C(0x47effffff0000000), UINT64_C(0x7ff0000000000000), // ties to infinity and input infinity
    UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8123456789abc), // negative infinity and quiet NaN
    UINT64_C(0xfff923456789abcd), UINT64_C(0x7ff0000000000001), // negative quiet NaN and tiny signaling payload
    UINT64_C(0xfff0123456789abc), UINT64_C(0x7ff0000100000000), // signed signaling payloads
};

#ifdef SKSE_NATIVE_SSE_REFERENCE
static void convert(Conversion operation, bool memory, const uint64_t input[2], uint8_t output[16]) {
    /* Default MXCSR masks exceptions and disables DAZ/FTZ, with round-to-nearest-even. */
    unsigned old = _mm_getcsr();
    _mm_setcsr(0x1f80);
#define RUN_SSE(op, preload, source) \
    __asm__ volatile("movdqu (%[initial]), %%xmm0\n\t" preload \
                     #op " " source ", %%xmm0\n\t" "movdqu %%xmm0, (%[output])" \
                     : : [initial] "r"(initial), [input] "r"(input), [output] "r"(output) \
                     : "xmm0", "xmm1", "memory")
#define CONVERT_MODE(op) do { \
    if (memory) RUN_SSE(op, "", "(%[input])"); \
    else RUN_SSE(op, "movdqu (%[input]), %%xmm1\n\t", "%%xmm1"); \
} while (0)
    switch (operation) {
    case WIDEN: CONVERT_MODE(cvtss2sd); break;
    case NARROW: CONVERT_MODE(cvtsd2ss); break;
    case PACKED: CONVERT_MODE(cvtpd2ps); break;
    }
#undef CONVERT_MODE
#undef RUN_SSE
    _mm_setcsr(old);
}
#else
static uint8_t scratch[256];
enum { IMAGE = 0x00100000, SCRATCH = 0x00200000, DATA_OFFSET = 0x80, OUTPUT_OFFSET = 32 };

static void instruction(uint8_t *bytes, size_t *used, uint8_t prefix, uint8_t opcode,
                        uint8_t operand, uint32_t address) {
    bytes[(*used)++] = prefix;
    bytes[(*used)++] = 0x0f;
    bytes[(*used)++] = opcode;
    bytes[(*used)++] = operand;
    if ((operand & 0xc7) == 0x05) { // mod=0, r/m=5 is a 32-bit absolute address.
        for (unsigned i = 0; i < 4; ++i) bytes[(*used)++] = (uint8_t)(address >> (8 * i));
    }
}

static SkseX86Result interpreted(Conversion operation, bool memory, const uint64_t input[2], bool truncated) {
    static const uint8_t prefixes[] = {0xf3, 0xf2, 0x66};
    static const uint32_t widths[] = {4, 8, 16};
    uint8_t image_bytes[DATA_OFFSET + 16] = {0};
    size_t used = 0;
    instruction(image_bytes, &used, 0xf3, 0x6f, 0x05, SCRATCH); // MOVDQU xmm0, [initial]
    if (!memory)
        instruction(image_bytes, &used, 0xf3, 0x6f, 0x0d, SCRATCH + 16); // MOVDQU xmm1, [input]
    instruction(image_bytes, &used, prefixes[operation], 0x5a, memory ? 0x05 : 0xc1, IMAGE + DATA_OFFSET);
    instruction(image_bytes, &used, 0xf3, 0x7f, 0x05, SCRATCH + OUTPUT_OFFSET); // MOVDQU [output], xmm0
    image_bytes[used++] = 0xc3; // RET

    memset(scratch, 0, sizeof scratch);
    memcpy(scratch, initial, sizeof initial);
    memcpy(scratch + 16, input, 16);
    memset(scratch + OUTPUT_OFFSET, 0xa5, 16);
    memcpy(image_bytes + DATA_OFFSET, input, sizeof image_bytes - DATA_OFFSET);
    uint32_t readable = widths[operation] - (truncated ? 1 : 0);
    SksePeImage image = {
        .base = IMAGE, .size = memory ? DATA_OFFSET + readable : (uint32_t)used, .bytes = image_bytes,
        .section_count = memory ? 2 : 1,
        .sections = {{0, (uint32_t)used, SKSE_PE_READ | SKSE_PE_EXEC}, {DATA_OFFSET, readable, SKSE_PE_READ}},
    };
    return skse_x86_call(&image, IMAGE, (SkseX86Scratch){SCRATCH, scratch, sizeof scratch}, NULL, 0, 30,
                         (SkseX86Services){0}, SKSE_X86_CDECL);
}

static void convert(Conversion operation, bool memory, const uint64_t input[2], uint8_t output[16]) {
    SkseX86Result result = interpreted(operation, memory, input, false);
    if (result.status != SKSE_X86_OK) {
        fprintf(stderr, "%s %s inputs %016" PRIx64 " %016" PRIx64 ": status %u at %08" PRIx32 "\n",
                mnemonics[operation], memory ? "mem" : "reg", input[0], input[1],
                (unsigned)result.status, result.fault_address);
    }
    assert(result.status == SKSE_X86_OK);
    memcpy(output, scratch + OUTPUT_OFFSET, 16);
}

static void check_boundaries(void) {
    const uint64_t input[] = {UINT64_C(0x3ff0000030000000), UINT64_C(0xb690000000000000)};
    for (Conversion operation = WIDEN; operation <= PACKED; ++operation) {
        SkseX86Result result = interpreted(operation, true, input, true);
        assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_address == IMAGE + DATA_OFFSET);
        for (unsigned i = 0; i < 16; ++i) assert(scratch[OUTPUT_OFFSET + i] == 0xa5);
    }
    // Check exact canonical values even on hosts without native SSE2.
    uint8_t out[16];
    uint32_t lanes[4];
    const uint64_t minimum[] = {1, 0}; // Smallest binary32 subnormal.
    convert(WIDEN, true, minimum, out);
    memcpy(lanes, out, sizeof lanes);
    assert(lanes[0] == 0 && lanes[1] == 0x36a00000);
    convert(NARROW, true, input, out);
    memcpy(lanes, out, sizeof lanes);
    assert(lanes[0] == 0x3f800002);
    convert(PACKED, true, input, out);
    memcpy(lanes, out, sizeof lanes);
    assert(lanes[0] == 0x3f800002 && lanes[1] == 0x80000000 && lanes[2] == 0 && lanes[3] == 0);
}
#endif

static void sample(Conversion operation, const uint64_t input[2]) {
    static const uint8_t packed_upper[8] = {0};
    for (unsigned memory = 0; memory <= 1; ++memory) {
        uint8_t out[16];
        convert(operation, memory, input, out);
        if (operation == PACKED) assert(!memcmp(out + 8, packed_upper, sizeof packed_upper));
        else {
            unsigned converted = operation == WIDEN ? 2 : 1;
            assert(!memcmp(out + converted * 4, initial + converted, (4 - converted) * 4));
        }
        uint32_t lanes[4];
        memcpy(lanes, out, sizeof lanes);
        printf("%s %s %016" PRIx64 " %016" PRIx64 " -> %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
               mnemonics[operation], memory ? "mem" : "reg", input[0], input[1], lanes[0], lanes[1], lanes[2], lanes[3]);
    }
}

int main(void) {
#ifndef SKSE_NATIVE_SSE_REFERENCE
    check_boundaries();
#endif
    for (size_t i = 0; i < sizeof singles / sizeof singles[0]; ++i) {
        _Alignas(16) const uint64_t input[2] = {UINT64_C(0x8765432100000000) | singles[i], UINT64_C(0x0123456789abcdef)};
        sample(WIDEN, input);
    }
    size_t count = sizeof doubles / sizeof doubles[0];
    for (size_t i = 0; i < count; ++i) {
        _Alignas(16) const uint64_t input[2] = {doubles[i], doubles[count - i - 1]};
        sample(NARROW, input);
        sample(PACKED, input);
    }
    return 0;
}
