/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    STACK = 0x2fc0, POOL = 0x2100, QUEUE = 0x2200,
    POOL_SINGLETON = 0x1b38308, READY_IMPORT = 0x106b160, CLEANUP_IMPORT = 0x106b150,
    READY = 1, CLEANUP, BEFORE_QUEUE, BEGIN_QUEUE, PROCESS_QUEUE
};

uint8_t *MEM;
static pthread_t owner_thread, main_thread;
static const CPU *owner_cpu;
static bool opted_in, queue_ready;
static unsigned notifications, events[5], event_count;

void f_6910F0(CPU *cpu);
void unobserved_task_queue(CPU *cpu);

static void record(unsigned event)
{
    assert(event_count < sizeof events / sizeof events[0]);
    events[event_count++] = event;
}

void f_515E30(CPU *cpu)
{
    assert(cpu->r[4] == STACK - 4 && M32(cpu->r[4]) == 0x6910f5);
    record(BEFORE_QUEUE);
    cpu->r[4] += 4;
}

void f_6A0950(CPU *cpu)
{
    assert(cpu->r[1] == POOL && M32(cpu->r[4]) == 0x691100);
    record(BEGIN_QUEUE);
    cpu->r[4] += 4;
}

void f_6A0B30(CPU *cpu)
{
    assert(cpu->r[1] == POOL && M32(cpu->r[4]) == 0x69110b);
    record(PROCESS_QUEUE);
    M32(QUEUE + 0x14) = 13;
    cpu->r[2] = M32(QUEUE + 0x14);
    cpu->flags.cf = 1;
    cpu->r[4] += 4;
}

void highheels_after_task_queue(const CPU *cpu)
{
    if (!opted_in || cpu != owner_cpu || !pthread_equal(pthread_self(), owner_thread))
        return;
    assert(event_count == 3 && events[2] == PROCESS_QUEUE);
    assert(cpu->r[4] == STACK && cpu->r[2] == 13 && cpu->flags.cf == 1);
    assert(M32(QUEUE + 0x14) == 13);
    ++notifications;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    uint32_t stack = cpu->r[4];
    record(target);
    if (target == READY) {
        assert(M32(stack) == 0x6a0935 && M32(stack + 4) == 13);
        assert(M32(stack + 8) == 1 && M32(stack + 12) == 0);
        cpu->r[0] = queue_ready;
        cpu->r[4] += 16;
    } else {
        assert(target == CLEANUP && M32(stack) == 0x6a0943 && M32(stack + 4) == QUEUE + 0x18);
        cpu->r[4] += 8;
    }
}

typedef struct Result {
    uint32_t registers[8];
    Flags flags;
    uint32_t queue_word;
    unsigned order[5];
    uint8_t stack_bytes[40];
} Result;

static Result run(void (*queue)(CPU *), bool permit, bool same_thread, bool ready, unsigned expected)
{
    CPU cpu = {.r = {1, 2, 3, 4, STACK, 5, 6, 7}};
    memset(MEM + STACK - 36, 0, 40);
    M32(STACK) = 0x100;
    M32(QUEUE + 0x14) = 0;
    memset(events, 0, sizeof events);
    event_count = 0;
    notifications = 0;
    owner_cpu = &cpu;
    owner_thread = same_thread ? pthread_self() : main_thread;
    opted_in = permit;
    queue_ready = ready;
    queue(&cpu);
    owner_cpu = NULL;
    assert(cpu.r[4] == STACK + 4 && cpu.r[6] == 6);
    assert(notifications == expected);
    assert(event_count == (ready ? 5u : 4u));
    assert(events[0] == BEFORE_QUEUE && events[1] == BEGIN_QUEUE && events[2] == PROCESS_QUEUE);
    assert(events[3] == READY && (!ready || events[4] == CLEANUP));

    Result result = {.flags = cpu.flags, .queue_word = M32(QUEUE + 0x14)};
    memcpy(result.registers, cpu.r, sizeof result.registers);
    memcpy(result.order, events, sizeof result.order);
    memcpy(result.stack_bytes, MEM + STACK - 36, sizeof result.stack_bytes);
    return result;
}

static void check_equal(const Result *a, const Result *b)
{
    assert(memcmp(a->registers, b->registers, sizeof a->registers) == 0);
    assert(memcmp(a->order, b->order, sizeof a->order) == 0);
    assert(memcmp(a->stack_bytes, b->stack_bytes, sizeof a->stack_bytes) == 0);
    assert(memcmp(&a->flags, &b->flags, sizeof a->flags) == 0);
    assert(a->queue_word == b->queue_word);
}

static void *check_queue(void *unused)
{
    (void)unused;
    for (unsigned ready = 0; ready <= 1; ++ready) {
        Result original = run(unobserved_task_queue, true, true, ready, 0);
        Result disabled = run(f_6910F0, false, true, ready, 0);
        check_equal(&original, &disabled);
        Result enabled = run(f_6910F0, true, true, ready, 1);
        check_equal(&original, &enabled);
        assert(!pthread_equal(pthread_self(), main_thread));
        Result foreign = run(f_6910F0, true, false, ready, 0);
        check_equal(&original, &foreign);
    }
    return NULL;
}

int main(void)
{
    gm_space memory = {0};
    assert(gm_reserve(&memory, NULL));
    const uint32_t addresses[] = {STACK, POOL, POOL_SINGLETON, READY_IMPORT};
    for (unsigned i = 0; i < sizeof addresses / sizeof addresses[0]; ++i) {
        uint32_t address = addresses[i];
        if (!gm_pointer(&memory, address, 4))
            assert(gm_commit_page(&memory, address / memory.page_size * memory.page_size, NULL));
    }
    MEM = memory.base;
    M32(POOL_SINGLETON) = POOL;
    M32(POOL + 4) = QUEUE;
    M32(READY_IMPORT) = READY;
    M32(CLEANUP_IMPORT) = CLEANUP;
    pthread_t thread;
    main_thread = pthread_self();
    assert(!pthread_create(&thread, NULL, check_queue, NULL));
    assert(!pthread_join(thread, NULL));
    assert(gm_release(&memory, NULL));
    puts("Classic task queue observation preserved the original guest state and owner");
    return 0;
}
