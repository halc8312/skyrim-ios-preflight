/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_CLASSIC_D3D9_DEVICE_BRIDGE_H
#define SKYRIM_CLASSIC_D3D9_DEVICE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CLASSIC_COM_ADDREF_SLOT = 1,
    CLASSIC_COM_RELEASE_SLOT = 2,
    CLASSIC_COM_GET_RENDER_STATE_SLOT = 58,
    CLASSIC_DEVICE9_SLOTS = 119,
    CLASSIC_DEVICE9_BYTES = 4 + 4 * CLASSIC_DEVICE9_SLOTS,
    CLASSIC_RS_ZENABLE = 7
};

/* Guest method tokens never resolve to addresses in the native process. */
#define CLASSIC_COM_ADDREF_TOKEN UINT32_C(0x7d100004)
#define CLASSIC_COM_RELEASE_TOKEN UINT32_C(0x7d100008)
#define CLASSIC_COM_GET_RENDER_STATE_TOKEN UINT32_C(0x7d1000e8)
#define CLASSIC_D3D_INVALID_CALL UINT32_C(0x8876086c)

typedef struct {
    uint32_t base;
    uint8_t* bytes;
    size_t length;
} ClassicGuestSpan;

/* Only this trusted backend ever sees its native device or a native output pointer. */
typedef struct {
    void* device;
    void (*retain)(void*);
    void (*release)(void*);
    uint32_t (*get_depth_state)(void*, uint32_t*);
} ClassicDeviceBackend;

typedef struct {
    ClassicGuestSpan object, stack, output;
    ClassicDeviceBackend backend;
    uint32_t references;
} ClassicDeviceProxy;

typedef enum {
    CLASSIC_DEVICE_OK,
    CLASSIC_DEVICE_BAD_LAYOUT,
    CLASSIC_DEVICE_UNKNOWN_METHOD,
    CLASSIC_DEVICE_BAD_OBJECT,
    CLASSIC_DEVICE_BAD_STACK,
    CLASSIC_DEVICE_REFERENCE_OVERFLOW
} ClassicDeviceStatus;

/* A fresh, zero-initialized proxy owns one native reference for its entire
 * guest lifetime. All calls to this one-device proxy are serialized. Its
 * issuer reserves the three non-overlapping guest ranges and keeps their
 * native backing valid through close. Object and table are guest-read-only. */
ClassicDeviceStatus classic_device_open(ClassicDeviceProxy* proxy, ClassicDeviceBackend backend,
                                        ClassicGuestSpan object, ClassicGuestSpan stack,
                                        ClassicGuestSpan output);
void classic_device_close(ClassicDeviceProxy* proxy);

/* On success advances the guest stdcall stack and sets guest EAX. On a bad
 * receiver, method or call frame, neither guest register is changed. */
ClassicDeviceStatus classic_device_call(ClassicDeviceProxy* proxy, uint32_t token, uint32_t* guest_sp,
                                        uint32_t* guest_eax);

#ifdef __cplusplus
}
#endif
#endif
