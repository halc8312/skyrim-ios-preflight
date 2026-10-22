// Real SDK API pair, self-authored state, one live C++ caller, same thread.
// This exercises no game code and never jumps through a Rust or Swift frame.
#include "ProbeCpuFixture.h"
#include "ppc_config.h"
#include <ppc_context.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

#if !defined(__aarch64__) || defined(__arm64e__)
#error "This execution fixture supports plain arm64 only"
#endif

using Capture = int (*)(jmp_buf);
using Restore = void (*)(jmp_buf, int);
struct ProbeCpuJumpCallbacks {
    Capture capture;
    Restore restore;
};
static_assert(sizeof(ProbeCpuJumpCallbacks) == 16);
static_assert(offsetof(ProbeCpuJumpCallbacks, restore) == 8);
// These facts are checked against the actual Apple SDK used by CI.
static_assert(sizeof(jmp_buf) == 192 && alignof(jmp_buf) == 4);
static_assert(sizeof(ProbeJumpReport) == 56);
extern "C" {
ProbeCpuJumpCallbacks probe_cpu_jump_callbacks{&::setjmp, &::longjmp};
int probe_cpu_setjmp_tail(jmp_buf buffer) __attribute__((returns_twice));
void probe_cpu_longjmp_tail(jmp_buf buffer, int value) __attribute__((noreturn));
}

namespace {
constexpr uint64_t GUARD = UINT64_C(0xA5C35A3CA5C35A3C);
struct alignas(64) SavedState {
    uint64_t before[8];
    alignas(16) jmp_buf buffer;
    PPCContext snapshot;
    uint64_t after[8];
};
struct alignas(64) LiveState {
    uint64_t before[8];
    PPCContext context;
    uint64_t after[8];
};
struct alignas(64) JumpArena {
    SavedState first;
    SavedState second;
    LiveState live;
    ProbeJumpReport report;
    uint32_t original_fpcr;
};
static_assert(alignof(SavedState) >= 16);

void mark(JumpArena *arena, bool success, unsigned bit) {
    (success ? arena->report.passed : arena->report.failed) |= UINT64_C(1) << bit;
}

void initialize_guards(JumpArena *arena) {
    for (size_t i = 0; i < 8; ++i) {
        arena->first.before[i] = arena->first.after[i] = GUARD;
        arena->second.before[i] = arena->second.after[i] = GUARD;
        arena->live.before[i] = arena->live.after[i] = GUARD;
    }
}

bool guards_intact(const JumpArena *arena) {
    for (size_t i = 0; i < 8; ++i)
        if (arena->first.before[i] != GUARD || arena->first.after[i] != GUARD ||
            arena->second.before[i] != GUARD || arena->second.after[i] != GUARD ||
            arena->live.before[i] != GUARD || arena->live.after[i] != GUARD) return false;
    return true;
}
}

__attribute__((noinline))
int32_t probe_cpp_jump_selftest(ProbeJumpReport *out, size_t capacity) {
    if (!out) return -1;
    if (capacity < sizeof(ProbeJumpReport)) return -2;
    void *allocation = nullptr;
    if (::posix_memalign(&allocation, alignof(JumpArena), sizeof(JumpArena)) != 0) return -3;
    std::memset(allocation, 0, sizeof(JumpArena));
    auto *const arena = new (allocation) JumpArena{};
    // All mutable test state lives in this allocation. The unchanged automatic
    // pointers are valid across longjmp; no nontrivial automatic destructor is
    // crossed. Buffer/snapshot/live context are disjoint subobjects by layout.
    initialize_guards(arena);
    arena->report.abi_version = 1;
    arena->report.struct_size = sizeof(ProbeJumpReport);
    arena->report.buffer_bytes = sizeof(jmp_buf);
    arena->report.buffer_alignment = alignof(jmp_buf);
    arena->report.state_alignment = alignof(SavedState);
    arena->live.context.fpscr.loadFromHost();
    arena->original_fpcr = arena->live.context.fpscr.getcsr();
    arena->live.context.r3.u64 = UINT64_C(0x1122334455667788);
    arena->live.context.f1.u64 = UINT64_C(0x3FF4000000000000);
    arena->live.context.v127.u8[3] = 0x66;
    arena->first.snapshot = arena->live.context;

    ++arena->report.capture_calls;
    const int first = probe_cpu_setjmp_tail(arena->first.buffer);
    if (first == 0) {
        ++arena->report.zero_returns;
        arena->live.context.r3.u64 ^= UINT64_C(0xFFEEDDCCBBAA0099);
        arena->live.context.f1.u64 = 0;
        arena->live.context.v127.u8[3] = 0x99;
        arena->live.context.fpscr.storeFromGuest(PPC_ROUND_UP);
        probe_cpu_longjmp_tail(arena->first.buffer, 7);
    }
    ++arena->report.resumed_returns;
    arena->report.first_return = first;
    mark(arena, first == 7, 0);
    // Restoration belongs to the surviving caller. Do not assume the SDK's
    // longjmp restores hardware FPCR: restore it explicitly from our snapshot.
    arena->live.context = arena->first.snapshot;
    arena->live.context.fpscr.setcsr(arena->live.context.fpscr.csr);
    mark(arena, std::memcmp(&arena->live.context, &arena->first.snapshot, sizeof(PPCContext)) == 0 &&
                arena->live.context.r3.u64 == UINT64_C(0x1122334455667788) &&
                arena->live.context.f1.u64 == UINT64_C(0x3FF4000000000000) &&
                arena->live.context.v127.u8[3] == 0x66, 1);

    // Capture a second independent buffer while this same caller frame lives.
    // Neither capture may be used after this function returns or across threads.
    arena->live.context.r3.u64 ^= UINT64_C(0x0101010101010101);
    arena->live.context.ctr.u64 = 23;
    arena->second.snapshot = arena->live.context;
    ++arena->report.capture_calls;
    const int second = probe_cpu_setjmp_tail(arena->second.buffer);
    if (second == 0) {
        ++arena->report.zero_returns;
        arena->live.context.r3.u64 = 0;
        arena->live.context.ctr.u64 = 0;
        arena->live.context.fpscr.storeFromGuest(PPC_ROUND_DOWN);
        probe_cpu_longjmp_tail(arena->second.buffer, 11);
    }
    ++arena->report.resumed_returns;
    arena->report.second_return = second;
    mark(arena, second == 11, 2);
    arena->live.context = arena->second.snapshot;
    arena->live.context.fpscr.setcsr(arena->live.context.fpscr.csr);
    mark(arena, std::memcmp(&arena->live.context, &arena->second.snapshot, sizeof(PPCContext)) == 0 &&
                arena->live.context.ctr.u64 == 23, 3);
    mark(arena, guards_intact(arena), 4);
    mark(arena, static_cast<void *>(arena->first.buffer) != static_cast<void *>(arena->second.buffer) &&
                arena->first.snapshot.r3.u64 != arena->second.snapshot.r3.u64, 5);
    mark(arena, arena->report.capture_calls == 2 && arena->report.zero_returns == 2 &&
                arena->report.resumed_returns == 2, 6);
    arena->live.context.fpscr.setcsr(arena->original_fpcr);
    mark(arena, arena->live.context.fpscr.getcsr() == arena->original_fpcr, 7);
    const ProbeJumpReport report = arena->report;
    arena->~JumpArena();
    std::free(allocation);
    std::memcpy(out, &report, sizeof(report));
    return 0;
}
