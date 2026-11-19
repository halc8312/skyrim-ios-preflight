/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdint.h>

/* One freestanding TLS directory per authored PE image. A normal plugin has
 * imports. Keep an approved, unused import so Win32 initializes this fixture. */
#pragma comment(linker, "/include:__imp__GetLastError@0")
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
__declspec(allocate(".tls")) unsigned char test_tls_start;
__declspec(allocate(".tls$ZZZ")) unsigned char test_tls_end;
unsigned long _tls_index = UINT32_MAX;
__asm__(".globl __tls_array\n.set __tls_array, 0x2c");

typedef void (__stdcall *FixtureTlsCallback)(void *, unsigned, void *);

#define FIXTURE_TLS_DIRECTORY(callbacks) \
    const struct { \
        uintptr_t start, end, index, callback_table; \
        uint32_t zero_fill, characteristics; \
    } _tls_used = { (uintptr_t)&test_tls_start, (uintptr_t)&test_tls_end, \
                    (uintptr_t)&_tls_index, (uintptr_t)(callbacks), 8, (3u << 20) }
