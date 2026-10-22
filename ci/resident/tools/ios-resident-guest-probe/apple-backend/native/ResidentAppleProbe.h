#ifndef RESIDENT_APPLE_PROBE_H
#define RESIDENT_APPLE_PROBE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RAAppleResultV1 {
    uint32_t version, bytes, passed, failure;
    uint64_t checked_flags, frequency, cleanup_count, dispatched_imports;
    uint64_t exit_code, function_count;
} RAAppleResultV1;
/* These are outcome flags, set together ONLY after all checks have passed.
 * They are never permissions, cryptographic attestation or game compatibility. */
enum RAAppleCheckedFlagV1 {
    RA_CHECK_C_LAYOUT49=1u<<0, RA_CHECK_LOADED_HEADER_RX=1u<<1,
    RA_CHECK_OWNED_API7=1u<<2, RA_CHECK_PUBLIC_DESCRIPTOR3=1u<<3,
    RA_CHECK_RUNTIME_LOAD=1u<<4, RA_CHECK_STRICT_KERNEL=1u<<5,
    RA_CHECK_LIVE_MEMORY_BIND=1u<<6, RA_CHECK_NATIVE_JUMP_RETURN=1u<<7,
    RA_CHECK_NORMAL_RETURN=1u<<8, RA_CHECK_THREAD_EXIT_ZERO=1u<<9,
    RA_CHECK_EXACT_IMPORT2=1u<<10, RA_CHECK_CPP_CLEANUP1=1u<<11,
    RA_CHECK_DIRTY_AND_BUSY=1u<<12, RA_CHECK_QUIESCENT_REPUBLISH=1u<<13,
    RA_CHECK_ALL=0x3fffu
};
/* Synchronous, one process/one probe only. trusted_sha256 is exactly 64 ASCII
 * lowercase hex bytes, obtained separately AFTER framework signing. Buffers
 * must be valid/exclusive/disjoint. Return 0 success; any negative is failure.
 * No Swift frame is present on the C-unwind guest thread's C++/Rust path.
 * A failed/partial run cannot be retried or reset in the same process. */
int32_t RAAppleRunPublicProbeV1(const uint8_t *trusted_sha256,size_t hash_bytes,
    RAAppleResultV1 *out,size_t out_bytes);
#ifdef __cplusplus
}
static_assert(sizeof(RAAppleResultV1)==64 && alignof(RAAppleResultV1)==8);
#endif
#endif
