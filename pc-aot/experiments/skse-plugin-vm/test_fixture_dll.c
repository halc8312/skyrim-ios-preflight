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

#if FIXTURE_VARIANT == 12
static __declspec(align(16)) const unsigned char vector_input[16] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x1a, 0x2b
};
static __declspec(align(16)) unsigned char vector_output[96];

static void setup_vectors(void) {
    const unsigned char *src = vector_input;
    unsigned char *dst = vector_output;
    __asm {
        mov esi, src
        mov edi, dst
        movdqa xmm0, xmmword ptr [esi]
        movdqu xmmword ptr [edi + 1], xmm0
        movups xmm1, xmmword ptr [edi + 1]
        pxor xmm1, xmm0
        movaps xmmword ptr [edi + 32], xmm1
        mov eax, 12345678h
        movd xmm2, eax
        movss xmm1, dword ptr [edi + 1]
        movss xmm1, xmm2
        movss dword ptr [edi + 50], xmm1
        movq qword ptr [edi + 64], xmm2
        movq xmm3, qword ptr [edi + 1]
        movq qword ptr [edi + 72], xmm3
        xorps xmm4, xmm4
        orps xmm4, xmm0
        andps xmm4, xmm0
        movd eax, xmm4
        mov dword ptr [edi + 88], eax
    }
}
#endif

#if FIXTURE_VARIANT == 13
static uint32_t arithmetic_output[9];

static void setup_arithmetic(void) {
    uint32_t *out = arithmetic_output;
    __asm {
        mov edi, out
        mov eax, -1000000001
        cdq
        mov ecx, 75
        idiv ecx
        mov dword ptr [edi], eax
        mov dword ptr [edi + 4], edx
        mov eax, 0f1234567h
        xor edx, edx
        mov ecx, 10000h
        div ecx
        mov dword ptr [edi + 8], eax
        mov dword ptr [edi + 12], edx
        mov eax, 12345678h
        mov ecx, 16
        mul ecx
        mov dword ptr [edi + 16], eax
        mov dword ptr [edi + 20], edx
        mov ax, 0fff1h
        mov cl, 3
        idiv cl
        mov dword ptr [edi + 24], eax
        mov eax, 0fedcba98h
        sar eax, 4
        mov dword ptr [edi + 28], eax
        mov eax, 558811bbh
        shl eax, 1
        mov cl, 2
        shr eax, cl
        neg eax
        not eax
        mov dword ptr [edi + 32], eax
    }
}
#endif

#if FIXTURE_VARIANT == 14
__declspec(dllimport) unsigned long __stdcall TlsAlloc(void);
__declspec(dllimport) int __stdcall TlsFree(unsigned long);
__declspec(dllimport) void *__stdcall TlsGetValue(unsigned long);
__declspec(dllimport) int __stdcall TlsSetValue(unsigned long, void *);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
__declspec(dllimport) void __stdcall SetLastError(unsigned long);
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
typedef struct { unsigned words[6]; } FixtureCriticalSection;
__declspec(dllimport) void __stdcall InitializeCriticalSection(FixtureCriticalSection *);
__declspec(dllimport) int __stdcall InitializeCriticalSectionAndSpinCount(FixtureCriticalSection *, unsigned long);
__declspec(dllimport) void __stdcall EnterCriticalSection(FixtureCriticalSection *);
__declspec(dllimport) int __stdcall TryEnterCriticalSection(FixtureCriticalSection *);
__declspec(dllimport) void __stdcall LeaveCriticalSection(FixtureCriticalSection *);
__declspec(dllimport) void __stdcall DeleteCriticalSection(FixtureCriticalSection *);
static unsigned long index, original_thread;
static FixtureCriticalSection section;

static _Bool setup_thread_values(void) {
    SetLastError(0x1234);
    index = TlsAlloc();
    if (index == 0xffffffffu || GetLastError() != 0x1234) return 0;
    if (TlsGetValue(index) != NULL || GetLastError() != 0) return 0;
    SetLastError(0x5678);
    if (!TlsSetValue(index, (void *)0x12345678u) || GetLastError() != 0x5678) return 0;
    original_thread = GetCurrentThreadId();
    if (!original_thread || !InitializeCriticalSectionAndSpinCount(&section, 42)) return 0;
    EnterCriticalSection(&section);
    return TryEnterCriticalSection(&section) != 0;
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
#if FIXTURE_VARIANT == 12
    setup_vectors();
#endif
#if FIXTURE_VARIANT == 13
    setup_arithmetic();
#endif
#if FIXTURE_VARIANT == 14
    if (supported && !setup_thread_values()) return 0;
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
#if FIXTURE_VARIANT == 12
    if (vector_output[1] != 0x11 || vector_output[16] != 0x2b || vector_output[32] || vector_output[47] ||
        vector_output[50] != 0x78 || vector_output[53] != 0x12 || vector_output[54] ||
        vector_output[64] != 0x78 || vector_output[67] != 0x12 || vector_output[68] ||
        vector_output[72] != 0x11 || vector_output[79] != 0x88 ||
        vector_output[88] != 0x11 || vector_output[91] != 0x44) return 0;
#endif
#if FIXTURE_VARIANT == 13
    if (arithmetic_output[0] != (uint32_t)-13333333 || arithmetic_output[1] != (uint32_t)-26 ||
        arithmetic_output[2] != 0xf123 || arithmetic_output[3] != 0x4567 || arithmetic_output[4] != 0x23456780 ||
        arithmetic_output[5] != 1 || (arithmetic_output[6] & 0xffff) != 0x00fb ||
        arithmetic_output[7] != 0xffedcba9 || arithmetic_output[8] != (0x558811bbu >> 1) - 1) return 0;
#endif
#if FIXTURE_VARIANT == 14
    if (GetCurrentThreadId() != original_thread || TlsGetValue(index) != (void *)0x12345678u || GetLastError()) return 0;
    LeaveCriticalSection(&section);
    LeaveCriticalSection(&section);
    DeleteCriticalSection(&section);
    InitializeCriticalSection(&section);
    EnterCriticalSection(&section);
    LeaveCriticalSection(&section);
    DeleteCriticalSection(&section);
    SetLastError(17);
    if (!TlsFree(index) || GetLastError() != 17) return 0;
    if (TlsGetValue(0xffffffffu) != NULL || GetLastError() != 87) return 0;
#endif
    return 1;
}
