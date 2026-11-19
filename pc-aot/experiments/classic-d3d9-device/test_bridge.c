/* SPDX-License-Identifier: GPL-3.0-only */
#include "device_bridge.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct Backend {
    unsigned holds, calls;
    uint32_t value, hr;
};
static void hold(void* p) { ++((struct Backend*)p)->holds; }
static void drop(void* p) {
    assert(((struct Backend*)p)->holds == 1);
    --((struct Backend*)p)->holds;
}
static uint32_t read_depth(void* p, uint32_t* word) {
    struct Backend* backend = p;
    ++backend->calls;
    *word = backend->value;
    return backend->hr;
}
static void put(uint8_t* p, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(word >> (i * 8));
}
static uint32_t get(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void) {
    uint8_t object[CLASSIC_DEVICE9_BYTES + 2], stack[64], out[12];
    memset(object, 0xa5, sizeof object);
    memset(stack, 0x7e, sizeof stack);
    memset(out, 0x33, sizeof out);
    ClassicGuestSpan view = {0x40000, object + 1, CLASSIC_DEVICE9_BYTES};
    ClassicGuestSpan args = {0x60000, stack, sizeof stack}, result = {0x50000, out + 1, sizeof out - 2};
    struct Backend b = {0, 0, 0xbcde6543, 0};
    ClassicDeviceBackend backend = {&b, hold, drop, read_depth};
    ClassicDeviceProxy p = {0};
    ClassicGuestSpan bad = args;
    bad.base = view.base + 12;
    assert(classic_device_open(&p, backend, view, bad, result) == CLASSIC_DEVICE_BAD_LAYOUT && !b.holds);
    bad = args;
    bad.bytes = view.bytes + 4;
    assert(classic_device_open(&p, backend, view, bad, result) == CLASSIC_DEVICE_BAD_LAYOUT && !b.holds);
    bad = args;
    bad.base = UINT32_MAX - 2;
    assert(classic_device_open(&p, backend, view, bad, result) == CLASSIC_DEVICE_BAD_LAYOUT && !b.holds);
    bad = result;
    bad.base = CLASSIC_COM_ADDREF_TOKEN;
    assert(classic_device_open(&p, backend, view, args, bad) == CLASSIC_DEVICE_BAD_LAYOUT && !b.holds);
    bad = view;
    --bad.length;
    assert(classic_device_open(&p, backend, bad, args, result) == CLASSIC_DEVICE_BAD_LAYOUT && !b.holds);
    assert(classic_device_open(&p, backend, view, args, result) == CLASSIC_DEVICE_OK && b.holds == 1);
    assert(classic_device_open(&p, backend, view, args, result) == CLASSIC_DEVICE_BAD_LAYOUT &&
           b.holds == 1);
    assert(get(view.bytes) == view.base + 4 && object[0] == 0xa5 && object[sizeof(object) - 1] == 0xa5);
    for (unsigned slot = 0; slot < CLASSIC_DEVICE9_SLOTS; ++slot) {
        uint32_t value = get(view.bytes + 4 + slot * 4);
        if (slot == 1)
            assert(value == CLASSIC_COM_ADDREF_TOKEN);
        else if (slot == 2)
            assert(value == CLASSIC_COM_RELEASE_TOKEN);
        else if (slot == 58)
            assert(value == CLASSIC_COM_GET_RENDER_STATE_TOKEN);
        else
            assert(value == 0);
    }
    uint32_t sp = args.base + 1, eax = 0x1234;
    put(stack + 1, 0x12345678);
    put(stack + 5, view.base);
    put(stack + 9, CLASSIC_RS_ZENABLE);
    put(stack + 13, result.base);
    assert(classic_device_call(&p, 0x12345678, &sp, &eax) == CLASSIC_DEVICE_UNKNOWN_METHOD &&
           sp == args.base + 1 && eax == 0x1234);
    for (unsigned i = 0; i < 16; ++i) {
        sp = args.base + args.length - i;
        eax = 0x5678;
        assert(classic_device_call(&p, CLASSIC_COM_GET_RENDER_STATE_TOKEN, &sp, &eax) ==
               CLASSIC_DEVICE_BAD_STACK);
        assert(sp == args.base + args.length - i && eax == 0x5678 && !b.calls);
    }
    sp = args.base + 1;
    eax = 0x1234;
    put(stack + 5, view.base + 4);
    assert(classic_device_call(&p, CLASSIC_COM_GET_RENDER_STATE_TOKEN, &sp, &eax) ==
           CLASSIC_DEVICE_BAD_OBJECT);
    assert(sp == args.base + 1 && eax == 0x1234 && !b.calls);
    put(stack + 5, view.base);
    const uint32_t denied[] = {0,
                               view.base,
                               view.base + 4 + 58 * 4,
                               args.base + 4,
                               UINT32_MAX,
                               result.base - 1,
                               result.base + (uint32_t)result.length - 3,
                               result.base + (uint32_t)result.length};
    for (unsigned i = 0; i < sizeof(denied) / sizeof(*denied); ++i) {
        sp = args.base + 1;
        put(stack + 13, denied[i]);
        assert(classic_device_call(&p, CLASSIC_COM_GET_RENDER_STATE_TOKEN, &sp, &eax) == CLASSIC_DEVICE_OK);
        assert(sp == args.base + 17 && eax == CLASSIC_D3D_INVALID_CALL && !b.calls);
    }
    for (unsigned i = 0; i < sizeof(out); ++i)
        assert(out[i] == 0x33);
    put(stack + 13, result.base + 1);
    put(stack + 9, 27);
    sp = args.base + 1;
    assert(classic_device_call(&p, CLASSIC_COM_GET_RENDER_STATE_TOKEN, &sp, &eax) == CLASSIC_DEVICE_OK);
    assert(eax == CLASSIC_D3D_INVALID_CALL && !b.calls);
    put(stack + 9, CLASSIC_RS_ZENABLE);
    for (unsigned offset = 0; offset <= result.length - 4; ++offset) {
        put(stack + 13, result.base + offset);
        sp = args.base + 1;
        b.hr = 1;
        assert(classic_device_call(&p, CLASSIC_COM_GET_RENDER_STATE_TOKEN, &sp, &eax) == CLASSIC_DEVICE_OK);
        assert(eax == 1 && get(result.bytes + offset) == b.value && b.calls == offset + 1);
    }
    assert(out[0] == 0x33 && out[sizeof(out) - 1] == 0x33);
    b.hr = 0x88760868;
    b.value = 0xfeed0000;
    sp = args.base + 1;
    put(stack + 13, result.base + 1);
    uint32_t saved = get(result.bytes + 1);
    assert(classic_device_call(&p, CLASSIC_COM_GET_RENDER_STATE_TOKEN, &sp, &eax) == CLASSIC_DEVICE_OK);
    assert(eax == b.hr && get(result.bytes + 1) == saved);
    put(stack + 5, view.base + 1);
    sp = args.base + 1;
    eax = 42;
    assert(classic_device_call(&p, CLASSIC_COM_RELEASE_TOKEN, &sp, &eax) == CLASSIC_DEVICE_BAD_OBJECT &&
           eax == 42 && p.references == 1);
    put(stack + 5, view.base);
    sp = args.base + 1;
    assert(classic_device_call(&p, CLASSIC_COM_ADDREF_TOKEN, &sp, &eax) == CLASSIC_DEVICE_OK && eax == 2 &&
           sp == args.base + 9);
    p.references = UINT32_MAX;
    sp = args.base + 1;
    eax = 42;
    assert(classic_device_call(&p, CLASSIC_COM_ADDREF_TOKEN, &sp, &eax) ==
               CLASSIC_DEVICE_REFERENCE_OVERFLOW &&
           eax == 42);
    p.references = 2;
    for (unsigned left = 2; left > 0; --left) {
        sp = args.base + 1;
        assert(classic_device_call(&p, CLASSIC_COM_RELEASE_TOKEN, &sp, &eax) == CLASSIC_DEVICE_OK &&
               eax == left - 1);
        assert(b.holds == (left > 1) && sp == args.base + 9);
    }
    sp = args.base + 1;
    eax = 42;
    assert(classic_device_call(&p, CLASSIC_COM_ADDREF_TOKEN, &sp, &eax) == CLASSIC_DEVICE_BAD_OBJECT &&
           eax == 42);
    assert(get(view.bytes) == 0 && !b.holds);
    classic_device_close(&p);
    assert(classic_device_open(&p, backend, view, args, result) == CLASSIC_DEVICE_BAD_LAYOUT && !b.holds);
    ClassicDeviceProxy abandoned = {0};
    assert(classic_device_open(&abandoned, backend, view, args, result) == CLASSIC_DEVICE_OK &&
           b.holds == 1);
    for (unsigned i = 0; i < 4; ++i) {
        sp = args.base + 1;
        assert(classic_device_call(&abandoned, CLASSIC_COM_ADDREF_TOKEN, &sp, &eax) == CLASSIC_DEVICE_OK);
    }
    assert(abandoned.references == 5 && b.holds == 1);
    classic_device_close(&abandoned);
    classic_device_close(&abandoned);
    assert(!b.holds && !abandoned.references && get(view.bytes) == 0);
    puts("Checked all 119 guest slots, stdcall frames, output bounds, HRESULTs and proxy lifetime");
}
