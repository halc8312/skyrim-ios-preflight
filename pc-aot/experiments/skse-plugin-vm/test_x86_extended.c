/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_call.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { Image = 0x00100000, Scratch = 0x00200000, Values = 0x00400000 };
static uint8_t scratch[4096];

static void word(uint8_t *out, unsigned width, uint64_t value) {
    for (unsigned i = 0; i < width; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

static uint64_t quad(const uint8_t *bytes) {
    uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= (uint64_t)bytes[i] << (i * 8);
    return bits;
}

static SkseX86Result run(uint8_t *code, size_t length, uint64_t left, uint64_t right) {
    uint8_t values[16];
    word(values, 8, left);
    word(values + 8, 8, right);
    const SkseX86ReadView view = {Values, values, sizeof values};
    const SkseX86Services services = {.read_views = &view, .read_view_count = 1};
    SksePeImage image = {
        .base = Image, .size = (uint32_t)length, .bytes = code, .section_count = 1,
        .sections = {{0, (uint32_t)length, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    memset(scratch, 0, sizeof scratch);
    return skse_x86_call(&image, Image, (SkseX86Scratch){Scratch, scratch, sizeof scratch},
                         NULL, 0, 100, services, SKSE_X86_CDECL);
}

static uint64_t guest_binary(unsigned group, uint64_t left, uint64_t right) {
    uint8_t code[] = {
        0xdd, 0x05, 0, 0, 0, 0,              /* FLD m64 */
        0xdc, 0x05, 0, 0, 0, 0,              /* arithmetic m64 */
        0xdd, 0x1d, 0, 0, 0, 0, 0xc3        /* FSTP m64, RET */
    };
    code[7] = (uint8_t)(0x05 | group << 3);
    word(code + 2, 4, Values);
    word(code + 8, 4, Values + 8);
    word(code + 14, 4, Scratch);
    const SkseX86Result result = run(code, sizeof code, left, right);
    assert(result.status == SKSE_X86_OK);
    return quad(scratch);
}

static void status_and_precision(void) {
    /* A double-rounded interpreter would drop the 1 in this expression. */
    uint8_t cancel[] = {
        0xdd, 0x05, 0, 0, 0, 0, 0xdc, 0x05, 0, 0, 0, 0,
        0xdc, 0x25, 0, 0, 0, 0, 0xdd, 0x1d, 0, 0, 0, 0, 0xc3
    };
    word(cancel + 2, 4, Values);
    word(cancel + 8, 4, Values + 8);
    word(cancel + 14, 4, Values);
    word(cancel + 20, 4, Scratch);
    const uint64_t one = UINT64_C(0x3ff0000000000000), big = UINT64_C(0x43e0000000000000);
    assert(guest_binary(0, one, one) == UINT64_C(0x4000000000000000));
    assert(run(cancel, sizeof cancel, big, one).status == SKSE_X86_OK && quad(scratch) == one);

    uint8_t compare[] = {0xdd, 0x05, 0, 0, 0, 0, 0xdd, 0x05, 0, 0, 0, 0, 0xda, 0xe9, 0xdf, 0xe0, 0xc3};
    word(compare + 2, 4, Values + 8);
    word(compare + 8, 4, Values);
    static const uint64_t right[] = {
        UINT64_C(0x4000000000000000), UINT64_C(0x3ff0000000000000),
        UINT64_C(0xbff0000000000000), UINT64_C(0x7ff8000000000031)
    };
    static const uint16_t expected[] = {0x0100, 0x4000, 0, 0x4500};
    for (unsigned i = 0; i < sizeof right / sizeof right[0]; ++i) {
        const SkseX86Result result = run(compare, sizeof compare, one, right[i]);
        assert(result.status == SKSE_X86_OK && (result.eax & 0x7d00) == expected[i]);
    }
    uint8_t underflow[] = {0xdd, 0xd8, 0xc3};
    assert(run(underflow, sizeof underflow, 0, 0).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
    uint8_t overflow[] = {0xd9, 0xee, 0xd9, 0xee, 0xd9, 0xee, 0xd9, 0xee, 0xd9, 0xee,
                          0xd9, 0xee, 0xd9, 0xee, 0xd9, 0xee, 0xd9, 0xee, 0xc3};
    assert(run(overflow, sizeof overflow, 0, 0).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
}

#if defined(__i386__) || defined(__x86_64__)
static uint64_t native_binary(unsigned group, uint64_t left, uint64_t right) {
    uint64_t result;
#define COMPARE(op) do { \
    __asm__ volatile("fldl %1\n\t" op " %2\n\tfstpl %0" : "=m"(result) : "m"(left), "m"(right) : "st"); \
    return result; \
} while (0)
    switch (group) {
    case 0: COMPARE("faddl");
    case 1: COMPARE("fmull");
    case 4: COMPARE("fsubl");
    case 5: COMPARE("fsubrl");
    default: assert(0); return 0;
    }
#undef COMPARE
}

static void same(unsigned group, uint64_t left, uint64_t right) {
    const uint64_t guest = guest_binary(group, left, right);
    const uint64_t native = native_binary(group, left, right);
    if (guest != native)
        fprintf(stderr, "x87 group=%u left=%016" PRIx64 " right=%016" PRIx64
                " interpreter=%016" PRIx64 " native=%016" PRIx64 "\n", group, left, right, guest, native);
    assert(guest == native);
}

static void compare_with_native(void) {
    uint16_t old_control;
    const uint16_t default_control = 0x037f;
    __asm__ volatile("fnstcw %0\n\tfnclex\n\tfldcw %1" : "=m"(old_control) : "m"(default_control));
    static const uint64_t edges[] = {
        0, UINT64_C(0x8000000000000000), 1, UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000),
        UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000001), UINT64_C(0x3fefffffffffffff),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff)
    };
    static const unsigned groups[] = {0, 1, 4, 5};
    uint64_t rng = UINT64_C(0x77b11234f00dcafe);
    for (unsigned k = 0; k < sizeof groups / sizeof groups[0]; ++k) {
        for (size_t i = 0; i < sizeof edges / sizeof edges[0]; ++i)
            for (size_t j = 0; j < sizeof edges / sizeof edges[0]; ++j)
                same(groups[k], edges[i], edges[j]);
        for (unsigned i = 0; i < 8192; ++i) {
            rng = rng * UINT64_C(6364136223846793005) + 1;
            uint64_t left = rng & ~UINT64_C(0x0010000000000000); // Keep arithmetic finite.
            rng = rng * UINT64_C(6364136223846793005) + 1;
            uint64_t right = rng & UINT64_C(0x800fffffffffffff);
            if (i % 2) right |= left & UINT64_C(0x7ff0000000000000);
            else right |= rng & UINT64_C(0x7fe0000000000000);
            same(groups[k], left, right);
        }
    }
    __asm__ volatile("fnclex\n\tfldcw %0" : : "m"(old_control));
}
#endif

int main(void) {
    status_and_precision();
#if defined(__i386__) || defined(__x86_64__)
    compare_with_native();
#endif
}
