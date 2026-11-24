/* SPDX-License-Identifier: GPL-3.0-only
 * Runs a whole ENB preset (docs/ENB_PLAN.md): loads the effect files, fills their
 * variables from enbseries.ini and the engine variables, and each frame executes
 * the pass plan: adaptation, bloom and lens stages write images (TextureAdaptation,
 * TextureBloom, TextureLens) that later effects sample; prepass, depth of field,
 * the main effect and postpass chain over the frame.
 *
 * The data flow between stages, the ini section the UI variables are read from and
 * the image formats follow common presets and are UNVERIFIED against real ENB
 * files; the owner's presets decide (ENB_CODEX_HANDOFF.md).
 */
#pragma once
#include "enb_exec.h"
extern "C" {
#include "enb_preset.h"
}

class EnbRuntime {
  public:
    EnbRuntime();
    ~EnbRuntime();
    EnbRuntime(const EnbRuntime&) = delete;
    EnbRuntime& operator=(const EnbRuntime&) = delete;

    // presetRoot: the folder holding enbseries.ini and enbseries/. Effects that fail to compile are switched off
    // individually and named in log; the call fails only when nothing usable remains.
    bool load(IDirect3DDevice9* device, const char* presetRoot, const EnbEffectExec::Compile& compile, std::string& log);
    void setGameState(const EnbGameState& state) { state_ = state; }
    // A sampleable depth image for the effects that use TextureDepth (not owned). Without one they are skipped.
    void setDepth(IDirect3DBaseTexture9* depth) { depth_ = depth; }
    // Execute the plan for this quality. scene: a sampleable copy of the frame. output: where the result goes.
    // With nothing to run the output is left untouched.
    HRESULT run(IDirect3DDevice9* device, IDirect3DTexture9* scene, IDirect3DSurface9* output, unsigned quality);
    void release();
    const EnbPlan& lastPlan() const { return plan_; }
    unsigned effectsLoaded() const;
    unsigned long long drawCount() const;
    const EnbPreset& preset() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    EnbGameState state_{};
    IDirect3DBaseTexture9* depth_ = nullptr;
    EnbPlan plan_{};
};
