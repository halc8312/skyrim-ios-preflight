/* Game-free native reference + actual emitted-C comparison driver.
 * Build with -mno-red-zone: PUSHFQ/POPFQ use the stack inside inline asm.
 * Only defined arithmetic flag bits are seeded; TF/DF/AC are never enabled.
 * Original host RFLAGS are restored before returning to compiler-generated C.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86.h"
#include "funcs_exe.h"
#include "function_table.inc"
#if !defined(__x86_64__)
#error This reference requires native x86_64 execution.
#endif
static unsigned char memory[4096];
uint8_t *MEM = memory;

#define NATIVE(instruction) do { \
    __asm__ volatile("pushfq\n\tpopq %%r11\n\tpushq %[seed]\n\tpopfq\n\t" \
        instruction "\n\tpushfq\n\tpopq %[flags]\n\tpushq %%r11\n\tpopfq" \
        : "+&a"(value), [flags] "=&r"(flags) \
        : "c"(rhs), [seed] "r"(seed), [leftf] "m"(leftf), [rightf] "m"(rightf), \
          [leftd] "m"(leftd), [rightd] "m"(rightd) \
        : "r11", "xmm0", "xmm1", "st", "st(1)", "cc", "memory"); \
} while (0)

static void reference(unsigned index, uint32_t lhs, uint32_t rhs, uint32_t input,
                      uint32_t *result, uint32_t *result_flags) {
    uint32_t value = lhs;
    float leftf = (float)(int32_t)lhs, rightf = (float)(int32_t)rhs;
    double leftd = (double)(int32_t)lhs, rightd = (double)(int32_t)rhs;
    uint64_t seed = (input & 0x8d5u) | 0x202u, flags = 0;
    switch (index) {
    case 0: NATIVE("addb %%cl, %%al"); break;
    case 1: NATIVE("addw %%cx, %%ax"); break;
    case 2: NATIVE("addl %%ecx, %%eax"); break;
    case 3: NATIVE("subb %%cl, %%al"); break;
    case 4: NATIVE("subw %%cx, %%ax"); break;
    case 5: NATIVE("subl %%ecx, %%eax"); break;
    case 6: NATIVE("adcb %%cl, %%al"); break;
    case 7: NATIVE("adcw %%cx, %%ax"); break;
    case 8: NATIVE("adcl %%ecx, %%eax"); break;
    case 9: NATIVE("sbbb %%cl, %%al"); break;
    case 10: NATIVE("sbbw %%cx, %%ax"); break;
    case 11: NATIVE("sbbl %%ecx, %%eax"); break;
    case 12: NATIVE("incb %%al"); break;
    case 13: NATIVE("incw %%ax"); break;
    case 14: NATIVE("incl %%eax"); break;
    case 15: NATIVE("decb %%al"); break;
    case 16: NATIVE("decw %%ax"); break;
    case 17: NATIVE("decl %%eax"); break;
    case 18: NATIVE("cmpb %%cl, %%al"); break;
    case 19: NATIVE("cmpw %%cx, %%ax"); break;
    case 20: NATIVE("cmpl %%ecx, %%eax"); break;
    case 21: NATIVE("negb %%al"); break;
    case 22: NATIVE("negw %%ax"); break;
    case 23: NATIVE("negl %%eax"); break;
    case 24: NATIVE("nop"); break;
    case 25: NATIVE("lahf"); break;
    case 26: NATIVE("sahf"); break;
    case 27: NATIVE("clc"); break;
    case 28: NATIVE("stc"); break;
    case 29: NATIVE("cmc"); break;
    case 30: NATIVE("movl %%ecx, %%eax"); break;
    case 31: NATIVE("movss %[leftf], %%xmm0; movss %[rightf], %%xmm1; comiss %%xmm1, %%xmm0"); break;
    case 32: NATIVE("movss %[leftf], %%xmm0; movss %[rightf], %%xmm1; ucomiss %%xmm1, %%xmm0"); break;
    case 33: NATIVE("movsd %[leftd], %%xmm0; movsd %[rightd], %%xmm1; comisd %%xmm1, %%xmm0"); break;
    case 34: NATIVE("movsd %[leftd], %%xmm0; movsd %[rightd], %%xmm1; ucomisd %%xmm1, %%xmm0"); break;
    /* Finite operands only. Each reference leaves the native x87 stack balanced. */
    case 35: NATIVE("fldl %[rightd]; fldl %[leftd]; fcomi %%st(1), %%st; fstp %%st(0); fstp %%st(0)"); break;
    case 36: NATIVE("fldl %[rightd]; fldl %[leftd]; fucomi %%st(1), %%st; fstp %%st(0); fstp %%st(0)"); break;
    case 37: NATIVE("fldl %[rightd]; fldl %[leftd]; fcomip %%st(1), %%st; fstp %%st(0)"); break;
    case 38: NATIVE("fldl %[rightd]; fldl %[leftd]; fucomip %%st(1), %%st; fstp %%st(0)"); break;
    default: fprintf(stderr, "invalid instruction index\n"); exit(2);
    }
    *result = value; *result_flags = (uint32_t)flags;
}

int main(void) {
    /* Check reference plumbing independently before trusting comparisons. */
    uint32_t r, f;
    reference(0, 0xA55A5A0Fu, 1, 0, &r, &f);
    if (r != 0xA55A5A10u || (f & 0x8d5u) != 0x10u) return 3;
    reference(12, 0xff, 0, 1, &r, &f);
    if (r != 0 || (f & 0x8d5u) != 0x55u) return 3;
    reference(8, UINT32_MAX, 0, 1, &r, &f);
    if (r != 0 || (f & 0x8d5u) != 0x55u) return 3;
    fprintf(stderr, "Native reference sanity checks passed; arithmetic flags restored after each operation.\n");
    unsigned id, instruction, lhs, rhs, seed;
    int scanned;
    while ((scanned = scanf("%u %u %x %x %x", &id, &instruction, &lhs, &rhs, &seed)) == 5) {
        if (instruction >= sizeof(lifted_functions) / sizeof(lifted_functions[0])) return 2;
        reference(instruction, lhs, rhs, seed, &r, &f);
        CPU cpu; memset(&cpu, 0, sizeof(cpu)); memset(memory, 0, sizeof(memory));
        cpu.r[0] = lhs; cpu.r[1] = rhs; cpu.r[4] = 0x100;
        if (instruction == 31 || instruction == 32) {
            cpu.xmm[0].f[0] = (float)(int32_t)lhs; cpu.xmm[1].f[0] = (float)(int32_t)rhs;
        } else {
            cpu.xmm[0].d[0] = (double)(int32_t)lhs; cpu.xmm[1].d[0] = (double)(int32_t)rhs;
        }
        cpu.st[0] = (double)(int32_t)lhs; cpu.st[1] = (double)(int32_t)rhs;
        uint32_t seeded = (seed & 0x8d5u) | 0x202u;
        memcpy(memory + 0x100, &seeded, sizeof(seeded));
        /* Actual emitted function: popfd; op; pushfd; pop edx; ret. */
        lifted_functions[instruction](&cpu);
        printf("{\"id\":%u,\"native_eax\":%u,\"lifted_eax\":%u,\"native_flags\":%u,\"lifted_flags\":%u,\"lifted_esp\":%u,\"lifted_ecx\":%u}\n",
               id, r, cpu.r[0], f, cpu.r[2], cpu.r[4], cpu.r[1]);
    }
    return scanned == EOF ? 0 : 2;
}
