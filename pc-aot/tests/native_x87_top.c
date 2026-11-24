/* Game-free comparison of emitted C with real native x86_64 x87 execution (patch 0009).
 * tests/test_x87_top.py assembles each test program twice from the same text: 32-bit code that tools/lift.py
 * translates, and 64-bit code run natively through a small wrapper (nat_prog_N). Inputs are doubles in a small
 * buffer (guest address 0x4000 for the translated code, a real buffer for the native code); results are the buffer,
 * eax, the status flags and the x87 stack position afterwards. Not a game run.
 *
 * Vector line: <id> <program> <eax hex> <ecx hex> <4 input doubles as hex bit patterns>.
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
    unsigned id, program, eax, ecx;
    unsigned long long bits[4];
    int scanned;
    while ((scanned = scanf("%u %u %x %x %llx %llx %llx %llx", &id, &program, &eax, &ecx, &bits[0], &bits[1], &bits[2],
                            &bits[3])) == 8) {
        if (program >= sizeof(programs) / sizeof(programs[0])) return 2;
        static uint64_t native_buffer[16] __attribute__((aligned(64)));
        memset(native_buffer, 0, sizeof(native_buffer));
        memcpy(native_buffer, bits, sizeof(bits));
        uint64_t out[2] = {0, 0};
        uint64_t native_eax = programs[program].nat(native_buffer, eax, ecx, out);
        CPU cpu;
        memset(&cpu, 0, sizeof(cpu));
        memset(memory, 0, sizeof(memory));
        memcpy(memory + 0x4000, bits, sizeof(bits));
        cpu.r[0] = eax; cpu.r[1] = ecx; cpu.r[2] = 0x4000; cpu.r[4] = 0x8000;
        cpu.fpu_cw = 0x37F;
        programs[program].lifted(&cpu);
        uint64_t lifted_buffer[16];
        memcpy(lifted_buffer, memory + 0x4000, sizeof(lifted_buffer));
        printf("{\"id\":%u,\"native_eax\":%u,\"lifted_eax\":%u,\"native_flags\":%u,\"lifted_flags\":%u,"
               "\"native_top\":%u,\"lifted_top\":%u,\"lifted_esp\":%u,\"native_buffer\":[",
               id, (uint32_t)native_eax, cpu.r[0], (uint32_t)(out[0] & 0x8d5u), pack(&cpu.flags),
               (uint32_t)((out[1] >> 11) & 7), cpu.top & 7, cpu.r[4]);
        for (int i = 0; i < 16; i++) printf("%s%llu", i ? "," : "", (unsigned long long)native_buffer[i]);
        printf("],\"lifted_buffer\":[");
        for (int i = 0; i < 16; i++) printf("%s%llu", i ? "," : "", (unsigned long long)lifted_buffer[i]);
        printf("]}\n");
    }
    return scanned == EOF ? 0 : 2;
}
