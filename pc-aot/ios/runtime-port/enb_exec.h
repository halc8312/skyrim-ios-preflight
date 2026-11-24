/* SPDX-License-Identifier: GPL-3.0-only
 * Executes the passes of an ENB effect file through the public D3D9 interface
 * (docs/ENB_PLAN.md section 4): the host runs it inside its Present path on the
 * game's device (native DXVK in this port). It restores the game's state, never
 * calls Present and rejects a call made inside a scene.
 *
 * HLSL is compiled by an injected function (the app passes its vkd3d-shader
 * route; tests pass a libvkd3d-shader one), so this file has no compiler
 * dependency. A pass renders into a scratch texture when more passes follow;
 * the last pass writes the output surface. TextureColor is the previous pass's
 * result (the scene copy for the first pass).
 */
#pragma once
#include <d3d9.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "enb_fx.h"
}

class EnbEffectExec {
  public:
    // Compile one entry point of the lowered HLSL to D3D9 shader bytecode (with its constant table).
    using Compile = std::function<bool(const std::string& hlsl, const std::string& entry, const std::string& profile,
                                       std::vector<DWORD>& code, std::string& log)>;
    struct Options {
        unsigned scaleDiv = 1; // intermediate passes use 1/scaleDiv of the output size
        unsigned technique = 0;
    };

    EnbEffectExec();
    ~EnbEffectExec();
    EnbEffectExec(const EnbEffectExec&) = delete;
    EnbEffectExec& operator=(const EnbEffectExec&) = delete;

    // Compile every pass of every technique and create the shaders. On failure nothing is kept and log says why.
    bool load(IDirect3DDevice9* device, const EnbLowered& lowered, const Compile& compile, std::string& log);
    // Float constants by variable name, applied to every shader that declares them. count: floats given.
    void setConstant(const std::string& name, const float* values, unsigned count);
    // Images by the engine/texture name used in the effect (TextureColor, TextureBloom, ...). Not owned.
    void setImage(const std::string& name, IDirect3DBaseTexture9* texture);
    unsigned techniqueCount() const;
    unsigned passCount(unsigned technique) const;
    // Draw the technique's passes from scene (a sampleable copy of the frame) into output (A8R8G8B8 / X8R8G8B8 render target).
    HRESULT run(IDirect3DDevice9* device, IDirect3DTexture9* scene, IDirect3DSurface9* output, const Options& options);
    void release();                  // before a device reset or teardown
    unsigned long long drawCount() const; // passes drawn so far

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
