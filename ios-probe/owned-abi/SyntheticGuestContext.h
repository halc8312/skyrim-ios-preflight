// Hand-written public-layout POD and synthetic entry tags; no title input.
#ifndef SYNTHETIC_GUEST_CONTEXT_H
#define SYNTHETIC_GUEST_CONTEXT_H
#include <stdint.h>
typedef struct __attribute__((aligned(64))) PGSyntheticContext {
    uint64_t gpr[32]; // Public storage order r3,r0,r1,r2,r4..r31.
    uint64_t lr, ctr;
    uint8_t xer[3], padding[5];
    uint64_t reserved;
    uint32_t msr;
    uint8_t cr[8][4];
    uint32_t fpscr;
    uint64_t fpr[32];
    uint8_t vr[128][16] __attribute__((aligned(16)));
} PGSyntheticContext;
#define PG_SYNTHETIC_NORMAL 0x1000u
#define PG_SYNTHETIC_JUMP 0x1004u
#define PG_SYNTHETIC_UNWIND 0x1008u
#endif
