/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* A VMIdentifier uses the high bit of its shared reference-count word as a
 * lock. The original engine can rebind its 64-bit handle between calls. */
static uint64_t skse_classic_identifier_handle(uint32_t identifier) {
    uint32_t *lock = GPTR(identifier + 0x18);
    uint32_t original;
    unsigned spins = 0;
    for (;;) {
        original = __atomic_load_n(lock, __ATOMIC_RELAXED);
        if (original & UINT32_C(0x80000000)) {
            if (spins <= 10000) { ++spins; SDL_Delay(0); }
            else SDL_Delay(1);
            continue;
        }
        if (__atomic_compare_exchange_n(lock, &original, original | UINT32_C(0x80000000),
                                        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
    }
    uint64_t handle = (uint64_t)M32(identifier + 0x14) << 32 | M32(identifier + 0x10);
    __atomic_store_n(lock, original, __ATOMIC_RELEASE);
    return handle;
}

/* Equivalent to Classic's NativeFunction receiver unpack and the event
 * registry's typed Create(). Registered handles may differ from the original
 * VM identifier, notably when the concrete form has a more specific type. */
static int skse_classic_input_target(CPU *cpu, const char *class_name,
                                     uint32_t registry, uint32_t base,
                                     uint32_t *owner_policy, uint64_t *target) {
    if (!registry || !base || base > UINT32_MAX - 7 || (base & 3)) return 0;
    uint32_t tag = M32(base);
    if (tag != 1 && (tag <= 15 || (tag & 1))) return 0;
    uint32_t identifier = M32(base + 4);
    uint32_t policy = M32(0x01BA7008);
    if (!identifier || identifier > UINT32_MAX - 0x1b || (identifier & 3) || !policy) return 0;
    uint32_t type;
    if (strcmp(class_name, "Form") == 0) type = 0;
    else if (strcmp(class_name, "Alias") == 0) type = 136;
    else if (strcmp(class_name, "ActiveMagicEffect") == 0) type = 139;
    else return 0;

    uint64_t handle = skse_classic_identifier_handle(identifier);
    uint32_t args[] = {type, (uint32_t)handle, (uint32_t)(handle >> 32)};
    if (!(uint8_t)skse_virtual(cpu, policy, 1, 3, args) ||
        !(uint8_t)skse_virtual(cpu, policy, 2, 2, args + 1)) return 0;
    uint32_t object = skse_virtual(cpu, policy, 8, 3, args);
    if (!object) return 0;
    *owner_policy = skse_virtual(cpu, registry, 0x2c, 0, NULL);
    if (!*owner_policy) return 0;
    if (!type) type = skse_virtual(cpu, object, 0x15, 0, NULL);
    uint32_t create[] = {type, object};
    /* IObjectHandlePolicy::Create returns UInt64 in EDX:EAX on Win32. */
    CPU saved = *cpu;
    cpu->r[1] = *owner_policy;
    uint32_t low = rt_invoke(cpu, M32(M32(*owner_policy) + 4 * 4), 2, create);
    *target = (uint64_t)cpu->r[2] << 32 | low;
    *cpu = saved;
    return 1;
}

static void skse_classic_input_reference(CPU *cpu, uint32_t policy, uint64_t handle, int retain) {
    uint32_t args[] = {(uint32_t)handle, (uint32_t)(handle >> 32)};
    skse_virtual(cpu, policy, retain ? 9 : 10, 2, args);
}
