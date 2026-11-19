/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef OWNED_CLASSIC_DLL_TLS_TRACE_H
#define OWNED_CLASSIC_DLL_TLS_TRACE_H

#include <stdint.h>

enum { TLS_TEST_EVENTS = 32, TLS_TEST_INITIAL = 0x13579bdfu };
enum { TLS_CALLBACK_A = 1, TLS_CALLBACK_B = 2, TLS_DLL_ENTRY = 3 };

struct TlsTestEvent {
    uint32_t source, reason, value, count;
};

struct TlsTestTrace {
    uint32_t used;
    struct TlsTestEvent events[TLS_TEST_EVENTS];
};

#endif
