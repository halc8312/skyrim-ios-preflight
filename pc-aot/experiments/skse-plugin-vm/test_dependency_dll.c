/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>

static volatile uint32_t attached, checkpoint;

#if defined(HELPER_STATIC_TLS)
#include "test_tls_directory.h"
static __declspec(thread) volatile unsigned helper_tls = 40;
static void __stdcall helper_callback(void *image, unsigned reason, void *reserved) {
    (void)reserved;
    if (!image || _tls_index == UINT32_MAX || helper_tls != (reason ? 40u : 64u)) __builtin_trap();
    helper_tls += 2;
}
static const FixtureTlsCallback helper_callbacks[] = {helper_callback, 0};
FIXTURE_TLS_DIRECTORY(helper_callbacks);
#endif

int __stdcall OwnedDependencyEntry(void *image, unsigned reason, void *reserved) {
    if (!image || reserved) return 0;
#if defined(HELPER_STATIC_TLS)
    if (helper_tls != (reason ? 42u : 66u)) __builtin_trap();
    if (reason) helper_tls = 64;
#endif
    if (reason == 1) attached = 0x147a;
    if (reason == 0) attached = 0;
    return 1;
}

__declspec(dllexport) uint32_t __cdecl TransformFixtureValue(uint32_t value) {
    if (attached != 0x147a) return 0;
#if defined(HELPER_STATIC_TLS)
    if (helper_tls != 64) return 0;
#endif
    if (value == 1) {
        checkpoint = 11;
        return 14;
    }
    if (value == 2 && checkpoint == 11) {
        checkpoint = 17;
        return 29;
    }
    return 0;
}
