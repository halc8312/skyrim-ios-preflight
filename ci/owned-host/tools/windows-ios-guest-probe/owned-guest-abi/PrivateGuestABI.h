/* Generic owned ABI. Contains no title addresses, hashes, or game data.
 * Plain little-endian ARM64 only; not arm64e. All callbacks are synchronous
 * and remain valid for the process. Only kernel_call and PG_CallGuestV1 may
 * propagate Rust C-unwind through C++ frames; no Swift frame may intervene.
 */
#ifndef PRIVATE_GUEST_ABI_H
#define PRIVATE_GUEST_ABI_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32) && defined(PG_SYNTHETIC_HOST_TEST)
#define PG_EXPORT __declspec(dllexport)
#else
#define PG_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define PG_ABI_VERSION_1 0x00010000u
#define PG_CONTEXT_LAYOUT_COUNT 20u
#define PG_EA_OFFSET_COUNT 4096u
#define PG_WRITTEN_COUNT 1048576u
#define PG_JUMP_APPLE_ARM64_SETJMP_1 0x414A0001u
#define PG_KERNEL_HANDLED 1
#define PG_UNWIND_APPLE_ARM64_DWARF_1 0x41550001u
#define PG_REQUIRED_CONTRACTS 0x0000001Fu
/* Required host declarations: only kernel/guest-call path may C-unwind; all listed
 * imports are handled or kernel_call returns != PG_KERNEL_HANDLED; jump pair
 * uses one native Apple ARM64 setjmp/longjmp provider; table buffers stay alive.
 * Flags acknowledge contracts; registration cannot prove callback semantics.
 */
enum PGStatusV1 {
    PG_OK = 0, PG_BAD_ARGUMENT = -1, PG_BAD_ABI = -2, PG_BAD_LAYOUT = -3,
    PG_BAD_MEMORY = -4, PG_BAD_CALLBACK = -5, PG_ALREADY_BOUND = -6,
    PG_NOT_BOUND = -7, PG_BUSY = -8, PG_BAD_JUMP_CONTRACT = -9
};
enum PGFatalV1 { PG_FATAL_UNBOUND = 1, PG_FATAL_UNHANDLED_IMPORT = 2,
    PG_FATAL_MISSING_FUNCTION = 3, PG_FATAL_STACK_CHECK = 4 };

/* Order: r3,r0,r1,r2,r4,r13,r31,lr,ctr,xer,reserved,msr,cr0,fpscr,
 * f0,f1,v0,v127,sizeof(PPCContext),alignof(PPCContext). */
typedef struct PGRequirementsV1 {
    uint32_t abi_version, struct_size;
    uint32_t pointer_bits, little_endian, plain_arm64, reserved;
    uint64_t context_layout[PG_CONTEXT_LAYOUT_COUNT];
    uint64_t jump_buffer_size, jump_buffer_alignment;
    uint64_t ea_offset_count, written_count;
} PGRequirementsV1;

typedef struct PGMemoryV1 {
    const int32_t *ea_offsets;
    uint64_t ea_offset_count;
    uint8_t *written;
    uint64_t written_count;
    uint8_t *guest_base;
    uint64_t guest_bytes;
} PGMemoryV1;
/* Trusted storage contract: every declared table element must be readable and
 * every written byte writable; every effective frame/range the guest can access
 * after folding must lie within guest_base[0..guest_bytes). The adapter checks
 * pointer alignment/counts/arithmetic, not OS allocation commitment or the
 * semantic validity of each mapping. In-place mapping changes must preserve
 * this contract and runtime synchronization, including physical-view edges. */

typedef uint64_t (*PGTimebaseV1)(void *user);
typedef int32_t (*PGKernelCallV1)(void *user, uint32_t module, uint32_t ordinal,
                               void *context, uint8_t *guest_base);
typedef void (*PGFatalCallbackV1)(void *user, uint32_t reason, uint64_t detail);
typedef void *(*PGMemcpyV1)(void *, const void *, size_t);
typedef void (*PGBzeroV1)(void *, size_t);
typedef float (*PGMathFloatV1)(float);
/* These point DIRECTLY to a matched native provider, never ordinary C wrappers.
 * Private ARM64 trampolines tail-BR without changing SP/LR or argument registers.
 * Call sites retain returns_twice / noreturn declarations. Jump buffers may
 * return only to a STILL-LIVE capturing frame on the SAME thread/provider and
 * within the SAME PG_CallGuestV1 invocation. Never reuse after that call returns
 * or jump across a nested PG_CallGuestV1 boundary. These lifetime constraints
 * remain the host's contract. Longjmp cannot replace Rust C-unwind cleanup. */
typedef int (*PGSetjmpV1)(void *buffer);
typedef void (*PGLongjmpV1)(void *buffer, int value);
typedef int32_t (*PGPersonalityV1)(int32_t version, int32_t actions,
    uint64_t exception_class, void *exception, void *unwind_context);
typedef void (*PGUnwindResumeV1)(void *exception);

typedef struct PGHostCallbacksV1 {
    uint32_t abi_version, struct_size, contracts, jump_abi;
    uint64_t context_layout[PG_CONTEXT_LAYOUT_COUNT];
    uint64_t jump_buffer_size, jump_buffer_alignment, jump_pair_cookie;
    uint32_t unwind_abi, unwind_reserved;
    uint64_t unwind_pair_cookie;
    void *user;
    PGMemoryV1 memory;
    PGTimebaseV1 timebase;
    PGKernelCallV1 kernel_call;
    PGFatalCallbackV1 fatal;
    PGMemcpyV1 copy_memory;
    PGBzeroV1 zero_memory;
    PGMathFloatV1 exp2_float, log2_float;
    PGSetjmpV1 native_setjmp;
    PGLongjmpV1 native_longjmp;
    PGPersonalityV1 native_personality;
    PGUnwindResumeV1 native_unwind_resume;
    uintptr_t stack_guard_seed; /* Nonzero and immutable after bind; adapter is compiled without stack protection. */
    uint64_t reserved[4];
} PGHostCallbacksV1;

/* Query is valid before binding. Binding is one-shot and exact-size/version.
 * No import, game function or constructor is called before validation succeeds.
 * The adapter copies callback POINTERS, never ea_offsets/written table contents.
 */
PG_EXPORT int32_t PG_GetRequirementsV1(PGRequirementsV1 *out, uint32_t out_bytes);
PG_EXPORT int32_t PG_BindHostV1(const PGHostCallbacksV1 *host, uint32_t host_bytes);
/* Re-publish live table pointers only while no guest call/context init is active.
 * In-place host updates must obey the runtime's thread/renderer synchronization.
 * Old buffers may be released only after this call succeeds. No unbind exists.
 */
PG_EXPORT int32_t PG_PublishMemoryV1(const PGMemoryV1 *memory, uint32_t memory_bytes);
PG_EXPORT int32_t PG_InitContextV1(void *context, uint64_t context_bytes);
/* Host must declare this as Rust extern "C-unwind" when Rust panics can cross
 * it. Both personality/resume pointers must come from the matching native
 * Apple C++ unwinder; no emulation/stub provider. Cleanup releases active calls. */
PG_EXPORT int32_t PG_CallGuestV1(uint32_t address, void *context, uint64_t context_bytes);

#ifdef __cplusplus
}
#endif
#endif
