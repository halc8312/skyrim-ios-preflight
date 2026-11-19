/* SPDX-License-Identifier: GPL-3.0-only */
#define BUILD_FIXTURE
#include "test_fixture.h"
#include <stddef.h>

#ifndef FIXTURE_VARIANT
#define FIXTURE_VARIANT 1
#endif

// The second independently linked DLL exercises persistent writable PE data.
#if FIXTURE_VARIANT == 2 || FIXTURE_VARIANT == 3
static volatile unsigned session;
#define FIXTURE_NAME "WorkshopSample"
#define FIXTURE_VERSION 41
#else
#define FIXTURE_NAME "ClassicProbe"
#define FIXTURE_VERSION 17
#endif

#if FIXTURE_VARIANT == 3
__declspec(dllimport) unsigned long __stdcall GetTickCount(void);
static volatile uint32_t attached_at;
static volatile int attached;

int __stdcall TestModuleEntry(void *image, unsigned reason, void *reserved) {
    if (!image || reserved) return 0;
    if (reason == 1) {
        attached_at = GetTickCount();
        attached = 1;
    } else if (reason == 0) {
        attached = 0;
    }
    return 1;
}
#endif

#if FIXTURE_VARIANT == 4
__declspec(dllimport) void *__cdecl memcpy(void *destination, const void *source, size_t count);
__declspec(dllimport) void *__cdecl memmove(void *destination, const void *source, size_t count);
__declspec(dllimport) void *__cdecl memset(void *destination, int value, size_t count);
static char buffer[128];
#endif

_Bool __cdecl SKSEPlugin_Query(const FixtureInterface *api, FixtureInfo *info) {
    if (!api || !info) return 0;
    info->info_version = 1;
    info->name = FIXTURE_NAME;
    info->plugin_version = FIXTURE_VERSION;
    _Bool supported = api->skse_version == 0x01070030u &&
        api->runtime_version == 0x09200000u && !api->is_editor;
#if FIXTURE_VARIANT == 2 || FIXTURE_VARIANT == 3
    session = supported ? api->runtime_version ^ FIXTURE_VERSION : 0;
#endif
#if FIXTURE_VARIANT == 4
    static const char marker[] = "readable data";
    if (memset(buffer, 0x78, sizeof buffer) != buffer ||
        memcpy(buffer, marker, sizeof marker) != buffer ||
        memmove(buffer + 1, buffer, sizeof marker) != buffer + 1) return 0;
#endif
    return supported;
}

_Bool __cdecl SKSEPlugin_Load(const FixtureInterface *api) {
    if (!api || !api->get_release_index || !api->get_plugin_handle) return 0;
#if FIXTURE_VARIANT == 2 || FIXTURE_VARIANT == 3
    if (session != (api->runtime_version ^ FIXTURE_VERSION)) return 0;
#endif
#if FIXTURE_VARIANT == 3
    if (!attached || GetTickCount() - attached_at > 10000) return 0;
#endif
#if FIXTURE_VARIANT == 4
    if (buffer[0] != 'r' || buffer[1] != 'r' || buffer[2] != 'e' ||
        buffer[14] != 0 || buffer[127] != 0x78) return 0;
#endif
    if (api->get_release_index() != 48) return 0;
    return api->get_plugin_handle() != 0xffffffffu;
}
