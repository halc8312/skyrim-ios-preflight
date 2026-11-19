/* SPDX-License-Identifier: GPL-3.0-only */
/* Freestanding, self-authored i386 test DLL. Only the Windows reference loads it. */
#include <windows.h>
#include "trace.h"

#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
__declspec(allocate(".tls")) unsigned char template_start;
__declspec(allocate(".tls$ZZZ")) unsigned char template_end;
unsigned long _tls_index = UINT32_MAX;
/* The x86 CRT supplies this absolute TEB-field offset to the compiler.
 * Provide the same SDK-defined location in this no-CRT test image. */
__asm__(".globl __tls_array\n.set __tls_array, 0x2c");

struct Slot {
    uint32_t value, count;
};
__declspec(thread) static volatile struct Slot slot = {TLS_TEST_INITIAL, 0};

static struct TlsTestTrace before_binding;
static struct TlsTestTrace *observer = &before_binding;

/* All notifications in this reference are serialized by the Windows loader.
 * The observer is a plain bounded buffer owned until after explicit unload. */
__declspec(noinline) static void record(uint32_t source, uint32_t reason) {
    uint32_t i = observer->used;
    if (i < TLS_TEST_EVENTS) {
        struct TlsTestEvent *event = &observer->events[i];
        event->source = source;
        event->reason = reason;
        event->value = _tls_index == UINT32_MAX ? UINT32_MAX : slot.value;
        event->count = _tls_index == UINT32_MAX ? UINT32_MAX : slot.count;
        observer->used = i + 1;
    }
}

static void NTAPI callback_a(void *module, DWORD reason, void *reserved) {
    (void)module;
    (void)reserved;
    record(TLS_CALLBACK_A, reason);
}
static void NTAPI callback_b(void *module, DWORD reason, void *reserved) {
    (void)module;
    (void)reserved;
    record(TLS_CALLBACK_B, reason);
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)module;
    (void)reserved;
    record(TLS_DLL_ENTRY, reason);
    return TRUE;
}

static PIMAGE_TLS_CALLBACK const callbacks[] = {callback_a, callback_b, 0};
/* Link.exe/lld find this conventional symbol. These are actual PE32 VAs,
 * including the writable cell into which Windows assigns the module slot. */
const IMAGE_TLS_DIRECTORY32 _tls_used = {
    (DWORD)(uintptr_t)&template_start,
    (DWORD)(uintptr_t)&template_end,
    (DWORD)(uintptr_t)&_tls_index,
    (DWORD)(uintptr_t)callbacks,
    12,
    {IMAGE_SCN_ALIGN_4BYTES}
};

__declspec(dllexport) uint32_t ReadValue(void) { return _tls_index == UINT32_MAX ? UINT32_MAX : slot.value; }
__declspec(dllexport) uint32_t ReadCount(void) { return _tls_index == UINT32_MAX ? UINT32_MAX : slot.count; }
__declspec(dllexport) uint32_t SetPair(uint32_t value, uint32_t count) {
    if (_tls_index == UINT32_MAX) return UINT32_MAX;
    uint32_t old = slot.value;
    slot.value = value;
    slot.count = count;
    return old;
}
__declspec(dllexport) void BindTrace(struct TlsTestTrace *out) {
    uint32_t n = before_binding.used;
    if (!out || n > TLS_TEST_EVENTS || observer != &before_binding) return;
    for (uint32_t i = 0; i < n; ++i) {
        const volatile struct TlsTestEvent *in = &before_binding.events[i];
        out->events[i].source = in->source;
        out->events[i].reason = in->reason;
        out->events[i].value = in->value;
        out->events[i].count = in->count;
    }
    out->used = n;
    observer = out;
}
