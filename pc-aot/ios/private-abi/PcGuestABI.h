#pragma once
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include "x86.h"
#include "rt_table.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PC_GUEST_ABI 1u
#define PC_GUEST_LAYOUT_WORDS 21u
typedef struct PcGuestHostAPI {
    uint32_t version, bytes;
    uint32_t cpu_layout[PC_GUEST_LAYOUT_WORDS];
    void (*call)(CPU *, uint32_t);
    void (*call_ic)(CPU *, uint32_t, const FnEntry **);
    void (*unimpl)(CPU *, uint32_t, const char *);
    void (*int3)(CPU *, uint32_t);
    void (*cpuid)(CPU *, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
    uint64_t (*rdtsc)(void);
    double (*load80)(uint32_t);
    void (*store80)(uint32_t, double);
    void (*examine)(CPU *);
    void (*remainder)(CPU *, int);
    void (*environment)(CPU *, uint32_t, int);
    jmp_buf *(*jump_slot)(CPU *, uint32_t);
    void (*jump_save)(CPU *);
} PcGuestHostAPI;

typedef struct PcGuestAPI {
    uint32_t version, bytes;
    uint32_t cpu_layout[PC_GUEST_LAYOUT_WORDS];
    const FnEntry *functions;
    uint32_t function_count;
    uint32_t reserved;
    int (*bind_memory)(const PcGuestHostAPI *, uint8_t *);
} PcGuestAPI;

#define PC_GUEST_LAYOUT_INITIALIZER { \
    sizeof(CPU), _Alignof(CPU), offsetof(CPU,r), offsetof(CPU,fs_base), \
    offsetof(CPU,df), offsetof(CPU,eflags_hi), offsetof(CPU,st), offsetof(CPU,top), \
    offsetof(CPU,c0), offsetof(CPU,c1), offsetof(CPU,c2), offsetof(CPU,c3), \
    offsetof(CPU,fpu_cw), offsetof(CPU,fpu_tag), offsetof(CPU,xmm), offsetof(CPU,mxcsr), \
    offsetof(CPU,mm), offsetof(CPU,thread), offsetof(CPU,flags), sizeof(Flags), sizeof(jmp_buf) }

typedef const PcGuestAPI *(*PcGuestGetAPI)(void);
int pc_guest_open(const char *path);
int pc_guest_bind_memory(uint8_t *memory);
#ifdef __cplusplus
}
#endif
