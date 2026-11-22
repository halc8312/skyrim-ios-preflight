/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "mod_highheel_effect_host.h"
#include "classic_mod_layout.h"
#include "classic_vm_epoch.h"
#include "highheel_effect.h"
#include "highheel_lifetimes.h"
#include "mod_highheel_pex_host.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern uint8_t *MEM;
extern struct CPU *rt_current_cpu(void);

static atomic_int startup_data = -1;
static HighHeelDigest digest_bytes;
static _Atomic(uint32_t) expected_setting;
static atomic_uint reported_phases;
static HighHeelLifetimes lifetimes;
static pthread_mutex_t lifetime_lock = PTHREAD_MUTEX_INITIALIZER;

bool skyrim_highheel_effect_prepare(int data, HighHeelDigest digest) {
    if (!digest || digest_bytes) return false;
    const int retained = fcntl(data, F_DUPFD_CLOEXEC, 0);
    if (retained < 0) return false;
    digest_bytes = digest;
    atomic_store_explicit(&startup_data, retained, memory_order_release);
    return true;
}

void skyrim_highheel_effect_cancel(void) {
    const int saved_errno = errno;
    const int data = atomic_exchange_explicit(&startup_data, -1, memory_order_acq_rel);
    if (data >= 0) close(data);
    pthread_mutex_lock(&lifetime_lock);
    atomic_store_explicit(&expected_setting, 0, memory_order_release);
    highheel_lifetimes_clear(&lifetimes);
    pthread_mutex_unlock(&lifetime_lock);
    errno = saved_errno;
}

static bool read_guest(void *unused, uint32_t address, void *bytes, size_t size) {
    (void)unused;
    if (!MEM || address < 0x10000u || size > UINT64_C(0x100000000) - address) return false;
    memcpy(bytes, MEM + address, size);
    return true;
}

static bool scope_current(SkyrimVmScope scope) {
    SkyrimVmScope current;
    return skyrim_vm_scope_current(&current) && scope.epoch == current.epoch && scope.registry == current.registry;
}

bool skyrim_highheel_effect_matches(SkyrimVmScope scope, uint64_t handle, uint32_t actor, bool keep,
                                     uint32_t magnitude) {
    if (!scope_current(scope)) return false;
    pthread_mutex_lock(&lifetime_lock);
    const bool matched = highheel_lifetimes_match(&lifetimes, scope.epoch, scope.registry, handle, actor,
                                                  keep, magnitude);
    pthread_mutex_unlock(&lifetime_lock);
    return matched && scope_current(scope);
}

static void plugins_ready(const struct CPU *cpu, uint32_t handler) {
    uint32_t current;
    if (!cpu || cpu != rt_current_cpu() || !handler ||
        !read_guest(NULL, CLASSIC_DATA_HANDLER, &current, sizeof current) || current != handler) return;
    const int data = atomic_exchange_explicit(&startup_data, -1, memory_order_acq_rel);
    if (data < 0) return;
    const int saved_errno = errno;
    ClassicModCatalog *catalog = NULL;
    uint32_t setting = 0;
    const HighHeelIdentityResult result = classic_mod_catalog_capture(read_guest, NULL, &catalog) == CLASSIC_MOD_CATALOG_OK
        ? highheel_identity_join(data, catalog, digest_bytes, &setting)
        : HIGHHEEL_IDENTITY_UNREADABLE;
    classic_mod_catalog_free(catalog);
    close(data);
    if (result == HIGHHEEL_IDENTITY_MATCH)
        atomic_store_explicit(&expected_setting, setting, memory_order_release);
    fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_CATALOG schema=1 result=%u\n", (unsigned)result);
    errno = saved_errno;
}

static void phase(const struct CPU *cpu, uint32_t effect, uint32_t arguments, uint32_t site) {
    const uint32_t setting = atomic_load_explicit(&expected_setting, memory_order_acquire);
    HighHeelEffectSample sample;
    SkyrimVmScope scope;
    if (!setting || !cpu || cpu != rt_current_cpu() ||
        !skyrim_vm_scope_current(&scope) ||
        !highheel_effect_sample(read_guest, NULL, effect, arguments, site, setting, &sample)) return;
    const bool linked = skyrim_highheel_pex_linked_in(scope.epoch);
    if (!scope_current(scope)) return;
    pthread_mutex_lock(&lifetime_lock);
    if (atomic_load_explicit(&expected_setting, memory_order_acquire) == setting)
        highheel_lifetimes_observe(&lifetimes, scope.epoch, scope.registry, &sample);
    pthread_mutex_unlock(&lifetime_lock);
    const unsigned bit = 1u << (2u * (unsigned)sample.starting + (unsigned)linked);
    if (atomic_fetch_or_explicit(&reported_phases, bit, memory_order_relaxed) & bit) return;
    const int saved_errno = errno;
    fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_EFFECT schema=1 start=%u linked=%u\n",
            (unsigned)sample.starting, (unsigned)linked);
    errno = saved_errno;
}

void skyrim_highheel_effect_fill(PcGuestModHooks *table) {
    table->highheel_effect = (PcGuestModHighHeelEffectHooks){plugins_ready, phase};
}
