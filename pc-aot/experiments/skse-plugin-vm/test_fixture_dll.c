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

#if FIXTURE_VARIANT == 9 || FIXTURE_VARIANT == 16
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
__declspec(dllimport) int __stdcall InitializeCriticalSectionEx(FixtureCriticalSection *, unsigned long, unsigned long);
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
    FixtureCriticalSection extended;
    if (!InitializeCriticalSectionEx(&extended, 42, 0x01000000u)) return 0;
    EnterCriticalSection(&extended);
    if (!TryEnterCriticalSection(&extended)) return 0;
    LeaveCriticalSection(&extended);
    LeaveCriticalSection(&extended);
    DeleteCriticalSection(&extended);
    if (!InitializeCriticalSectionEx(&extended, 0, 0)) return 0;
    DeleteCriticalSection(&extended);
    EnterCriticalSection(&section);
    return TryEnterCriticalSection(&section) != 0;
}
#endif

#if FIXTURE_VARIANT == 15 || FIXTURE_VARIANT == 16
#include "test_tls_directory.h"
static __declspec(thread) volatile unsigned tls_value = 7;

static void __stdcall test_tls_first(void *module, unsigned reason, void *reserved) {
    (void)reserved;
    if (!module) __builtin_trap();
    const unsigned expected = reason ? 7 : 25;
    if (_tls_index == UINT32_MAX || tls_value != expected) __builtin_trap();
    tls_value += 5;
}

static void __stdcall test_tls_second(void *module, unsigned reason, void *reserved) {
    (void)reserved;
    if (!module || tls_value != (reason ? 12u : 30u)) __builtin_trap();
    tls_value += 6;
}

static const FixtureTlsCallback test_tls_callbacks[] = {test_tls_first, test_tls_second, NULL};
FIXTURE_TLS_DIRECTORY(test_tls_callbacks);

int __stdcall TestModuleEntry(void *image, unsigned reason, void *reserved) {
    if (!image || reserved || tls_value != (reason ? 18u : 36u)) __builtin_trap();
    if (reason) tls_value = 25;
    return 1;
}
#endif

#if FIXTURE_VARIANT == 17
__declspec(dllimport) long __stdcall InterlockedIncrement(volatile long *);
__declspec(dllimport) long __stdcall InterlockedDecrement(volatile long *);
__declspec(dllimport) long __stdcall InterlockedExchange(volatile long *, long);
__declspec(dllimport) long __stdcall InterlockedExchangeAdd(volatile long *, long);
__declspec(dllimport) long __stdcall InterlockedCompareExchange(volatile long *, long, long);
static volatile long interlocked_value;

static _Bool check_interlocked(void) {
    interlocked_value = 2147483647;
    if (InterlockedIncrement(&interlocked_value) != (-2147483647L - 1L)) return 0;
    if (InterlockedDecrement(&interlocked_value) != 2147483647L) return 0;
    if (InterlockedExchange(&interlocked_value, 19) != 2147483647L) return 0;
    if (InterlockedExchangeAdd(&interlocked_value, -23) != 19 || interlocked_value != -4) return 0;
    if (InterlockedCompareExchange(&interlocked_value, 31, 9) != -4 || interlocked_value != -4) return 0;
    return InterlockedCompareExchange(&interlocked_value, 31, -4) == -4 && interlocked_value == 31;
}
#endif

#if FIXTURE_VARIANT == 18
typedef unsigned long (__stdcall *FixtureLastError)(void);
__declspec(dllimport) void *__stdcall GetModuleHandleA(const char *);
__declspec(dllimport) void *__stdcall GetModuleHandleW(const wchar_t *);
__declspec(dllimport) FixtureLastError __stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
__declspec(dllimport) void __stdcall SetLastError(unsigned long);

static _Bool check_named_module(void) {
    void *kernel = GetModuleHandleA("kErNeL32");
    if (!kernel || kernel != GetModuleHandleW(L"KerNel32.DlL")) return 0;
    FixtureLastError last_error = GetProcAddress(kernel, "GetLastError");
    if (!last_error) return 0;
    SetLastError(917);
    if (last_error() != 917) return 0;
    if (GetProcAddress(kernel, "getlasterror") || GetLastError() != 127) return 0;
    if (GetModuleHandleA("OwnedFixtureNotLoaded") || GetLastError() != 126) return 0;
    return 1;
}
#endif

#if FIXTURE_VARIANT == 19
static volatile uint32_t shared_word;
static _Bool check_locked_instructions(void) {
    volatile uint32_t *address = &shared_word;
    uint32_t before_add, before_miss, before_exchange;
    unsigned char hit, miss, add_carry;
    shared_word = 0xfffffffeu;
    __asm {
        mov edx, address
        mov ecx, 3
        lock xadd dword ptr [edx], ecx
        setc add_carry
        mov before_add, ecx
        mov eax, 1
        mov ecx, 9
        lock cmpxchg dword ptr [edx], ecx
        setz hit
        mov eax, 69
        lock cmpxchg dword ptr [edx], ecx
        setz miss
        mov before_miss, eax
        mov ecx, 77
        xchg dword ptr [edx], ecx
        mov before_exchange, ecx
        lock sub dword ptr [edx], 13
        lock inc word ptr [edx]
        lock dec byte ptr [edx]
    }
    return before_add == 0xfffffffeu && add_carry && hit && !miss &&
           before_miss == 9 && before_exchange == 9 && shared_word == 64;
}
#endif

#if FIXTURE_VARIANT == 20
__declspec(dllimport) void *__stdcall CreateFileA(const char *, uint32_t, uint32_t, void *, uint32_t, uint32_t, void *);
__declspec(dllimport) void *__stdcall CreateFileW(const wchar_t *, uint32_t, uint32_t, void *, uint32_t, uint32_t, void *);
__declspec(dllimport) int __stdcall ReadFile(void *, void *, uint32_t, uint32_t *, void *);
__declspec(dllimport) uint32_t __stdcall GetFileSize(void *, uint32_t *);
__declspec(dllimport) int __stdcall GetFileSizeEx(void *, int64_t *);
__declspec(dllimport) uint32_t __stdcall GetFileType(void *);
__declspec(dllimport) uint32_t __stdcall GetFileAttributesA(const char *);
__declspec(dllimport) uint32_t __stdcall GetFileAttributesW(const wchar_t *);
__declspec(dllimport) int __stdcall SetFilePointerEx(void *, int64_t, int64_t *, uint32_t);
__declspec(dllimport) uint32_t __stdcall SetFilePointer(void *, int32_t, int32_t *, uint32_t);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
__declspec(dllimport) void __stdcall SetLastError(unsigned long);

static _Bool check_legacy_seek(void *file) {
    int64_t position = -1;
    int32_t high = 0;
    SetLastError(777);
    if (SetFilePointer(file, 5, NULL, 0) != 5 || GetLastError() != 777 ||
        SetFilePointer(file, -1, NULL, 1) != 4 ||
        SetFilePointer(file, -8, NULL, 2) != UINT32_MAX || GetLastError() != 131 ||
        SetFilePointer(file, 0, NULL, 1) != 4) return 0;
    SetLastError(777);
    if (SetFilePointer(file, -1, &high, 0) != UINT32_MAX || GetLastError() != 0 || high ||
        SetFilePointer(file, 1, NULL, 1) != UINT32_MAX || GetLastError() != 87 ||
        !SetFilePointerEx(file, 0, &position, 1) || position != 0xffffffffLL) return 0;
    high = 1;
    if (SetFilePointer(file, 3, &high, 0) != 3 || high != 1 ||
        SetFilePointer(file, -4, &high, 9) != UINT32_MAX || GetLastError() != 87 || high != 1 ||
        !SetFilePointerEx(file, 0, &position, 1) || position != 0x100000003LL) return 0;
    high = -1;
    return SetFilePointer(file, -4, &high, 1) == UINT32_MAX && GetLastError() == 0 && !high &&
           SetFilePointer(file, 0, NULL, 0) == 0;
}

static _Bool check_file_assets(void) {
    const uint32_t read_access = 0x80000000u;
    void *const invalid = (void *)(uintptr_t)UINT32_MAX;
    uint32_t attributes = GetFileAttributesA("Data\\SKSE");
    if (attributes == UINT32_MAX || !(attributes & 0x10)) return 0;
    attributes = GetFileAttributesW(L"Data/SKSE/Plugins/\u8a2d\u5b9a.ini");
    if (attributes == UINT32_MAX || (attributes & 0x10) ||
        GetFileAttributesW(L"OwnedAbsent.ini") != UINT32_MAX || GetLastError() != 2) return 0;
    void *file = CreateFileA("Data\\SKSE\\Plugins\\Settings.ini", read_access, 0, NULL, 3, 0x80, NULL);
    if (file == invalid) return 0;
    void *second = CreateFileW(L"Data/SKSE/Plugins/Settings.ini", read_access, 1, NULL, 3, 0x80, NULL);
    if (second != invalid || GetLastError() != 32) return 0;
    uint32_t high = 12;
    int64_t size = -1;
    if (GetFileType(file) != 1 || GetFileType(invalid) || GetLastError() != 6 ||
        GetFileSize(file, &high) != 7 || high || !GetFileSizeEx(file, &size) || size != 7) return 0;
    if (SetFilePointer(invalid, 0, NULL, 0) != UINT32_MAX || GetLastError() != 6 || !check_legacy_seek(file)) return 0;
    unsigned char bytes[8];
    uint32_t transferred = 19;
    if (!ReadFile(file, bytes, 2, &transferred, NULL) || transferred != 2 || bytes[0] != 'A' || bytes[1] != 'B') return 0;
    if (!SetFilePointerEx(file, 0, &size, 1) || size != 2 || !SetFilePointerEx(file, -1, NULL, 1) ||
        !ReadFile(file, bytes, 1, &transferred, NULL) || transferred != 1 || bytes[0] != 'B') return 0;
    if (!ReadFile(file, bytes, 8, &transferred, NULL) || transferred != 5 || bytes[0] != 'C' || bytes[4] != 'G') return 0;
    if (!SetFilePointerEx(file, 0x100000003LL, &size, 0) || size != 0x100000003LL ||
        !ReadFile(file, bytes, 8, &transferred, NULL) || transferred != 0) return 0;
    if (SetFilePointerEx(file, -8, &size, 2) || GetLastError() != 131 || size != 0x100000003LL ||
        SetFilePointerEx(file, 0, &size, 9) || GetLastError() != 87 ||
        !SetFilePointerEx(file, 0, &size, 1) || size != 0x100000003LL) return 0;
    if (!SetFilePointerEx(file, -2, &size, 2) || size != 5 ||
        !ReadFile(file, bytes, 8, &transferred, NULL) || transferred != 2 || bytes[0] != 'F' || bytes[1] != 'G' ||
        !CloseHandle(file)) return 0;
    second = CreateFileW(L"Data/SKSE/Plugins/\u8a2d\u5b9a.ini", read_access, 1, NULL, 3, 0x08000080, NULL);
    if (second == invalid || !ReadFile(second, bytes, 4, &transferred, NULL) ||
        transferred != 4 || bytes[0] != 'j' || bytes[3] != '\n' || !CloseHandle(second)) return 0;
    return CreateFileA("OwnedAbsent.ini", read_access, 1, NULL, 3, 0x80, NULL) == invalid && GetLastError() == 2;
}
#endif

#if FIXTURE_VARIANT == 21
__declspec(dllimport) void *__stdcall CreateEventA(void *, int, int, const char *);
__declspec(dllimport) void *__stdcall CreateEventW(void *, int, int, const wchar_t *);
__declspec(dllimport) void *__stdcall CreateSemaphoreA(void *, long, long, const char *);
__declspec(dllimport) void *__stdcall CreateSemaphoreW(void *, long, long, const wchar_t *);
__declspec(dllimport) int __stdcall ReleaseSemaphore(void *, long, long *);
__declspec(dllimport) int __stdcall SetEvent(void *);
__declspec(dllimport) int __stdcall ResetEvent(void *);
__declspec(dllimport) uint32_t __stdcall WaitForSingleObject(void *, uint32_t);
__declspec(dllimport) uint32_t __stdcall WaitForSingleObjectEx(void *, uint32_t, int);
__declspec(dllimport) uint32_t __stdcall WaitForMultipleObjects(uint32_t, void *const *, int, uint32_t);
__declspec(dllimport) uint32_t __stdcall WaitForMultipleObjectsEx(uint32_t, void *const *, int, uint32_t, int);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);

static _Bool check_semaphores(void) {
    const uint32_t timeout = 0x102;
    void *ready = CreateSemaphoreA(NULL, 1, 3, NULL);
    void *empty = CreateSemaphoreW(NULL, 0, 2, NULL);
    void *gate = CreateEventW(NULL, 0, 0, NULL);
    if (!ready || !empty || !gate || CreateSemaphoreW(NULL, 3, 2, NULL) || GetLastError() != 87) return 0;
    void *items[] = {empty, ready, gate};
    if (WaitForMultipleObjects(3, items, 1, 0) != timeout || WaitForMultipleObjects(3, items, 0, 0) != 1 ||
        WaitForSingleObject(ready, 0) != timeout) return 0;
    long prior = -1;
    if (!ReleaseSemaphore(ready, 3, &prior) || prior != 0 ||
        ReleaseSemaphore(ready, 1, &prior) || GetLastError() != 298 || prior ||
        !ReleaseSemaphore(empty, 1, NULL) || !SetEvent(gate)) return 0;
    if (WaitForMultipleObjectsEx(3, items, 1, 0, 0) != 0 || WaitForSingleObject(empty, 0) != timeout ||
        WaitForSingleObject(gate, 0) != timeout || WaitForSingleObject(ready, 0) != 0 ||
        WaitForSingleObject(ready, 0) != 0 || WaitForSingleObject(ready, 0) != timeout) return 0;
    return CloseHandle(gate) && CloseHandle(empty) && CloseHandle(ready);
}

static _Bool check_events(void) {
    const uint32_t timeout = 0x102, infinite = UINT32_MAX;
    void *manual = CreateEventA(NULL, 1, 0, NULL);
    void *automatic = CreateEventW(NULL, 0, 1, NULL);
    if (!manual || !automatic || WaitForSingleObject(manual, 0) != timeout) return 0;
    void *items[] = {manual, automatic};
    if (WaitForMultipleObjectsEx(2, items, 0, 0, 0) != 1 ||
        WaitForSingleObject(automatic, 0) != timeout) return 0;
    if (!SetEvent(manual) || !SetEvent(automatic) ||
        WaitForMultipleObjects(2, items, 1, infinite) != 0 ||
        WaitForSingleObjectEx(manual, infinite, 0) != 0 || WaitForSingleObject(automatic, 0) != timeout) return 0;
    return ResetEvent(manual) && WaitForSingleObject(manual, 0) == timeout &&
           CloseHandle(automatic) && CloseHandle(manual) && check_semaphores();
}
#endif

#if FIXTURE_VARIANT == 23
#include "test_tls_directory.h"

typedef uint32_t (__stdcall *FixtureWorker)(void *);
__declspec(dllimport) void *__stdcall CreateThread(void *, size_t, FixtureWorker, void *, uint32_t, uint32_t *);
__declspec(dllimport) uint32_t __stdcall ResumeThread(void *);
__declspec(dllimport) int __stdcall GetExitCodeThread(void *, uint32_t *);
__declspec(dllimport) uint32_t __stdcall WaitForSingleObject(void *, uint32_t);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) uint32_t __stdcall TlsAlloc(void);
__declspec(dllimport) int __stdcall TlsFree(uint32_t);
__declspec(dllimport) void *__stdcall TlsGetValue(uint32_t);
__declspec(dllimport) int __stdcall TlsSetValue(uint32_t, void *);
__declspec(dllimport) uint32_t __stdcall GetCurrentThreadId(void);
__declspec(dllimport) uint32_t __stdcall GetLastError(void);
__declspec(dllimport) void __stdcall SetLastError(uint32_t);

static __declspec(thread) volatile uint32_t local_mark = 7;
static volatile uint32_t child_stage;
static uint32_t child_id;

typedef struct {
    uint32_t slot, parent_id;
    volatile uint32_t answer;
} FixtureWorkerInput;

static void __stdcall worker_tls_callback(void *module, unsigned reason, void *reserved) {
    (void)reserved;
    if (!module) __builtin_trap();
    if (reason == 1) {
        if (local_mark != 7) __builtin_trap();
        local_mark = 13;
    } else if (reason == 2 && GetCurrentThreadId() == child_id) {
        if (local_mark != 7 || child_stage != 0) __builtin_trap();
        local_mark = 11;
        child_stage = 1;
    } else if (reason == 3 && GetCurrentThreadId() == child_id) {
        if (local_mark != 37 || child_stage != 3) __builtin_trap();
        local_mark = 41;
        child_stage = 4;
    }
}
static const FixtureTlsCallback worker_tls_callbacks[] = {worker_tls_callback, NULL};
FIXTURE_TLS_DIRECTORY(worker_tls_callbacks);

int __stdcall TestModuleEntry(void *module, unsigned reason, void *reserved) {
    (void)reserved;
    if (!module) return 0;
    if (reason == 1) {
        if (local_mark != 13) return 0;
        local_mark = 25;
    } else if (reason == 2 && GetCurrentThreadId() == child_id) {
        if (local_mark != 11 || child_stage != 1) __builtin_trap();
        local_mark = 17;
        child_stage = 2;
    } else if (reason == 3 && GetCurrentThreadId() == child_id) {
        if (local_mark != 41 || child_stage != 4) __builtin_trap();
        child_stage = 5;
        SetLastError(0x2222);
    }
    return 1;
}

static uint32_t __stdcall fixture_worker(void *parameter) {
    FixtureWorkerInput *input = parameter;
    uint32_t tid = GetCurrentThreadId();
    if (!input || !tid || tid != child_id || tid == input->parent_id ||
        child_stage != 2 || local_mark != 17) return 1;
    SetLastError(0x1234);
    if (TlsGetValue(input->slot) != NULL || GetLastError() != 0 ||
        !TlsSetValue(input->slot, (void *)0x5555) ||
        TlsGetValue(input->slot) != (void *)0x5555) return 2;
    local_mark = 37;
    input->answer = 73;
    child_stage = 3;
    return 42;
}

static _Bool check_child_thread(void) {
    if (local_mark != 25) return 0;
    FixtureWorkerInput input;
    input.slot = TlsAlloc();
    input.parent_id = GetCurrentThreadId();
    input.answer = 0;
    if (input.slot == UINT32_MAX || !input.parent_id || !TlsSetValue(input.slot, &input)) return 0;
    child_stage = 0;
    child_id = 0;
    void *thread = CreateThread(NULL, 0, fixture_worker, &input, 4, &child_id); // CREATE_SUSPENDED.
    if (!thread) { TlsFree(input.slot); return 0; }
    uint32_t code;
    _Bool ok = child_id && child_id != input.parent_id && child_stage == 0 &&
        WaitForSingleObject(thread, 0) == 258 && GetExitCodeThread(thread, &code) && code == 259;
    SetLastError(0x1111);
    if (ResumeThread(thread) != 1) __builtin_trap();
    // Successful resume and wait on the native reference preserve this thread's value.
    if (WaitForSingleObject(thread, UINT32_MAX) != 0) __builtin_trap();
    ok = ok && GetLastError() == 0x1111 && GetExitCodeThread(thread, &code) && code == 42 &&
        WaitForSingleObject(thread, 0) == 0 && child_stage == 5 && input.answer == 73 &&
        local_mark == 25 && GetCurrentThreadId() == input.parent_id && TlsGetValue(input.slot) == &input;
    child_id = 0;
    int closed = CloseHandle(thread);
    return TlsFree(input.slot) && closed && ok;
}
#endif

#if FIXTURE_VARIANT == 22
static const FixtureSerialization *save_api;
static unsigned save_stage;

static void __cdecl make_test_records(const FixtureSerialization *api) {
    unsigned char payload[] = {'m', 'o', 'd', '!'};
    if (api != save_api || save_stage != 5 || !api->write(0x54534554, 4, payload, 4) ||
        !api->open(0x48544f4d, 7) || !api->append(payload + 1, 1)) __builtin_trap();
    payload[0] = 'X';
    if (!api->append(payload, 1)) __builtin_trap();
    save_stage = 9;
}

static void __cdecl revert_test_records(const FixtureSerialization *api) {
    if (api != save_api || save_stage != 9) __builtin_trap();
    save_stage = 0;
}

static void __cdecl read_test_records(const FixtureSerialization *api) {
    uint32_t type, version, length;
    unsigned char part[4];
    if (api != save_api || save_stage || !api->next(&type, &version, &length) ||
        type != 0x54534554 || version != 4 || length != 4 || api->read(part, 2) != 2 ||
        part[0] != 'm' || part[1] != 'o') __builtin_trap();
    if (!api->next(&type, &version, &length) || type != 0x48544f4d || version != 7 ||
        length != 2 || api->read(part, 4) != 2 || part[0] != 'o' || part[1] != 'X' ||
        api->read(part, 1) || api->next(&type, &version, &length)) __builtin_trap();
    save_stage = 11;
}

static void __cdecl reject_synthetic_form_delete(uint64_t handle) {
    (void)handle;
    __builtin_trap();
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
#if FIXTURE_VARIANT == 9 || FIXTURE_VARIANT == 16
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
#if FIXTURE_VARIANT == 15 || FIXTURE_VARIANT == 16
    if (tls_value != 25) return 0;
#endif
#if FIXTURE_VARIANT == 17
    if (supported && !check_interlocked()) return 0;
#endif
#if FIXTURE_VARIANT == 18
    if (supported && !check_named_module()) return 0;
#endif
#if FIXTURE_VARIANT == 19
    if (supported && !check_locked_instructions()) return 0;
#endif
#if FIXTURE_VARIANT == 20
    if (supported && !check_file_assets()) return 0;
#endif
#if FIXTURE_VARIANT == 21
    if (supported && !check_events()) return 0;
#endif
#if FIXTURE_VARIANT == 23
    if (supported && !check_child_thread()) return 0;
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
#if FIXTURE_VARIANT == 9 || FIXTURE_VARIANT == 16
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
#if FIXTURE_VARIANT == 15 || FIXTURE_VARIANT == 16
    if (tls_value != 25) return 0;
#endif
#if FIXTURE_VARIANT == 17
    if (interlocked_value != 31) return 0;
#endif
#if FIXTURE_VARIANT == 22
    if (!api->query_interface) return 0;
    save_api = api->query_interface(3);
    if (!save_api || save_api->version != 4) return 0;
    save_api->set_uid(handle, 0x1ced1591);
    save_api->set_revert(handle, revert_test_records);
    save_api->set_save(handle, make_test_records);
    save_api->set_load(handle, read_test_records);
    save_api->set_deleted(handle, reject_synthetic_form_delete);
    save_stage = 5;
#endif
    return 1;
}
