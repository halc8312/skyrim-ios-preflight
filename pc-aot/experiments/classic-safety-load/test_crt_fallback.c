/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "rt.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdlib.h>

enum {
    STACK = 0x12F00, HEAP_ALLOC_IAT = 0x106B1CC, CRT_HEAP = 0x1BBEF40, CRT_MODE = 0x1BC0458,
    CRT_NEW_HANDLER = 0x1BBF1C8, TEST_ERRNO = 0x12000, TEST_HEAP_HANDLE = 117,
    HEAP_ALLOC_THUNK = 0x10000, N = 1238, TEST_ALIGNMENT = 32
};

uint8_t *MEM;
int g_log_level;
static unsigned heap_calls;
static uint32_t allocated;
static bool fail_heap_alloc;

void f_F568FA(CPU *c);
void f_F587F5(CPU *c);
void f_F53EBD(CPU *c);
void f_F5D518(CPU *c);
void f_F5D36D(CPU *c);
void f_F58933(CPU *c);
void f_F568AB(CPU *c);
void f_F6917A(CPU *c);
void f_F549DC(CPU *c);

void rt_call_ic(CPU *c, uint32_t target, const FnEntry **cache) {
    (void)cache;
    uint32_t sp = c->r[4];
    assert(target == HEAP_ALLOC_THUNK && M32(sp) == 0xF56973);
    assert(M32(sp + 4) == TEST_HEAP_HANDLE && M32(sp + 8) == 0 && M32(sp + 12) >= N);
    c->r[0] = allocated = fail_heap_alloc ? 0 : gmalloc(M32(sp + 12));
    c->r[4] += 16;
    ++heap_calls;
}

void f_F53EBD(CPU *c) {
    c->r[0] = TEST_ERRNO;
    c->r[4] += 4;
}

/* The selected ordinary initialized CRT heap uses no new-handler or error
 * callbacks. Failure sets the thread errno through the leaf above. */
void f_F5D518(CPU *c) { (void)c; abort(); }
void f_F5D36D(CPU *c) { (void)c; abort(); }
void f_F58933(CPU *c) { (void)c; abort(); }
void f_F568AB(CPU *c) { (void)c; abort(); }
void f_F6917A(CPU *c) { (void)c; abort(); }
void f_F549DC(CPU *c) { (void)c; abort(); }

void rt_host_discard(void *unused, size_t bytes) { (void)unused; (void)bytes; abort(); }

static void commit(gm_space *memory, uint32_t address) {
    assert(gm_commit_page(memory, (uint64_t)address / memory->page_size * memory->page_size, NULL));
}

static uint32_t run(bool aligned, bool fail) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    M32(STACK) = 0x1111;
    M32(STACK + 4) = N;
    M32(STACK + 8) = TEST_ALIGNMENT;
    M32(TEST_ERRNO) = 0;
    heap_calls = 0;
    fail_heap_alloc = fail;
    (aligned ? f_F587F5 : f_F568FA)(&cpu);
    assert(cpu.r[4] == STACK + 4 && heap_calls == 1);
    assert((M32(TEST_ERRNO) == 12) == fail);
    return cpu.r[0];
}

int main(void) {
    gm_space guest = {0};
    assert(gm_reserve(&guest, NULL));
    MEM = guest.base;
    commit(&guest, STACK);
    commit(&guest, HEAP_ALLOC_IAT);
    commit(&guest, CRT_HEAP);
    commit(&guest, CRT_MODE);
    commit(&guest, CRT_NEW_HANDLER);
    commit(&guest, GUEST_HEAP_BASE);
    M32(HEAP_ALLOC_IAT) = HEAP_ALLOC_THUNK;
    M32(CRT_HEAP) = TEST_HEAP_HANDLE;
    M32(CRT_MODE) = 1;
    M32(CRT_NEW_HANDLER) = 0;
    uint32_t plain = run(false, false);
    assert(plain && plain == allocated && gsize(plain) >= N);
    gfree(plain);
    uint32_t aligned = run(true, false);
    assert(aligned && aligned % TEST_ALIGNMENT == 0 && M32(aligned - 4) == allocated);
    assert((uint64_t)aligned + N <= (uint64_t)allocated + gsize(allocated));
    gfree(allocated);
    assert(!run(false, true));
    assert(!run(true, true));
    assert(gm_release(&guest, NULL));
}
