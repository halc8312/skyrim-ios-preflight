/* Isolated mandatory-XMA-MMIO revision. V1 is included unchanged, not extended.
 * Only requirements/bind symbols change. Memory/init/call retain their V1 ABI.
 * The two NEW XMA callbacks are synchronous, process-retained, never Swift and never unwind.
 * Address is the original aligned PPC32 EA; word is device-logical LE numeric.
 * 0 means ordinary-memory fallback; 1 means handled; ALL other values are fatal.
 * Read output is used only on1. No polling or reserved-field interpretation.
 */
#ifndef PRIVATE_GUEST_ABI_V2_H
#define PRIVATE_GUEST_ABI_V2_H
#include "PrivateGuestABI.h"
#define PG_ABI_VERSION_2 0x00020000u
#define PG_V2_REQUIRED_CONTRACTS (PG_REQUIRED_CONTRACTS | 0x20u)
#define PG_MMIO_UNHANDLED_V2 0
#define PG_MMIO_HANDLED_V2 1
#define PG_FATAL_MMIO_CALLBACK_V2 5u
#ifdef __cplusplus
extern "C" {
#endif
typedef PGRequirementsV1 PGRequirementsV2; /* Same fields; version MUST be2. */
typedef int32_t (*PGXmaReadU32V2)(void *user, uint32_t address, uint32_t *out_device_word);
typedef int32_t (*PGXmaWriteU32V2)(void *user, uint32_t address, uint32_t device_word);
typedef struct PGHostCallbacksV2 {
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
    uintptr_t stack_guard_seed;
    PGXmaReadU32V2 xma_read_u32;
    PGXmaWriteU32V2 xma_write_u32;
    uint64_t reserved[4];
} PGHostCallbacksV2;
PG_EXPORT int32_t PG_GetRequirementsV2(PGRequirementsV2 *, uint32_t);
PG_EXPORT int32_t PG_BindHostV2(const PGHostCallbacksV2 *, uint32_t);
#ifdef __cplusplus
}
static_assert(sizeof(void *) != 8 || sizeof(PGHostCallbacksV1) == 400);
static_assert(sizeof(void *) != 8 || sizeof(PGHostCallbacksV2) == 416);
static_assert(sizeof(void *) != 8 || offsetof(PGHostCallbacksV2, xma_read_u32) == 368);
static_assert(sizeof(void *) != 8 || offsetof(PGHostCallbacksV2, xma_write_u32) == 376);
static_assert(sizeof(void *) != 8 || offsetof(PGHostCallbacksV2, reserved) == 384);
#endif
#endif
