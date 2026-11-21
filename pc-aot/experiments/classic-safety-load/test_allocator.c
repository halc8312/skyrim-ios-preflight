/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum { STACK = 0x12F00, HEAP_OWNER = 0x12400, ALLOCATION_SIZE = 1238, ORIGINAL_RESULT = 0x73100,
       PLAIN_RESULT = 0x74100, ALIGNED_RESULT = 0x75100, ALIGNMENT = 32 };
uint8_t *MEM;
static bool enabled, loading, chargen, original_allows_fallback, refuse_next_allocation;
static unsigned checks, reclaims, plain, aligned;
static uint32_t expected_mode, expected_size;

void baseline_allocator(CPU *c);
void staged_allocator(CPU *c);
bool skyrim_safety_load_bypass_reclamation(void);
void f_A48C80(CPU *c);
void f_F568FA(CPU *c);
void f_F587F5(CPU *c);
void f_A4EE00(CPU *c);

void f_A4EE00(CPU *c) { (void)c; abort(); } /* Fixture disables the separate small pool. */

bool skyrim_safety_load_bypass_reclamation(void) {
    ++checks;
    return enabled && loading && !chargen;
}

void f_A48C80(CPU *c) {
    uint32_t sp = c->r[4];
    assert(M32(sp) == 0xA48EAE && c->r[1] == c->r[5] && M32(sp + 8) == expected_mode);
    assert(M32(sp + 12) == STACK + 0x28);
    M8(M32(sp + 12)) = original_allows_fallback;
    c->r[0] = ORIGINAL_RESULT;
    c->r[4] += 16;
    ++reclaims;
}

void f_F568FA(CPU *c) {
    assert(M32(c->r[4]) == 0xA48ED6 && M32(c->r[4] + 4) == expected_size);
    c->r[0] = refuse_next_allocation ? 0 : PLAIN_RESULT;
    refuse_next_allocation = false;
    c->r[4] += 4;
    ++plain;
}

void f_F587F5(CPU *c) {
    assert(M32(c->r[4]) == 0xA48ECB && M32(c->r[4] + 4) == expected_size);
    assert(M32(c->r[4] + 8) == ALIGNMENT);
    c->r[0] = refuse_next_allocation ? 0 : ALIGNED_RESULT;
    refuse_next_allocation = false;
    c->r[4] += 4;
    ++aligned;
}

static CPU run(void (*entry)(CPU *), uint32_t mode, uint32_t size, bool uses_alignment) {
    CPU c = {0};
    expected_mode = mode;
    expected_size = size;
    c.r[0] = 0x1234;
    c.r[1] = 0x2345;
    c.r[3] = 0x3456;
    c.r[4] = STACK;
    c.r[5] = HEAP_OWNER;
    c.r[6] = size;
    M32(STACK + 0x1C) = mode;
    M32(STACK + 0x28) = 0;
    M32(STACK + 0x2C) = ALIGNMENT;
    M32(STACK + 0x30) = uses_alignment;
    entry(&c);
    assert(c.r[4] == STACK);
    return c;
}

static void expect_unchanged(uint32_t mode, uint32_t size, bool uses_alignment, bool checks_gate) {
    checks = reclaims = plain = aligned = 0;
    CPU before = run(baseline_allocator, mode, size, uses_alignment);
    uint32_t original = M32(STACK + 0x1C);
    CPU after = run(staged_allocator, mode, size, uses_alignment);
    assert(!memcmp(before.r, after.r, sizeof(before.r)) &&
           !memcmp(&before.flags, &after.flags, sizeof(before.flags)));
    assert(M32(STACK + 0x1C) == original && reclaims == 2 && checks == checks_gate);
    assert(plain == 2 * (original_allows_fallback && !uses_alignment));
    assert(aligned == 2 * (original_allows_fallback && uses_alignment));
}

int main(void) {
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    assert(gm_commit_page(&space, (uint64_t)STACK / space.page_size * space.page_size, NULL));
    MEM = space.base;
    M8(HEAP_OWNER + 0x221) = 1;

    expect_unchanged(0, ALLOCATION_SIZE, false, true);
    enabled = true;
    expect_unchanged(0, ALLOCATION_SIZE, true, true);
    loading = true;
    chargen = true;
    expect_unchanged(0, ALLOCATION_SIZE, true, true);
    chargen = false;
    original_allows_fallback = true;
    expect_unchanged(1, ALLOCATION_SIZE, false, false);
    expect_unchanged(0, 0, false, false);
    expect_unchanged(0, 0x10000000u, true, false);

    checks = reclaims = plain = aligned = 0;
    original_allows_fallback = false;
    CPU direct = run(staged_allocator, 0, ALLOCATION_SIZE, false);
    assert(!reclaims && plain == 1 && !aligned && checks == 1);
    assert(direct.r[3] == PLAIN_RESULT && direct.r[1] == direct.r[5] && M32(STACK + 0x1C) == 0);
    CPU direct_aligned = run(staged_allocator, 0, ALLOCATION_SIZE, true);
    assert(!reclaims && plain == 1 && aligned == 1 && checks == 2);
    assert(direct_aligned.r[3] == ALIGNED_RESULT && M32(STACK + 0x1C) == 0);

    for (unsigned uses_alignment = 0; uses_alignment < 2; ++uses_alignment) {
        checks = reclaims = plain = aligned = 0;
        refuse_next_allocation = true;
        original_allows_fallback = true;
        const CPU retried = run(staged_allocator, 0, ALLOCATION_SIZE, uses_alignment);
        assert(checks == 1 && reclaims == 1 && !refuse_next_allocation);
        assert(plain == 2 * !uses_alignment && aligned == 2 * uses_alignment);
        assert(retried.r[3] == (uses_alignment ? ALIGNED_RESULT : PLAIN_RESULT));
        assert(M32(STACK + 0x1C) == ORIGINAL_RESULT);
    }

    loading = false;
    expect_unchanged(0, ALLOCATION_SIZE, false, true);
    assert(gm_release(&space, NULL));
    return 0;
}
