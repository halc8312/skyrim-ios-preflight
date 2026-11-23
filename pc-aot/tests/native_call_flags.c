/* Game-free comparison of emitted C with real native x86_64 execution, for arithmetic flags carried across
 * calls (patch 0008). tests/test_call_flags.py assembles each test program twice from the same text: as 32-bit
 * code that tools/lift.py translates, and as 64-bit code that runs natively here through a small wrapper
 * (nat_prog_N) which loads the seed flags, calls the program and stores the final flags. Not a game run.
 *
 * Vector line: <id> <program> <seed flags hex> <eax hex> <ecx hex>. Output: one JSON object per vector.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86.h"
#include "funcs_exe.h"
#include "rt_table.h"
#include "program_table.inc"
#if !defined(__x86_64__)
#error This reference requires native x86_64 execution.
#endif

static unsigned char memory[1 << 16];
uint8_t *MEM = memory;
extern const FnEntry g_fn_table_exe[];
extern const unsigned g_fn_count_exe;

static const FnEntry *find_entry(uint32_t target) {
    for (unsigned i = 0; i < g_fn_count_exe; i++)
        if (g_fn_table_exe[i].addr == target) return &g_fn_table_exe[i];
    fprintf(stderr, "no lifted function at %x\n", target);
    exit(3);
}
void rt_call(CPU *c, uint32_t target) { find_entry(target)->fn(c); }
void rt_call_ic(CPU *c, uint32_t target, const FnEntry **cache) {
    const FnEntry *entry = find_entry(target);
    __atomic_store_n(cache, entry, __ATOMIC_RELAXED);
    entry->fn(c);
}
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c; fprintf(stderr, "unimplemented %x %s\n", addr, what); exit(4);
}
void rt_int3(CPU *c, uint32_t addr) { (void)c; fprintf(stderr, "int3 %x\n", addr); exit(4); }

static uint32_t pack(const Flags *f) {
    return (f->cf ? 0x1u : 0) | (f->pf ? 0x4u : 0) | (f->af ? 0x10u : 0) | (f->zf ? 0x40u : 0) |
           (f->sf ? 0x80u : 0) | (f->of ? 0x800u : 0);
}

int main(void) {
    unsigned id, program, seed, eax, ecx;
    int scanned;
    while ((scanned = scanf("%u %u %x %x %x", &id, &program, &seed, &eax, &ecx)) == 5) {
        if (program >= sizeof(programs) / sizeof(programs[0])) return 2;
        uint64_t out[2] = {0, 0};
        seed = (seed & 0x8d5u) | 0x202u;
        uint64_t native_eax = programs[program].nat(seed, eax, ecx, out);
        CPU cpu;
        memset(&cpu, 0, sizeof(cpu));
        memset(memory, 0, sizeof(memory));
        cpu.r[0] = eax; cpu.r[1] = ecx; cpu.r[2] = 0; cpu.r[4] = 0x8000;
        cpu.flags.cf = seed & 1; cpu.flags.pf = (seed >> 2) & 1; cpu.flags.af = (seed >> 4) & 1;
        cpu.flags.zf = (seed >> 6) & 1; cpu.flags.sf = (seed >> 7) & 1; cpu.flags.of = (seed >> 11) & 1;
        programs[program].lifted(&cpu);
        printf("{\"id\":%u,\"native_eax\":%u,\"lifted_eax\":%u,\"native_edx\":%u,\"lifted_edx\":%u,"
               "\"native_flags\":%u,\"lifted_flags\":%u,\"lifted_esp\":%u}\n",
               id, (uint32_t)native_eax, cpu.r[0], (uint32_t)out[1], cpu.r[2], (uint32_t)(out[0] & 0x8d5u),
               pack(&cpu.flags), cpu.r[4]);
    }
    return scanned == EOF ? 0 : 2;
}
