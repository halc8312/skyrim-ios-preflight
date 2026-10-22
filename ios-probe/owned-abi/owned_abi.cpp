/* Generic adapter, compiled freestanding with -fno-builtin and
 * -fno-stack-protector. Never include title headers here. */
#include "PrivateGuestABI.h"
#if !defined(PG_SYNTHETIC_HOST_TEST) && (!defined(__aarch64__) || defined(__arm64e__))
#error This ABI requires plain ARM64
#endif
extern "C" {

extern const PGRequirementsV1 pg_private_requirements;
extern void pg_private_init_context(void *);
extern void pg_private_call_guest(uint32_t, void *, uint8_t *);
extern int pg_private_address_valid(uint32_t);

/* 0 unbound, 2 first binding, 1 ready, 3 memory-update admission lock.
 * Existing imports/unwind may continue in 3; new entry requires exactly 1. */
static uint32_t pg_state, pg_active;
static PGHostCallbacksV1 pg_host;
const int32_t *skyrt_ea_offset;
uint8_t *skyrt_written;
void *pg_jump_targets[3];
uintptr_t __stack_chk_guard;

static int ready(void) {
    uint32_t state = __atomic_load_n(&pg_state, __ATOMIC_ACQUIRE);
    return state == 1 || state == 3;
}
static int admission(void) { return __atomic_load_n(&pg_state, __ATOMIC_SEQ_CST) == 1; }
static void copy_volatile(void *destination, const void *source, size_t size) {
    volatile unsigned char *to = (volatile unsigned char *)destination;
    const volatile unsigned char *from = (const volatile unsigned char *)source;
    for (size_t i = 0; i < size; ++i) to[i] = from[i];
}
static int memory_valid(const PGMemoryV1 *memory) {
    return memory && memory->ea_offsets && memory->written && memory->guest_base &&
        ((uintptr_t)memory->ea_offsets & 3u) == 0 && ((uintptr_t)memory->guest_base & 31u) == 0 &&
        memory->ea_offset_count == PG_EA_OFFSET_COUNT && memory->written_count == PG_WRITTEN_COUNT &&
        memory->guest_bytes && memory->guest_bytes <= UINT64_C(0x100000000) &&
        (uintptr_t)memory->guest_base <= UINTPTR_MAX - memory->guest_bytes;
}
static void publish(const PGMemoryV1 *memory) {
    copy_volatile(&pg_host.memory, memory, sizeof(*memory));
    __atomic_store_n(&skyrt_ea_offset, memory->ea_offsets, __ATOMIC_RELEASE);
    __atomic_store_n(&skyrt_written, memory->written, __ATOMIC_RELEASE);
}
static __attribute__((noreturn)) void die(uint32_t reason, uint64_t detail) {
    if (ready() && pg_host.fatal) pg_host.fatal(pg_host.user, reason, detail);
    __builtin_trap();
}
static int enter(void) {
    if (!admission()) return __atomic_load_n(&pg_state, __ATOMIC_ACQUIRE) ? PG_BUSY : PG_NOT_BOUND;
    __atomic_fetch_add(&pg_active, 1u, __ATOMIC_SEQ_CST);
    if (!admission()) {
        __atomic_fetch_sub(&pg_active, 1u, __ATOMIC_RELEASE);
        return PG_BUSY;
    }
    return PG_OK;
}
static int context_valid(void *context, uint64_t bytes) {
    uint64_t alignment = pg_private_requirements.context_layout[19];
    return context && bytes == pg_private_requirements.context_layout[18] &&
        alignment && ((uintptr_t)context % alignment) == 0;
}

int32_t PG_GetRequirementsV1(PGRequirementsV1 *out, uint32_t bytes) {
    if (!out || bytes != sizeof(*out) || (uintptr_t)out % alignof(PGRequirementsV1)) return PG_BAD_ARGUMENT;
    copy_volatile(out, &pg_private_requirements, sizeof(*out));
    return PG_OK;
}
int32_t PG_BindHostV1(const PGHostCallbacksV1 *host, uint32_t bytes) {
    if (__atomic_load_n(&pg_state, __ATOMIC_ACQUIRE)) return PG_ALREADY_BOUND;
    if (!host || bytes != sizeof(*host) || (uintptr_t)host % alignof(PGHostCallbacksV1)) return PG_BAD_ARGUMENT;
    if (host->abi_version != PG_ABI_VERSION_1 || host->struct_size != sizeof(*host) || host->contracts != PG_REQUIRED_CONTRACTS) return PG_BAD_ABI;
    for (size_t i = 0; i < 4; ++i) if (host->reserved[i]) return PG_BAD_ABI;
    for (size_t i = 0; i < PG_CONTEXT_LAYOUT_COUNT; ++i)
        if (host->context_layout[i] != pg_private_requirements.context_layout[i]) return PG_BAD_LAYOUT;
    if (!memory_valid(&host->memory)) return PG_BAD_MEMORY;
    if (!host->timebase || !host->kernel_call || !host->fatal || !host->copy_memory || !host->zero_memory || !host->exp2_float || !host->log2_float || !host->stack_guard_seed) return PG_BAD_CALLBACK;
    if (host->jump_abi != PG_JUMP_APPLE_ARM64_SETJMP_1 || !host->jump_pair_cookie ||
        host->jump_buffer_size != pg_private_requirements.jump_buffer_size || host->jump_buffer_alignment != pg_private_requirements.jump_buffer_alignment ||
        !host->native_setjmp || !host->native_longjmp || (uintptr_t)host->native_setjmp == (uintptr_t)host->native_longjmp) return PG_BAD_JUMP_CONTRACT;
    if (host->unwind_abi != PG_UNWIND_APPLE_ARM64_DWARF_1 || host->unwind_reserved || !host->unwind_pair_cookie || !host->native_personality || !host->native_unwind_resume) return PG_BAD_JUMP_CONTRACT;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&pg_state, &expected, 2u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return PG_ALREADY_BOUND;
    copy_volatile(&pg_host, host, sizeof(*host));
    publish(&host->memory);
    __stack_chk_guard = host->stack_guard_seed;
    __atomic_store_n(&pg_jump_targets[0], (void *)host->native_setjmp, __ATOMIC_RELEASE);
    __atomic_store_n(&pg_jump_targets[1], (void *)host->native_longjmp, __ATOMIC_RELEASE);
    __atomic_store_n(&pg_jump_targets[2], (void *)host->native_unwind_resume, __ATOMIC_RELEASE);
    __atomic_store_n(&pg_state, 1u, __ATOMIC_RELEASE);
    return PG_OK;
}
int32_t PG_PublishMemoryV1(const PGMemoryV1 *memory, uint32_t bytes) {
    if (!memory || bytes != sizeof(*memory) || (uintptr_t)memory % alignof(PGMemoryV1)) return PG_BAD_ARGUMENT;
    if (!memory_valid(memory)) return PG_BAD_MEMORY;
    uint32_t expected = 1;
    if (!__atomic_compare_exchange_n(&pg_state, &expected, 3u, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return expected == 0 ? PG_NOT_BOUND : PG_BUSY;
#if defined(PG_SYNTHETIC_HOST_TEST)
    extern void PG_TestDuringUpdate(void);
    PG_TestDuringUpdate();
#endif
    if (__atomic_load_n(&pg_active, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&pg_state, 1u, __ATOMIC_RELEASE);
        return PG_BUSY;
    }
    publish(memory);
    __atomic_store_n(&pg_state, 1u, __ATOMIC_RELEASE);
    return PG_OK;
}
int32_t PG_InitContextV1(void *context, uint64_t bytes) {
    if (!context_valid(context, bytes)) return PG_BAD_ARGUMENT;
    int status = enter(); if (status != PG_OK) return status;
    pg_private_init_context(context);
    __atomic_fetch_sub(&pg_active, 1u, __ATOMIC_RELEASE);
    return PG_OK;
}
int32_t PG_CallGuestV1(uint32_t address, void *context, uint64_t bytes) {
    if (!context_valid(context, bytes) || !pg_private_address_valid(address)) return PG_BAD_ARGUMENT;
    int status = enter(); if (status != PG_OK) return status;
    struct ActiveCall {
        ~ActiveCall() noexcept { __atomic_fetch_sub(&pg_active, 1u, __ATOMIC_RELEASE); }
    } active_call;
    pg_private_call_guest(address, context, pg_host.memory.guest_base);
    return PG_OK;
}

uint64_t skyrt_guest_timebase(void) {
    if (!ready()) die(PG_FATAL_UNBOUND, 0);
    return pg_host.timebase(pg_host.user);
}
void skyrt_kernel_call(uint32_t module, uint32_t ordinal, void *context, uint8_t *base) {
    if (!ready()) die(PG_FATAL_UNBOUND, 0);
    if (pg_host.kernel_call(pg_host.user, module, ordinal, context, base) != PG_KERNEL_HANDLED)
        die(PG_FATAL_UNHANDLED_IMPORT, ((uint64_t)module << 32) | ordinal);
}
__attribute__((noreturn)) void skyrt_missing_function(uint32_t address) { die(PG_FATAL_MISSING_FUNCTION, address); }
void *memcpy(void *destination, const void *source, size_t size) {
    if (!ready()) die(PG_FATAL_UNBOUND, 0);
    void *result = pg_host.copy_memory(destination, source, size);
    if (result != destination) die(PG_FATAL_UNBOUND, 0);
    return result;
}
void bzero(void *destination, size_t size) {
    if (!ready()) die(PG_FATAL_UNBOUND, 0);
    pg_host.zero_memory(destination, size);
}
float exp2f(float value) { if (!ready()) die(PG_FATAL_UNBOUND, 0); return pg_host.exp2_float(value); }
float log2f(float value) { if (!ready()) die(PG_FATAL_UNBOUND, 0); return pg_host.log2_float(value); }
__attribute__((noreturn)) void __stack_chk_fail(void) { die(PG_FATAL_STACK_CHECK, 0); }
int32_t __gxx_personality_v0(int32_t version, int32_t actions, uint64_t exception_class, void *exception, void *context) {
    if (!ready()) die(PG_FATAL_UNBOUND, 0);
    return pg_host.native_personality(version, actions, exception_class, exception, context);
}
} // extern "C"
