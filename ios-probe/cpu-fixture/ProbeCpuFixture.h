#ifndef PROBE_CPU_FIXTURE_H
#define PROBE_CPU_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
#include "XenosProbeBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

// All 209 fields use the order documented by probe_runtime_context_layout.
// Null/short output returns -1/-2 without writes; success returns 209 words.
int32_t probe_cpp_context_layout(uint32_t *out, size_t word_capacity);
const ProbeGuestDescriptor *probe_cpp_guest_descriptor(void);

// Bits 0..3: hardware rounding mode write/readback; 4/5: flush enable/disable;
// bit 6: original hardware FPCR restored. No floating exception is raised.
uint32_t probe_cpp_fpcr_roundtrip(void);

// Dedicated Rust C-unwind probe only: the runtime validates counter and catches
// its own SyntheticExit payload after this callback's RAII cleanup executes.
// Do not call this directly from Swift or the normal GuestModule callback.
void probe_cpp_unwind_cleanup(uint32_t *counter);
// Callback address is obtained wholly in C++; Swift only sees the normal result
// after the dedicated Rust caller has caught its own synthetic payload.
int32_t probe_cpp_unwind_selftest(uint32_t *counter);

typedef struct ProbeJumpReport {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t passed;
    uint64_t failed;
    uint32_t capture_calls;
    uint32_t zero_returns;
    uint32_t resumed_returns;
    uint32_t first_return;
    uint32_t second_return;
    uint32_t buffer_bytes;
    uint32_t buffer_alignment;
    uint32_t state_alignment;
} ProbeJumpReport;

// Execute only self-authored POD state and SDK setjmp/longjmp, inside one live
// caller frame and one thread. It cannot capture Swift/Rust callers or game code.
// Bits 0..7: first return/snapshot, second return/snapshot, guards, distinct
// buffers/snapshots, exact twice-return counters, original FPCR restoration.
// Output must be aligned, non-aliased writable sizeof(ProbeJumpReport) storage.
// Null/short output returns -1/-2 without writes; allocation failure is -3.
// Returns 0 after execution; report.failed == 0 is required for test success.
// This plain-arm64 probe establishes neither arm64e/PAC nor BTI compatibility.
int32_t probe_cpp_jump_selftest(ProbeJumpReport *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
