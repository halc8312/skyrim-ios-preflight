/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "device_bridge.h"
#include <d3d9.h>

#ifdef __cplusplus
extern "C" {
#endif
ClassicDeviceBackend classic_d3d9_depth_backend(IDirect3DDevice9* device);
#ifdef __cplusplus
}
#endif
