#ifndef OWNED_ABI_FIXTURE_H
#define OWNED_ABI_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OwnedAbiReport {
    uint32_t abi_version, struct_size;
    uint64_t passed, failed;
    uint32_t rejected_bindings, fatal_callbacks, unwind_cleanups, reserved;
} OwnedAbiReport;
// Runs once per process. All inputs are hand-written public synthetic fixtures.
// 16 checked bits; expected passed=0xffff and failed=0. No game is loaded.
// Return -1 null/-2 short without writes; -3 provider/setup failure; -6 rerun.
int32_t probe_owned_abi_selftest(OwnedAbiReport *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
