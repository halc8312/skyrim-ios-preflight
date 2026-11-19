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

#if FIXTURE_VARIANT == 5
static unsigned char slab[96];
static void initialize_string_buffers(void) {
    unsigned char *start = slab;
    unsigned char *overlap = slab + 1;
    unsigned char *fill = slab + 32;
    __asm {
        cld
        mov edi, fill
        mov eax, 12345678h
        mov ecx, 8
        rep stosd
        mov esi, fill
        mov edi, start
        mov ecx, 8
        rep movsd
        mov esi, start
        mov edi, overlap
        mov ecx, 8
        rep movsb
        mov edi, start
        add edi, 40
        mov eax, 0a132h
        mov ecx, 3
        std
        xor edx, edx
        rep stosw
        cld
        mov esi, fill
        mov edi, start
        add edi, 80
        xor ecx, ecx
        rep movsd
    }
}
#endif

#if FIXTURE_VARIANT >= 6 && FIXTURE_VARIANT <= 8
static const FixtureMessaging *messenger;
static volatile unsigned next_message;

static void __cdecl wrong_handler(FixtureMessage *message) {
    (void)message;
    messenger->get_dispatcher(1000);
}

static void __cdecl observe_message(FixtureMessage *message) {
    const char *sender = message->sender;
    if (message->type != next_message || message->type >= 2 || message->length || message->data ||
        !sender || sender[0] != 'S' || sender[1] != 'K' || sender[2] != 'S' || sender[3] != 'E' || sender[4]) {
        wrong_handler(message);
        return;
    }
    ++next_message;
#if FIXTURE_VARIANT == 8
    messenger->get_dispatcher(1);
#endif
}
#endif

#if FIXTURE_VARIANT == 9
__declspec(dllimport) uint32_t __cdecl TransformFixtureValue(uint32_t);
#endif

#if FIXTURE_VARIANT == 10
__declspec(dllimport) void *__cdecl malloc(size_t);
__declspec(dllimport) void *__cdecl calloc(size_t, size_t);
__declspec(dllimport) void *__cdecl realloc(void *, size_t);
__declspec(dllimport) void __cdecl free(void *);
static unsigned char *session_buffer;

static _Bool setup_allocation(void) {
    unsigned char *buffer = calloc(4, 8);
    unsigned char *blocking = malloc(24);
    if (!buffer || !blocking) return 0;
    if (buffer[0] || buffer[31]) return 0;
    buffer[0] = 27;
    buffer[31] = 59;
    unsigned char *larger = realloc(buffer, 144);
    free(blocking);
    if (!larger || larger[0] != 27 || larger[31] != 59) return 0;
    larger[143] = 17;
    session_buffer = larger;
    return 1;
}
#endif

#if FIXTURE_VARIANT == 11
__declspec(dllimport) void *__stdcall GetProcessHeap(void);
__declspec(dllimport) void *__stdcall HeapAlloc(void *, unsigned long, size_t);
__declspec(dllimport) void *__stdcall HeapReAlloc(void *, unsigned long, void *, size_t);
__declspec(dllimport) int __stdcall HeapFree(void *, unsigned long, void *);
static unsigned char *session_buffer;

static _Bool setup_allocation(void) {
    void *heap = GetProcessHeap();
    if (!heap) return 0;
    unsigned char *buffer = HeapAlloc(heap, 8, 32);
    if (!buffer || buffer[0] || buffer[31]) return 0;
    buffer[0] = 27;
    buffer[31] = 59;
    unsigned char *larger = HeapReAlloc(heap, 8, buffer, 144);
    if (!larger || larger[0] != 27 || larger[31] != 59 || larger[32] || larger[143]) return 0;
    larger[143] = 17;
    session_buffer = larger;
    return 1;
}
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
#if FIXTURE_VARIANT == 5
    initialize_string_buffers();
#endif
#if FIXTURE_VARIANT == 9
    if (TransformFixtureValue(1) != 14) return 0;
#endif
#if FIXTURE_VARIANT == 10 || FIXTURE_VARIANT == 11
    if (supported && !setup_allocation()) return 0;
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
#if FIXTURE_VARIANT == 5
    if (slab[0] != 0x78 || slab[1] != 0x78 || slab[8] != 0x78 || slab[9] != 0x56 ||
        slab[35] != 0x12 || slab[36] != 0x32 || slab[37] != 0xa1 || slab[40] != 0x32 ||
        slab[41] != 0xa1 || slab[42] != 0x34 || slab[80] != 0) return 0;
#endif
    if (api->get_release_index() != 48) return 0;
    unsigned handle = api->get_plugin_handle();
    if (handle == 0xffffffffu) return 0;
#if FIXTURE_VARIANT >= 6 && FIXTURE_VARIANT <= 8
    if (!api->query_interface) return 0;
#if FIXTURE_VARIANT == 7
    if (api->query_interface(2)) return 0;
#endif
    messenger = api->query_interface(5);
    if (!messenger || messenger->version < 2) return 0;
    next_message = 0;
    if (!messenger->register_listener(handle, NULL, wrong_handler) ||
        messenger->register_listener(handle, "another plugin", wrong_handler) ||
        !messenger->register_listener(handle, "sKsE", observe_message) ||
        !messenger->register_listener(handle, "SKSE", wrong_handler)) return 0;
#endif
#if FIXTURE_VARIANT == 9
    if (TransformFixtureValue(2) != 29) return 0;
#endif
#if FIXTURE_VARIANT == 10 || FIXTURE_VARIANT == 11
    if (!session_buffer || session_buffer[0] != 27 || session_buffer[31] != 59 || session_buffer[143] != 17) return 0;
#if FIXTURE_VARIANT == 10
    free(session_buffer);
#else
    if (!HeapFree(GetProcessHeap(), 0, session_buffer)) return 0;
#endif
    session_buffer = NULL;
#endif
    return 1;
}
