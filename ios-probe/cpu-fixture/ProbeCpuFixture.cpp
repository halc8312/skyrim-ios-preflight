// Self-authored CPU fixture using the pinned PUBLIC context and real Rust memory.
// It is never generated from game instructions or a game configuration.
#include "ProbeCpuFixture.h"
#include "ppc_config.h"
#include <ppc_context.h>
#include <cstddef>
#include <cstring>
#include <new>

static_assert(sizeof(PPCContext) == 2688 && alignof(PPCContext) == 64);
static_assert(sizeof(ProbeGuestDescriptor) == 64);
static_assert(sizeof(ProbeRuntimeResult) == 32);

int32_t probe_cpp_context_layout(uint32_t *out, size_t capacity) {
    if (!out) return -1;
    if (capacity < 209) return -2;
    uint32_t words[209]{};
    words[0] = 1;
    words[1] = sizeof(PPCContext);
    words[2] = alignof(PPCContext);
    words[3] = offsetof(PPCContext, lr);
    words[4] = offsetof(PPCContext, ctr);
    words[5] = offsetof(PPCContext, xer);
    words[6] = offsetof(PPCContext, reserved);
    words[7] = offsetof(PPCContext, msr);
    words[8] = offsetof(PPCContext, fpscr);
#define GPR(N) static_cast<uint32_t>(offsetof(PPCContext, r##N))
    constexpr uint32_t gpr[] = {
        GPR(0), GPR(1), GPR(2), GPR(3), GPR(4), GPR(5), GPR(6), GPR(7),
        GPR(8), GPR(9), GPR(10), GPR(11), GPR(12), GPR(13), GPR(14), GPR(15),
        GPR(16), GPR(17), GPR(18), GPR(19), GPR(20), GPR(21), GPR(22), GPR(23),
        GPR(24), GPR(25), GPR(26), GPR(27), GPR(28), GPR(29), GPR(30), GPR(31)};
#undef GPR
#define CR(N) static_cast<uint32_t>(offsetof(PPCContext, cr##N))
    constexpr uint32_t cr[] = {CR(0), CR(1), CR(2), CR(3), CR(4), CR(5), CR(6), CR(7)};
#undef CR
#define FPR(N) static_cast<uint32_t>(offsetof(PPCContext, f##N))
    constexpr uint32_t fpr[] = {
        FPR(0), FPR(1), FPR(2), FPR(3), FPR(4), FPR(5), FPR(6), FPR(7),
        FPR(8), FPR(9), FPR(10), FPR(11), FPR(12), FPR(13), FPR(14), FPR(15),
        FPR(16), FPR(17), FPR(18), FPR(19), FPR(20), FPR(21), FPR(22), FPR(23),
        FPR(24), FPR(25), FPR(26), FPR(27), FPR(28), FPR(29), FPR(30), FPR(31)};
#undef FPR
#define VR(N) static_cast<uint32_t>(offsetof(PPCContext, v##N))
    constexpr uint32_t vr[] = {
        VR(0), VR(1), VR(2), VR(3), VR(4), VR(5), VR(6), VR(7),
        VR(8), VR(9), VR(10), VR(11), VR(12), VR(13), VR(14), VR(15),
        VR(16), VR(17), VR(18), VR(19), VR(20), VR(21), VR(22), VR(23),
        VR(24), VR(25), VR(26), VR(27), VR(28), VR(29), VR(30), VR(31),
        VR(32), VR(33), VR(34), VR(35), VR(36), VR(37), VR(38), VR(39),
        VR(40), VR(41), VR(42), VR(43), VR(44), VR(45), VR(46), VR(47),
        VR(48), VR(49), VR(50), VR(51), VR(52), VR(53), VR(54), VR(55),
        VR(56), VR(57), VR(58), VR(59), VR(60), VR(61), VR(62), VR(63),
        VR(64), VR(65), VR(66), VR(67), VR(68), VR(69), VR(70), VR(71),
        VR(72), VR(73), VR(74), VR(75), VR(76), VR(77), VR(78), VR(79),
        VR(80), VR(81), VR(82), VR(83), VR(84), VR(85), VR(86), VR(87),
        VR(88), VR(89), VR(90), VR(91), VR(92), VR(93), VR(94), VR(95),
        VR(96), VR(97), VR(98), VR(99), VR(100), VR(101), VR(102), VR(103),
        VR(104), VR(105), VR(106), VR(107), VR(108), VR(109), VR(110), VR(111),
        VR(112), VR(113), VR(114), VR(115), VR(116), VR(117), VR(118), VR(119),
        VR(120), VR(121), VR(122), VR(123), VR(124), VR(125), VR(126), VR(127)};
#undef VR
    static_assert(sizeof(gpr) / sizeof(*gpr) == 32 && sizeof(vr) / sizeof(*vr) == 128);
    std::memcpy(words + 9, gpr, sizeof(gpr));
    std::memcpy(words + 41, cr, sizeof(cr));
    std::memcpy(words + 49, fpr, sizeof(fpr));
    std::memcpy(words + 81, vr, sizeof(vr));
    std::memcpy(out, words, sizeof(words));
    return 209;
}

static void init_context(void *storage) {
    auto *ctx = new (storage) PPCContext{};
    ctx->fpscr.loadFromHost();
}

static void call_fixture(uint32_t address, void *storage, uint8_t *base) {
    auto &ctx = *static_cast<PPCContext *>(storage);
    PPC_FUNC_PROLOGUE();
    if (address == 0x82001000) {
        ctx.r3.u64 ^= UINT64_C(0xFFEEDDCCBBAA0099);
        ctx.r7.u64 = PPC_LOAD_U32(ctx.r4.u32);
        const uint32_t value = probe_runtime_callback_mix(ctx.r5.u32, ctx.r6.u32);
        PPC_STORE_U32(ctx.r4.u32, value);
    } else if (address == 0x82001004) {
        // Independent second public entry, no game body or extracted instruction.
        ctx.r3.u64 += 1;
    }
}

const ProbeGuestDescriptor *probe_cpp_guest_descriptor(void) {
    static const uint32_t functions[] = {0x82001000, 0x82001004};
    static const ProbeGuestDescriptor descriptor = {
        1, sizeof(ProbeGuestDescriptor), sizeof(PPCContext), alignof(PPCContext),
        0x82000000, 0x2000, 0x82001000, 8,
        functions, 2, &call_fixture, &init_context};
    return &descriptor;
}

uint32_t probe_cpp_fpcr_roundtrip(void) {
    PPCFPSCRRegister control{};
    const uint32_t original = control.getcsr();
    control.loadFromHost();
    uint32_t passed = 0;
    for (uint32_t mode = 0; mode < 4; ++mode) {
        control.storeFromGuest(mode);
        const uint32_t actual = control.getcsr();
        if (control.loadFromHost() == mode &&
            (actual & control.RoundMask) == control.GuestToHost[mode])
            passed |= 1u << mode;
    }
    control.enableFlushModeUnconditional();
    if ((control.getcsr() & control.FlushMask) == control.FlushMask) passed |= 1u << 4;
    control.disableFlushModeUnconditional();
    if ((control.getcsr() & control.FlushMask) == 0) passed |= 1u << 5;
    control.setcsr(original);
    if (control.getcsr() == original) passed |= 1u << 6;
    return passed;
}

void probe_cpp_unwind_cleanup(uint32_t *counter) {
    struct Cleanup {
        uint32_t *counter;
        ~Cleanup() { ++*counter; }
    } cleanup{counter};
    // The real panic/unwind starts in Rust, crosses this C++ cleanup frame,
    // and is caught by the dedicated Rust caller. No Swift frame is crossed.
    probe_runtime_raise_synthetic_exit();
}

int32_t probe_cpp_unwind_selftest(uint32_t *counter) {
    return probe_runtime_catch_cpp_unwind(&probe_cpp_unwind_cleanup, counter);
}
