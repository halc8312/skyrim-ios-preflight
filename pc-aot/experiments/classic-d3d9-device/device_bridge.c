/* SPDX-License-Identifier: GPL-3.0-only */
#include "device_bridge.h"
#include <string.h>

static uint32_t read32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void write32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = (uint8_t)(value >> (8 * i));
}

static int valid_span(ClassicGuestSpan span, size_t minimum) {
    /* The virtual one-past address must fit in an x86 stack register. */
    return span.bytes && span.base && span.length >= minimum && span.length <= UINT32_MAX - span.base &&
           span.length <= UINTPTR_MAX - (uintptr_t)span.bytes;
}

static int disjoint(ClassicGuestSpan a, ClassicGuestSpan b) {
    uintptr_t pa = (uintptr_t)a.bytes, pb = (uintptr_t)b.bytes;
    return (a.base + a.length <= b.base || b.base + b.length <= a.base) &&
           (pa + a.length <= pb || pb + b.length <= pa);
}

static uint8_t* within(ClassicGuestSpan span, uint32_t address, size_t size) {
    if (address < span.base || size > span.length)
        return NULL;
    uint32_t offset = address - span.base;
    return offset <= span.length - size ? span.bytes + offset : NULL;
}

ClassicDeviceStatus classic_device_open(ClassicDeviceProxy* p, ClassicDeviceBackend backend,
                                        ClassicGuestSpan object, ClassicGuestSpan stack,
                                        ClassicGuestSpan output) {
    if (!p || p->object.base || !backend.device || !backend.retain || !backend.release ||
        !backend.get_depth_state || !valid_span(object, CLASSIC_DEVICE9_BYTES) || !valid_span(stack, 16) ||
        !valid_span(output, 4) || !disjoint(object, stack) || !disjoint(stack, output) ||
        !disjoint(output, object))
        return CLASSIC_DEVICE_BAD_LAYOUT;
    const uint32_t tokens[] = {CLASSIC_COM_ADDREF_TOKEN, CLASSIC_COM_RELEASE_TOKEN,
                               CLASSIC_COM_GET_RENDER_STATE_TOKEN};
    for (unsigned i = 0; i < sizeof(tokens) / sizeof(*tokens); ++i) {
        if (within(object, tokens[i], 1) || within(stack, tokens[i], 1) || within(output, tokens[i], 1))
            return CLASSIC_DEVICE_BAD_LAYOUT;
    }
    p->object = object;
    p->stack = stack;
    p->output = output;
    p->backend = backend;
    uint8_t* vtable = object.bytes + 4;
    memset(vtable, 0, 4 * CLASSIC_DEVICE9_SLOTS);
    write32(object.bytes, object.base + 4);
    write32(vtable + 4 * CLASSIC_COM_ADDREF_SLOT, tokens[0]);
    write32(vtable + 4 * CLASSIC_COM_RELEASE_SLOT, tokens[1]);
    write32(vtable + 4 * CLASSIC_COM_GET_RENDER_STATE_SLOT, tokens[2]);
    p->backend.retain(p->backend.device);
    p->references = 1;
    return CLASSIC_DEVICE_OK;
}

void classic_device_close(ClassicDeviceProxy* p) {
    if (p && p->references) {
        p->references = 0;
        write32(p->object.bytes, 0);
        p->backend.release(p->backend.device);
    }
}

ClassicDeviceStatus classic_device_call(ClassicDeviceProxy* p, uint32_t token, uint32_t* sp,
                                        uint32_t* eax) {
    if (!p || !sp || !eax)
        return CLASSIC_DEVICE_BAD_LAYOUT;
    if (token != CLASSIC_COM_ADDREF_TOKEN && token != CLASSIC_COM_RELEASE_TOKEN &&
        token != CLASSIC_COM_GET_RENDER_STATE_TOKEN)
        return CLASSIC_DEVICE_UNKNOWN_METHOD;
    if (!p->references)
        return CLASSIC_DEVICE_BAD_OBJECT;
    const uint32_t frame_size = token == CLASSIC_COM_GET_RENDER_STATE_TOKEN ? 16 : 8;
    const uint8_t* frame = within(p->stack, *sp, frame_size);
    if (!frame)
        return CLASSIC_DEVICE_BAD_STACK;
    if (read32(frame + 4) != p->object.base)
        return CLASSIC_DEVICE_BAD_OBJECT;
    uint32_t result;
    if (token == CLASSIC_COM_ADDREF_TOKEN) {
        if (p->references == UINT32_MAX)
            return CLASSIC_DEVICE_REFERENCE_OVERFLOW;
        result = ++p->references;
    } else if (token == CLASSIC_COM_RELEASE_TOKEN) {
        if (p->references == 1)
            classic_device_close(p);
        else
            --p->references;
        result = p->references;
    } else {
        uint8_t* destination = within(p->output, read32(frame + 12), 4);
        result = CLASSIC_D3D_INVALID_CALL;
        if (read32(frame + 8) == CLASSIC_RS_ZENABLE && destination) {
            uint32_t word = 0;
            result = p->backend.get_depth_state(p->backend.device, &word);
            if (!(result & UINT32_C(0x80000000)))
                write32(destination, word);
        }
    }
    *eax = result;
    *sp += frame_size;
    return CLASSIC_DEVICE_OK;
}
