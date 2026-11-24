/* SPDX-License-Identifier: GPL-3.0-only */
/* Authored stacks and images only. The scenario lines printed to stdout are fed to
 * the real analyzer by tests/test_cpu_sample.py. */
#include "cpu_sample_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t stack_memory[64] __attribute__((aligned(16)));
#define LO ((uint64_t)(uintptr_t)&stack_memory[0])
#define HI ((uint64_t)(uintptr_t)&stack_memory[64])
#define AT(i) ((uint64_t)(uintptr_t)&stack_memory[i])

static void set_images(void) {
    CpuSampleImage list[3];
    memset(list, 0, sizeof list);
    list[0] = (CpuSampleImage){0x200000000ull, 0x1000000, {0x4c,0x4c,0x44,0x68,0x55,0x55,0x31,0x44,0xa1,0x0b,0x79,0x81,0x85,0xd8,0x69,0x8a}};
    list[1] = (CpuSampleImage){0x300000000ull, 0x100000, {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}};
    list[2] = (CpuSampleImage){0x400000000ull, 0x200000000ull, {0}};   /* too large for an offset */
    assert(cpu_sample_set_images(list, 3) == 2);
}

static void resolve_boundaries(void) {
    unsigned index;
    uint64_t offset;
    assert(cpu_sample_resolve(0x200000000ull, &index, &offset) && index == 0 && offset == 0);
    assert(cpu_sample_resolve(0x200000000ull + 0xFFFFFF, &index, &offset) && index == 0 && offset == 0xFFFFFF);
    assert(!cpu_sample_resolve(0x200000000ull + 0x1000000, &index, &offset));   /* one past the end */
    assert(!cpu_sample_resolve(0x1FFFFFFFFull, &index, &offset));
    assert(cpu_sample_resolve(0x300000010ull, &index, &offset) && index == 1 && offset == 0x10);
    assert(!cpu_sample_resolve(0x400000010ull, &index, &offset));               /* dropped image */
    assert(!cpu_sample_resolve(0, &index, &offset));
}

static void unwind_checks(void) {
    uint64_t out[CPU_SAMPLE_MAX_DEPTH];
    memset(stack_memory, 0, sizeof stack_memory);
    /* A healthy chain of three records; the last has no caller. The first return
     * address carries pointer-authentication bits that must be removed. */
    stack_memory[0] = AT(4);  stack_memory[1] = 0x5A00000200004100ull;
    stack_memory[4] = AT(8);  stack_memory[5] = 0x200005000ull;
    stack_memory[8] = 0;      stack_memory[9] = 0x300000100ull;
    assert(cpu_sample_unwind(0x200004010ull, 0xdeadbeef, AT(0), LO, HI, out) == 4);
    assert(out[0] == 0x200004010ull && out[1] == 0x200004100ull &&
           out[2] == 0x200005000ull && out[3] == 0x300000100ull);

    assert(cpu_sample_unwind(0, 0, AT(0), LO, HI, out) == 0);                 /* no pc */
    assert(cpu_sample_unwind(0x200004010ull, 0, 0, LO, HI, out) == 1);        /* no frame pointer */
    assert(cpu_sample_unwind(0x200004010ull, 0, AT(1), LO, HI, out) == 1);    /* misaligned */
    assert(cpu_sample_unwind(0x200004010ull, 0, LO - 16, LO, HI, out) == 1);  /* below the stack */
    assert(cpu_sample_unwind(0x200004010ull, 0, HI - 8, LO, HI, out) == 1);   /* record straddles the top */
    assert(cpu_sample_unwind(0x200004010ull, 0, AT(0), LO, LO, out) == 1);    /* empty stack range */

    /* Next frame pointer outside the stack: the return address is kept, the walk ends. */
    stack_memory[4] = HI + 4096;
    assert(cpu_sample_unwind(0x200004010ull, 0, AT(0), LO, HI, out) == 3);
    /* A loop (the next record is the same or lower) cannot repeat forever. */
    stack_memory[4] = AT(0);
    assert(cpu_sample_unwind(0x200004010ull, 0, AT(0), LO, HI, out) == 3);
    stack_memory[0] = AT(0);
    assert(cpu_sample_unwind(0x200004010ull, 0, AT(0), LO, HI, out) == 2);
    /* A zero return address ends the walk without adding a frame. */
    stack_memory[0] = AT(4); stack_memory[1] = 0; stack_memory[4] = AT(8);
    assert(cpu_sample_unwind(0x200004010ull, 0, AT(0), LO, HI, out) == 1);

    /* Depth is capped at eight even when the chain is longer. */
    memset(stack_memory, 0, sizeof stack_memory);
    for (unsigned i = 0; i < 30; ++i) {
        stack_memory[i * 2] = AT((i + 1) * 2);
        stack_memory[i * 2 + 1] = 0x200001000ull + i;
    }
    assert(cpu_sample_unwind(0x200004010ull, 0, AT(0), LO, HI, out) == CPU_SAMPLE_MAX_DEPTH);
    assert(out[7] == 0x200001006ull);
}

static size_t drain(char *line, size_t capacity) {
    return cpu_sample_format_window(line, capacity, 1000, 5000, 500);
}

static void aggregation_and_reporting(void) {
    char line[8192];
    drain(line, sizeof line);
    assert(drain(line, sizeof line) == 0);            /* an empty window reports nothing */

    for (unsigned i = 0; i < 50; ++i) cpu_sample_note_tick();
    cpu_sample_note_suspend_error();
    cpu_sample_note_overrun();
    cpu_sample_note_overrun();
    const uint64_t hot[] = {0x200004010ull, 0x200004100ull, 0x300000100ull};
    const uint64_t other_thread_unresolved[] = {0x1c8000000ull, 0x200005008ull};
    for (unsigned i = 0; i < 30; ++i) cpu_sample_add(1, 1, hot, 3);
    for (unsigned i = 0; i < 10; ++i) cpu_sample_add(2, 0, other_thread_unresolved, 2);
    cpu_sample_add(2, 0, hot, 3);                       /* same frames, other thread: own entry */
    size_t length = cpu_sample_format_window(line, sizeof line, 5000000000ull, 5000000000ull, 500);
    assert(length && line[length - 1] == '\n');
    assert(strstr(line, "ticks=50 samples=41 unresolved_leaf=10 suspend_errors=1 overruns=2 dropped=0 stacks_total=3 omitted_stacks=0 omitted_samples=0"));
    /* Most frequent first, numeric frames only. */
    assert(strstr(line, "stacks=30:1:1:0@4010,0@4100,1@100;10:2:0:255@0,0@5008;1:2:0:0@4010,0@4100,1@100\n"));
    fputs(line, stdout);                                /* scenario for the analyzer test */

    /* The report cleared the window: nothing leaks into the next one. */
    assert(drain(line, sizeof line) == 0);
    cpu_sample_note_tick();
    cpu_sample_add(1, 1, hot, 3);
    assert(cpu_sample_format_window(line, sizeof line, 1, 1, 500));
    assert(strstr(line, "samples=1 ") && strstr(line, "stacks_total=1 ") && strstr(line, "stacks=1:1:1:"));
}

static void table_full_and_long_report_reconcile(void) {
    static char line[8192];
    for (unsigned i = 0; i < 20; ++i) cpu_sample_note_tick();   /* up to 64 samples per tick */
    unsigned added = 0;
    for (unsigned i = 0; i < CPU_SAMPLE_STACK_CAPACITY + 40; ++i) {
        uint64_t frames[CPU_SAMPLE_MAX_DEPTH];
        for (unsigned f = 0; f < CPU_SAMPLE_MAX_DEPTH; ++f) frames[f] = 0x200000000ull + 0x1000 * (f + 1) + i * 16;
        cpu_sample_add(i % 7, i % 2, frames, CPU_SAMPLE_MAX_DEPTH);
        ++added;
    }
    size_t length = cpu_sample_format_window(line, sizeof line, 9, 9, 500);
    assert(length && length < 8191);
    assert(strstr(line, "dropped=40 stacks_total=1024 "));
    assert(!strstr(line, "omitted_stacks=0 "));        /* it cannot all fit: the rest is accounted for */
    fputs(line, stdout);
    (void)added;
}

int main(void) {
    set_images();
    resolve_boundaries();
    unwind_checks();
    aggregation_and_reporting();
    table_full_and_long_report_reconcile();
    char image[128];
    assert(cpu_sample_format_image(image, sizeof image, 0));
    assert(!strcmp(image, "IOS_CPU_IMAGE schema=1 index=0 uuid=4c4c446855553144a10b798185d8698a text_size=16777216\n"));
    assert(!cpu_sample_format_image(image, sizeof image, 2));
    fputs(image, stdout);
    assert(cpu_sample_format_image(image, sizeof image, 1));
    fputs(image, stdout);
    fputs("cpu_sample_core=PASS\n", stdout);
    return 0;
}
