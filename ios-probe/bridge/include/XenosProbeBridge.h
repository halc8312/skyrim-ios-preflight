#ifndef XENOS_PROBE_BRIDGE_H
#define XENOS_PROBE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t probe_abi_version(void); /* 1 */
uint32_t probe_width(void);       /* 4 texels */
uint32_t probe_height(void);      /* 4 texels */
size_t probe_rgba_byte_len(void); /* 64 bytes */

/*
 * Decode a hand-authored BC1 fixture to 4 x 4 row-major RGBA8 texels.
 * Each row is red, blue, (170,0,85,255), (85,0,170,255).
 * No game data is used. This is a texture/FFI probe, not a game runtime.
 *
 * Returns: 64 success; -1 null output; -2 capacity < 64; -3 decoder panic.
 * Failure writes nothing. Success writes exactly 64 bytes and retains no pointer.
 * If non-null with capacity >= 64, out must refer to 64 writable bytes that
 * no other thread accesses during this call. Capacity is measured in bytes.
 */
int32_t probe_decode_rgba(uint8_t *out, size_t capacity);

/* Runtime ABI 1: 64-bit Apple targets only. No game inputs or game boot. */
#define PROBE_RUNTIME_ABI_VERSION 1u
#define PROBE_CONTEXT_LAYOUT_WORDS 209u
#define PROBE_MEMORY_EXPECTED_FLAGS UINT64_C(0x3ff)
#define PROBE_CPU_EXPECTED_FLAGS UINT64_C(0x3f0000)

typedef struct ProbeRuntimeResult {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t passed;
    uint64_t failed;
    uint32_t callback_result;
    uint32_t reserved;
} ProbeRuntimeResult;

/* C++ implementations use the corresponding C ABI; Rust invokes call using
 * extern "C-unwind". These synthetic callbacks must not throw or longjmp,
 * change threads, recurse into a runtime probe, or retain context/base. */
typedef void (*ProbeGuestCall)(uint32_t address, void *context, uint8_t *base);
typedef void (*ProbeGuestInit)(void *context);
typedef struct ProbeGuestDescriptor {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t context_size;
    uint32_t context_alignment;
    uint32_t image_base;
    uint32_t image_size;
    uint32_t code_base;
    uint32_t code_size;
    const uint32_t *functions;
    size_t function_count;
    ProbeGuestCall call;
    ProbeGuestInit init_context;
} ProbeGuestDescriptor;

/* Returns 209, -1 null, -2 short word capacity, -3 panic, -4 unaligned.
 * Failure writes nothing. Aligned out must have 209 writable uint32_t values.
 * Words: 0 ABI; 1 sizeof(PPCContext); 2 alignment; 3 LR; 4 CTR; 5 XER;
 * 6 reserved; 7 MSR; 8 FPSCR; 9..40 architectural GPR0..31 offsets;
 * 41..48 CR0..7 offsets; 49..80 FPR0..31 offsets; 81..208 VR0..127 offsets. */
int32_t probe_runtime_context_layout(uint32_t *out, size_t word_capacity);

/* Result capacity is BYTES. Output must be aligned/writable for 32 bytes.
 * Returns 0 all checks passed; -1 null; -2 short; -3 panic/poison;
 * -4 unaligned; -5 invalid descriptor; -6 memory OS failure;
 * -7 a completed check failed (result IS written). Other errors write nothing.
 * Probes serialize their own global memory/dirty tables. Never overlap them
 * with an actual running guest. Darwin zeroing preserves 4KB neighbours;
 * physical-page reclamation is not tested or implemented by that backend.
 * Memory bits 0..9: reserve, commit-zero, big-endian, decommit-zero,
 * 4KB neighbours in 16KB, recommit-zero, physical views, dirty marks,
 * release-zero, rejected invalid ranges. */
int32_t probe_runtime_memory_selftest(ProbeRuntimeResult *out, size_t byte_capacity);

/* Synchronous synthetic descriptor adapter, NOT general game registration.
 * Descriptor/storage must be aligned, readable, stable through return and
 * callbacks must be genuine callable code pointers. Validation cannot prove
 * arbitrary non-null addresses safe. functions must be exactly the sorted
 * [0x82001000,0x82001004]; image/code must contain them without overflow.
 * Rust uses its own static table, never retaining the caller's table/pointers.
 * CPU bits 16..21: descriptor, C++ init, function lookup, register return,
 * BE memory return, exactly one same-thread C++ -> Rust callback.
 * Call receives address 0x82001000. Initial r3=0x1122334455667788,
 * r4=guest address 0x40014000, r5=0xa1b2c3d4, r6=0x55667788;
 * BE memory[r4]=0x10203040. Callback xor's r3 with 0xffeeddccbbaa0099,
 * copies original BE memory[r4] into r7, then writes callback_mix(r5,r6)
 * through the real translated guest memory path. init_context constructs a
 * zero PPCContext (MSR=0x0200a000), capturing host FP control state. */
int32_t probe_runtime_cpu_roundtrip(const ProbeGuestDescriptor *descriptor,
                                  ProbeRuntimeResult *out, size_t byte_capacity);
uint32_t probe_runtime_callback_mix(uint32_t left, uint32_t right);

/* Five narrow public-source checks: kernel export table; VFS wildcard;
 * XEX bad-magic rejection; D3D NOP header; XMA sample conversion. Returns
 * bitmask 31 on success, 0 on panic. Not Kernel boot/GPU/audio execution. */
uint32_t probe_runtime_component_checks(void);

/* Dedicated Rust-panic -> C++ RAII destructor -> Rust catch probe, isolated
 * from the ordinary CPU callback above. The call starts in Rust and must not
 * insert a Swift/noexcept/non-unwind C frame. counter is aligned, exclusively
 * readable/writable, initially zero. CPP callback constructs a guard whose
 * destructor increments it, then calls raise_synthetic_exit. No C++ throw,
 * catching/replacing Rust panic, retained pointer, cross-thread or longjmp.
 * Returns 0 only for the dedicated Rust payload AND counter==1 after cleanup;
 * -1 null counter, -4 unaligned, -5 missing callback/nonzero initial counter,
 * -8 normal return, unrelated Rust panic or incorrect cleanup count.
 * This is not proof of guest EH or exceptions through arbitrary game frames. */
typedef void (*ProbeCppUnwindCall)(uint32_t *counter);
int32_t probe_runtime_catch_cpp_unwind(ProbeCppUnwindCall call, uint32_t *counter);
#ifdef __cplusplus
[[noreturn]]
#endif
void probe_runtime_raise_synthetic_exit(void);

#ifdef __cplusplus
}
#endif

#endif
