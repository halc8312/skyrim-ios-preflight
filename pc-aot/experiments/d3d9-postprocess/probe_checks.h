/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "postprocess.h"
#include <cstdio>
#include <stdexcept>
#include <string>

namespace postfx_probe {
inline void checked(HRESULT result, const char* action) {
    if (FAILED(result)) {
        char code[18];
        std::snprintf(code, sizeof code, " (0x%08x)", static_cast<unsigned>(result));
        throw std::runtime_error(std::string(action) + code);
    }
}

inline void expect(bool ok, const char* action) {
    if (!ok)
        throw std::runtime_error(action);
}

bool checkDepthInputs(IDirect3D9* api, IDirect3DDevice9* device, bool testFog);
}

#define TRY(call) ::postfx_probe::checked((call), #call)
