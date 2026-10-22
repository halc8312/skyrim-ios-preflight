// App-side SDK provider. The generic adapter lives in a separate API-only
// framework, so these native symbols cannot bind to its private import shims.
#include "OwnedAbiFixture.h"
#include "ProbeCpuFixture.h"
#include <SyntheticGuest/PrivateGuestABI.h>
#include <SyntheticGuest/SyntheticGuestContext.h>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <dlfcn.h>
#include <strings.h>

namespace {
struct alignas(64) HostState {
    int32_t ea_a[PG_EA_OFFSET_COUNT], ea_b[PG_EA_OFFSET_COUNT];
    uint8_t written_a[PG_WRITTEN_COUNT], written_b[PG_WRITTEN_COUNT];
    alignas(64) uint8_t guest_a[4096], guest_b[4096];
    PGSyntheticContext context;
    PGMemoryV1 memory_a, memory_b;
    uint32_t fatal_count, normal_imports;
    int32_t busy_status, unexpected_unwind_return;
};
HostState state;
bool attempted;
constexpr uint64_t TIMEBASE = UINT64_C(0x123456789);

void mark(OwnedAbiReport &report, bool condition, unsigned bit) {
    (condition ? report.passed : report.failed) |= UINT64_C(1) << bit;
}
uint64_t native_timebase(void *) { return TIMEBASE; }
void native_fatal(void *, uint32_t, uint64_t) { ++state.fatal_count; }
int32_t native_kernel(void *, uint32_t module, uint32_t ordinal, void *pointer, uint8_t *) {
    if (module == 1 && ordinal == 91) {
        // Dedicated C-unwind path. The enclosing Rust caller catches its own
        // payload after provider cleanup and the real adapter's active--.
        probe_runtime_raise_synthetic_exit();
        return 0;
    }
    if (module != 1 || ordinal != 1) return 0;
    ++state.normal_imports;
    state.busy_status = PG_PublishMemoryV1(&state.memory_b, sizeof(state.memory_b));
    static_cast<PGSyntheticContext *>(pointer)->gpr[6] = 0x55;
    return PG_KERNEL_HANDLED;
}
void adapter_unwind_call(uint32_t *counter) {
    state.context.gpr[0] = reinterpret_cast<uintptr_t>(counter);
    state.unexpected_unwind_return = PG_CallGuestV1(PG_SYNTHETIC_UNWIND, &state.context, sizeof(state.context));
}
bool native_provider_is_separate(const PGHostCallbacksV1 &host) {
    Dl_info adapter{}, provider{};
    if (!dladdr(reinterpret_cast<void *>(&PG_GetRequirementsV1), &adapter) || !adapter.dli_fbase) return false;
    const void *callbacks[] = {
        reinterpret_cast<void *>(host.copy_memory), reinterpret_cast<void *>(host.zero_memory),
        reinterpret_cast<void *>(host.exp2_float), reinterpret_cast<void *>(host.log2_float),
        reinterpret_cast<void *>(host.native_setjmp), reinterpret_cast<void *>(host.native_longjmp),
        reinterpret_cast<void *>(host.native_personality), reinterpret_cast<void *>(host.native_unwind_resume)};
    for (const void *callback : callbacks)
        if (!callback || !dladdr(callback, &provider) || !provider.dli_fbase || provider.dli_fbase == adapter.dli_fbase)
            return false;
    return true;
}
bool normal_result(int32_t expected, const uint8_t *guest) {
    uint64_t payload = 0, zero = UINT64_MAX;
    std::memcpy(&payload, guest + 64, sizeof(payload));
    std::memcpy(&zero, guest + 80, sizeof(zero));
    return state.context.gpr[0] == static_cast<uint32_t>(expected) && guest[0] == expected &&
        state.context.gpr[4] == TIMEBASE && state.context.gpr[5] == 1 && state.context.gpr[6] == 0x55 &&
        payload == UINT64_C(0x1122334455667788) && zero == 0;
}
int finish(OwnedAbiReport *out, OwnedAbiReport &report, int status) {
    report.fatal_callbacks = state.fatal_count;
    std::memcpy(out, &report, sizeof(report));
    return status;
}
}

int32_t probe_owned_abi_selftest(OwnedAbiReport *out, size_t capacity) {
    if (!out) return -1;
    if (capacity < sizeof(OwnedAbiReport)) return -2;
    if (attempted) return -6;
    attempted = true; // This fixture is serial, single-threaded and one-shot.
    OwnedAbiReport report{1, sizeof(OwnedAbiReport), 0, 0, 0, 0, 0, 0};
    state.ea_a[3] = 17;
    state.ea_b[3] = 41;
    state.memory_a = {state.ea_a, PG_EA_OFFSET_COUNT, state.written_a, PG_WRITTEN_COUNT,
                      state.guest_a, sizeof(state.guest_a)};
    state.memory_b = {state.ea_b, PG_EA_OFFSET_COUNT, state.written_b, PG_WRITTEN_COUNT,
                      state.guest_b, sizeof(state.guest_b)};
    PGRequirementsV1 requirements{};
    if (PG_GetRequirementsV1(&requirements, sizeof(requirements)) != PG_OK) {
        mark(report, false, 0); return finish(out, report, -3);
    }
    uint32_t layout[209]{};
    const unsigned indices[] = {12,9,10,11,13,22,40,3,4,5,6,7,41,8,49,50,81,208,1,2};
    bool matches = probe_cpp_context_layout(layout, 209) == 209;
    for (unsigned i = 0; i < 20; ++i) matches = matches && requirements.context_layout[i] == layout[indices[i]];
    matches = matches && requirements.abi_version == PG_ABI_VERSION_1 && requirements.struct_size == sizeof(requirements) &&
        requirements.pointer_bits == 64 && requirements.little_endian == 1 && requirements.plain_arm64 == 1 &&
        requirements.jump_buffer_size == sizeof(jmp_buf) && requirements.jump_buffer_alignment == alignof(jmp_buf) &&
        requirements.ea_offset_count == PG_EA_OFFSET_COUNT && requirements.written_count == PG_WRITTEN_COUNT;
    mark(report, matches, 0);
    if (!matches) return finish(out, report, -3);
    PGRequirementsV1 short_output;
    std::memset(&short_output, 0xA5, sizeof(short_output));
    const PGRequirementsV1 unchanged = short_output;
    mark(report, PG_GetRequirementsV1(nullptr, sizeof(requirements)) == PG_BAD_ARGUMENT &&
        PG_GetRequirementsV1(&short_output, sizeof(short_output) - 1) == PG_BAD_ARGUMENT &&
        std::memcmp(&short_output, &unchanged, sizeof(short_output)) == 0, 1);
    mark(report, PG_InitContextV1(&state.context, sizeof(state.context)) == PG_NOT_BOUND &&
        PG_CallGuestV1(PG_SYNTHETIC_NORMAL, &state.context, sizeof(state.context)) == PG_NOT_BOUND &&
        PG_PublishMemoryV1(&state.memory_a, sizeof(state.memory_a)) == PG_NOT_BOUND, 2);

    PGHostCallbacksV1 host{};
    host.abi_version = PG_ABI_VERSION_1; host.struct_size = sizeof(host); host.contracts = PG_REQUIRED_CONTRACTS;
    host.jump_abi = PG_JUMP_APPLE_ARM64_SETJMP_1;
    std::memcpy(host.context_layout, requirements.context_layout, sizeof(host.context_layout));
    host.jump_buffer_size = requirements.jump_buffer_size; host.jump_buffer_alignment = requirements.jump_buffer_alignment;
    host.jump_pair_cookie = 1; host.unwind_abi = PG_UNWIND_APPLE_ARM64_DWARF_1; host.unwind_pair_cookie = 1;
    host.user = &state; host.memory = state.memory_a;
    host.timebase = &native_timebase; host.kernel_call = &native_kernel; host.fatal = &native_fatal;
    host.copy_memory = &::memcpy; host.zero_memory = &::bzero;
    host.exp2_float = &::exp2f; host.log2_float = &::log2f;
    host.native_setjmp = reinterpret_cast<PGSetjmpV1>(&::setjmp);
    host.native_longjmp = reinterpret_cast<PGLongjmpV1>(&::longjmp);
    // The framework exports PG_* only. RTLD_DEFAULT therefore resolves the
    // SDK's native unwinder, never the framework's private forwarding symbols.
    host.native_personality = reinterpret_cast<PGPersonalityV1>(dlsym(RTLD_DEFAULT, "__gxx_personality_v0"));
    host.native_unwind_resume = reinterpret_cast<PGUnwindResumeV1>(dlsym(RTLD_DEFAULT, "_Unwind_Resume"));
    host.stack_guard_seed = UINT64_C(0x13579BDF2468ACE1);
    const bool separate_provider = native_provider_is_separate(host);
    mark(report, separate_provider, 14);
    if (!separate_provider) return finish(out, report, -3);

    bool rejected = PG_BindHostV1(nullptr, sizeof(host)) == PG_BAD_ARGUMENT &&
        PG_BindHostV1(&host, sizeof(host) - 1) == PG_BAD_ARGUMENT;
    auto reject = [&](PGHostCallbacksV1 invalid, int expected) {
        const bool good = PG_BindHostV1(&invalid, sizeof(invalid)) == expected;
        if (good) ++report.rejected_bindings;
        rejected = rejected && good;
    };
    auto bad = host; bad.abi_version ^= 1; reject(bad, PG_BAD_ABI);
    bad = host; bad.context_layout[0] ^= 8; reject(bad, PG_BAD_LAYOUT);
    bad = host; bad.memory.ea_offset_count -= 1; reject(bad, PG_BAD_MEMORY);
    bad = host; bad.kernel_call = nullptr; reject(bad, PG_BAD_CALLBACK);
    bad = host; bad.jump_pair_cookie = 0; reject(bad, PG_BAD_JUMP_CONTRACT);
    bad = host; bad.unwind_pair_cookie = 0; reject(bad, PG_BAD_JUMP_CONTRACT);
    bad = host; bad.reserved[0] = 1; reject(bad, PG_BAD_ABI);
    mark(report, rejected && report.rejected_bindings == 7, 3);
    const int bound = PG_BindHostV1(&host, sizeof(host));
    mark(report, bound == PG_OK && PG_BindHostV1(&host, sizeof(host)) == PG_ALREADY_BOUND, 4);
    if (bound != PG_OK) return finish(out, report, -3);
    mark(report, PG_InitContextV1(&state.context, sizeof(state.context)) == PG_OK &&
        state.context.msr == 0x0200A000 && state.context.gpr[0] == 0 &&
        PG_InitContextV1(&state.context, sizeof(state.context) - 1) == PG_BAD_ARGUMENT &&
        PG_CallGuestV1(PG_SYNTHETIC_NORMAL + 2, &state.context, sizeof(state.context)) == PG_BAD_ARGUMENT, 5);
    std::memset(state.guest_a + 80, 0xA5, 8);
    mark(report, PG_CallGuestV1(PG_SYNTHETIC_NORMAL, &state.context, sizeof(state.context)) == PG_OK &&
        normal_result(17, state.guest_a) && state.written_a[17] == 1, 6);
    state.ea_a[3] = 29; state.written_a[29] = 0;
    mark(report, PG_CallGuestV1(PG_SYNTHETIC_NORMAL, &state.context, sizeof(state.context)) == PG_OK &&
        normal_result(29, state.guest_a) && state.written_a[29] == 1, 7);
    mark(report, state.busy_status == PG_BUSY && state.guest_b[0] == 0 && state.written_b[41] == 0, 8);
    mark(report, PG_PublishMemoryV1(&state.memory_b, sizeof(state.memory_b)) == PG_OK &&
        PG_CallGuestV1(PG_SYNTHETIC_NORMAL, &state.context, sizeof(state.context)) == PG_OK &&
        normal_result(41, state.guest_b) && state.written_b[41] == 1 && state.guest_a[0] == 29, 9);
    PGMemoryV1 invalid_memory = state.memory_a; invalid_memory.written_count -= 1;
    mark(report, PG_PublishMemoryV1(&invalid_memory, sizeof(invalid_memory)) == PG_BAD_MEMORY &&
        PG_CallGuestV1(PG_SYNTHETIC_NORMAL, &state.context, sizeof(state.context)) == PG_OK &&
        normal_result(41, state.guest_b) && state.guest_a[0] == 29, 10);
    state.context.gpr[0] = UINT64_C(0x1122334455667788);
    mark(report, PG_CallGuestV1(PG_SYNTHETIC_JUMP, &state.context, sizeof(state.context)) == PG_OK &&
        state.context.gpr[0] == UINT64_C(0x1122334455667788) && state.context.gpr[3] == 19 &&
        state.context.gpr[4] == 1 && state.context.gpr[5] == 1 && state.context.gpr[6] == 1, 11);
    uint32_t counter = 0;
    state.unexpected_unwind_return = 777;
    const int unwind = probe_runtime_catch_cpp_unwind(&adapter_unwind_call, &counter);
    report.unwind_cleanups = counter;
    mark(report, unwind == 0 && counter == 1 && state.unexpected_unwind_return == 777, 12);
    mark(report, PG_PublishMemoryV1(&state.memory_a, sizeof(state.memory_a)) == PG_OK, 13);
    mark(report, state.fatal_count == 0, 15);
    return finish(out, report, 0);
}
