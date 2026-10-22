// Self-authored provider for the real generic adapter. No actual guest body,
// private layout, title address, game instruction, or generated export list.
#include "PrivateGuestABI.h"
#include "SyntheticGuestContext.h"
#include <setjmp.h>
#include <stddef.h>

static_assert(sizeof(PGSyntheticContext) == 2688 && alignof(PGSyntheticContext) == 64);
extern "C" {
extern const int32_t *skyrt_ea_offset;
extern uint8_t *skyrt_written;
uint64_t skyrt_guest_timebase(void);
void skyrt_kernel_call(uint32_t, uint32_t, void *, uint8_t *);
void *memcpy(void *, const void *, size_t);
void bzero(void *, size_t);
float exp2f(float);
float log2f(float);
// Freestanding/-fno-builtin must not erase these callsite semantics. Do not
// rely on compiler recognition of the standard name or SDK header attributes.
int setjmp(jmp_buf) __attribute__((returns_twice));
void longjmp(jmp_buf, int) __attribute__((noreturn));

#define GO(N) (offsetof(PGSyntheticContext, gpr) + (N) * sizeof(uint64_t))
extern const PGRequirementsV1 pg_private_requirements = {
    PG_ABI_VERSION_1, sizeof(PGRequirementsV1), 64, 1, 1, 0,
    {GO(0), GO(1), GO(2), GO(3), GO(4), GO(13), GO(31),
     offsetof(PGSyntheticContext, lr), offsetof(PGSyntheticContext, ctr),
     offsetof(PGSyntheticContext, xer), offsetof(PGSyntheticContext, reserved),
     offsetof(PGSyntheticContext, msr), offsetof(PGSyntheticContext, cr),
     offsetof(PGSyntheticContext, fpscr), offsetof(PGSyntheticContext, fpr),
     offsetof(PGSyntheticContext, fpr) + 8, offsetof(PGSyntheticContext, vr),
     offsetof(PGSyntheticContext, vr) + 127 * 16,
     sizeof(PGSyntheticContext), alignof(PGSyntheticContext)},
    sizeof(jmp_buf), alignof(jmp_buf), PG_EA_OFFSET_COUNT, PG_WRITTEN_COUNT};
#undef GO

void pg_private_init_context(void *pointer) {
    auto *bytes = static_cast<volatile uint8_t *>(pointer);
    for (size_t i = 0; i < sizeof(PGSyntheticContext); ++i) bytes[i] = 0;
    static_cast<PGSyntheticContext *>(pointer)->msr = 0x0200A000;
}
int pg_private_address_valid(uint32_t address) {
    return address == PG_SYNTHETIC_NORMAL || address == PG_SYNTHETIC_JUMP || address == PG_SYNTHETIC_UNWIND;
}
}

namespace {
constexpr uint64_t GUARD = UINT64_C(0xA55A3CC3A55A3CC3);
struct JumpState {
    uint64_t before[8];
    alignas(16) jmp_buf buffer;
    uint64_t after[8];
    PGSyntheticContext snapshot;
    uint32_t zero_returns, resumed_returns;
};
static_assert(offsetof(JumpState, buffer) == 8 * sizeof(uint64_t));
static_assert(offsetof(JumpState, after) == offsetof(JumpState, buffer) + sizeof(jmp_buf));
// This synthetic provider is deliberately single-threaded/non-reentrant.
// Mutable capture state has static storage, never automatic changed locals.
// A capture is used only by its still-live provider frame in the same PG call.
JumpState jump_state;
bool same_bytes(const void *a, const void *b, size_t size) {
    auto *left = static_cast<const volatile uint8_t *>(a);
    auto *right = static_cast<const volatile uint8_t *>(b);
    for (size_t i = 0; i < size; ++i) if (left[i] != right[i]) return false;
    return true;
}
}

extern "C" void pg_private_call_guest(uint32_t address, void *pointer, uint8_t *base) {
    auto *const context = static_cast<PGSyntheticContext *>(pointer);
    if (address == PG_SYNTHETIC_NORMAL) {
        const int32_t offset = __atomic_load_n(&skyrt_ea_offset, __ATOMIC_ACQUIRE)[3];
        context->gpr[0] = static_cast<uint32_t>(offset);
        __atomic_load_n(&skyrt_written, __ATOMIC_ACQUIRE)[static_cast<uint32_t>(offset)] = 1;
        base[0] = static_cast<uint8_t>(offset);
        context->gpr[4] = skyrt_guest_timebase();
        volatile float power_input = 3.0f, log_input = 8.0f;
        context->gpr[5] = exp2f(power_input) == 8.0f && log2f(log_input) == 3.0f;
        const uint64_t payload = UINT64_C(0x1122334455667788);
        memcpy(base + 64, &payload, sizeof(payload));
        bzero(base + 80, sizeof(payload));
        skyrt_kernel_call(1, 1, context, base);
    } else if (address == PG_SYNTHETIC_JUMP) {
        for (size_t i = 0; i < 8; ++i) jump_state.before[i] = jump_state.after[i] = GUARD;
        jump_state.zero_returns = jump_state.resumed_returns = 0;
        memcpy(&jump_state.snapshot, context, sizeof(*context));
        const int returned = setjmp(jump_state.buffer); // Real private tail trampoline.
        if (returned == 0) {
            ++jump_state.zero_returns;
            context->gpr[0] ^= UINT64_C(0xFEDCBA9876543210);
            longjmp(jump_state.buffer, 19);
        }
        ++jump_state.resumed_returns;
        memcpy(context, &jump_state.snapshot, sizeof(*context));
        bool guards = true;
        for (size_t i = 0; i < 8; ++i)
            guards = guards && jump_state.before[i] == GUARD && jump_state.after[i] == GUARD;
        const bool restored = same_bytes(context, &jump_state.snapshot, sizeof(*context));
        context->gpr[3] = returned;
        context->gpr[4] = jump_state.zero_returns;
        context->gpr[5] = jump_state.resumed_returns;
        context->gpr[6] = guards && restored;
    } else if (address == PG_SYNTHETIC_UNWIND) {
        struct Cleanup {
            uint32_t *counter;
            ~Cleanup() { ++*counter; }
        } cleanup{reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(context->gpr[0]))};
        // Host kernel callback raises Rust's own dedicated payload. Its Rust
        // caller catches it after this cleanup and PG_CallGuest's active--.
        skyrt_kernel_call(1, 91, context, base);
    }
}
