/* SPDX-License-Identifier: GPL-3.0-only */
#include "enb_runtime.h"

extern "C" {
#include "mod_plugins.h"
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool readFile(const std::string& path, std::string& out) {
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > ENB_FX_MAX_BYTES) { close(fd); return false; }
    out.resize(size_t(st.st_size));
    size_t have = 0;
    while (have < out.size()) {
        const ssize_t got = read(fd, &out[have], out.size() - have);
        if (got <= 0) break;
        have += size_t(got);
    }
    close(fd);
    return have == out.size();
}

// Replace #include "name" with the file's text from the preset folder (matched without case), up to 8 levels.
bool inlineIncludes(const std::string& text, const std::string& dir, unsigned depth, std::string& out, std::string& log) {
    if (depth > 8) { log += "includes nest too deeply\n"; return false; }
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const size_t q1 = line.find('"'), q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
        if (line.compare(i, 8, "#include") == 0 && q2 != std::string::npos) {
            const std::string name = line.substr(q1 + 1, q2 - q1 - 1);
            char path[4096];
            std::string body;
            if (skyrim_plugin_child_path(dir.c_str(), name.c_str(), path, sizeof path) != 1 || !readFile(path, body)) {
                log += "include not found: " + name + "\n";
                return false;
            }
            if (!inlineIncludes(body, dir, depth + 1, out, log)) return false;
            out += "\n";
        } else {
            out += line + "\n";
        }
        if (out.size() > size_t(ENB_FX_MAX_BYTES)) { log += "effect source too large\n"; return false; }
        at = end + 1;
    }
    return true;
}

template <class T> struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    Ref(Ref&& o) noexcept : p(o.p) { o.p = nullptr; }
    Ref& operator=(Ref&& o) noexcept { if (this != &o) { reset(); p = o.p; o.p = nullptr; } return *this; }
    T** put() { reset(); return &p; }
    T* get() const { return p; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};
} // namespace

struct EnbRuntime::Impl {
    EnbPreset preset{};
    struct Stage {
        std::unique_ptr<EnbEffectExec> exec;
        std::vector<EnbFxUniform> uniforms;
        bool loaded = false;
    } stages[ENB_EFFECT_COUNT];
    Ref<IDirect3DTexture9> images[ENB_EFFECT_COUNT];   // output of adaptation, bloom and lens
    Ref<IDirect3DTexture9> neutral[3];                 // 1x1 stand-ins when a stage is not run: white adaptation, black bloom and lens
    bool produced[3] = {false, false, false};
    Ref<IDirect3DTexture9> chain[2];
    D3DSURFACE_DESC chainDesc{};
    IDirect3DDevice9* device = nullptr;
    bool loaded = false;
    ~Impl() { enb_preset_free(&preset); }
};

EnbRuntime::EnbRuntime() : impl_(new Impl) {}
EnbRuntime::~EnbRuntime() = default;
const EnbPreset& EnbRuntime::preset() const { return impl_->preset; }

unsigned EnbRuntime::effectsLoaded() const {
    unsigned n = 0;
    for (const auto& s : impl_->stages) n += s.loaded;
    return n;
}

unsigned long long EnbRuntime::drawCount() const {
    unsigned long long n = 0;
    for (const auto& s : impl_->stages) if (s.exec) n += s.exec->drawCount();
    return n;
}

void EnbRuntime::release() {
    Impl& m = *impl_;
    for (auto& s : m.stages) { if (s.exec) s.exec->release(); s.loaded = false; }
    for (auto& i : m.images) i.reset();
    for (auto& n : m.neutral) n.reset();
    m.chain[0].reset();
    m.chain[1].reset();
    m.device = nullptr;
    m.loaded = false;
}

bool EnbRuntime::load(IDirect3DDevice9* device, const char* root, const EnbEffectExec::Compile& compile, std::string& log) {
    release();
    Impl& m = *impl_;
    enb_preset_free(&m.preset);
    if (!device || !root || enb_preset_load(root, &m.preset)) { log += "no enbseries.ini at the preset folder\n"; return false; }
    char dir[4096];
    if (skyrim_plugin_child_path(root, "enbseries", dir, sizeof dir) != 1) { log += "no enbseries folder\n"; return false; }
    for (int e = 0; e < ENB_EFFECT_COUNT; ++e) {
        const EnbEffectInfo& info = m.preset.effects[e];
        if (!info.present || !info.enabled || !info.usable) continue;
        char path[4096];
        std::string text, source;
        std::string name = enb_effect_file(EnbEffect(e));
        if (skyrim_plugin_child_path(dir, name.c_str(), path, sizeof path) != 1 || !readFile(path, text) ||
            !inlineIncludes(text, dir, 0, source, log)) {
            log += name + ": cannot be read\n";
            continue;
        }
        std::unique_ptr<EnbFx> scan(new EnbFx);
        EnbLowered lowered;
        if (enb_fx_scan(source.data(), source.size(), scan.get()) || enb_fx_lower(source.data(), source.size(), &lowered)) {
            log += name + ": cannot be parsed\n";
            continue;
        }
        auto exec = std::make_unique<EnbEffectExec>();
        std::string effectLog;
        const bool ok = exec->load(device, lowered, compile, effectLog);
        enb_lowered_free(&lowered);
        if (!ok) { log += name + ": switched off: " + effectLog; continue; }
        // UI variables take their values from the ini (section [EFFECT], key = the variable's UIName).
        for (unsigned u = 0; u < scan->uniform_count; ++u) {
            const EnbFxUniform& uniform = scan->uniforms[u];
            if (!uniform.ui_name[0]) continue;
            const char* value = enb_ini_get(&m.preset.ini, "EFFECT", uniform.ui_name);
            float v[4];
            const int n = value ? enb_value_parse(value, v) : 0;
            if (n) exec->setConstant(uniform.name, v, unsigned(n));
        }
        m.stages[e].uniforms.assign(scan->uniforms, scan->uniforms + scan->uniform_count);
        m.stages[e].exec = std::move(exec);
        m.stages[e].loaded = true;
    }
    m.device = device;
    m.loaded = effectsLoaded() > 0;
    if (!m.loaded) log += "no effect of the preset could be loaded\n";
    return m.loaded;
}

HRESULT EnbRuntime::run(IDirect3DDevice9* d, IDirect3DTexture9* scene, IDirect3DSurface9* output, unsigned quality) {
    Impl& m = *impl_;
    if (!m.loaded || d != m.device || !scene || !output) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC target{};
    HRESULT r = output->GetDesc(&target);
    if (FAILED(r)) return r;
    EnbPlan plan;
    // Only effects that really loaded take part; the plan sees the preset with the rest switched off.
    EnbPreset view = m.preset;
    view.ini = {};
    for (int e = 0; e < ENB_EFFECT_COUNT; ++e) if (!m.stages[e].loaded) view.effects[e].enabled = 0;
    enb_plan_build(&view, quality, depth_ != nullptr, &plan);
    plan_ = plan;
    if (!plan.count) return D3D_OK;

    EnbVars vars;
    enb_vars_compute(&state_, &vars);
    EnbGameState state = state_;
    if (!state.width) { state.width = target.Width; state.height = target.Height; }
    enb_vars_compute(&state, &vars);
    for (int e = 0; e < ENB_EFFECT_COUNT; ++e) {
        EnbEffectExec* exec = m.stages[e].exec.get();
        if (!exec || !m.stages[e].loaded) continue;
        exec->setConstant("Timer", vars.timer, 4);
        exec->setConstant("ScreenSize", vars.screen_size, 4);
        exec->setConstant("TimeOfDay1", vars.tod1, 4);
        exec->setConstant("TimeOfDay2", vars.tod2, 4);
        const float nd[4] = {vars.night_day, 0, 0, 0}, in[4] = {vars.interior, 0, 0, 0}, fov[4] = {vars.field_of_view, 0, 0, 0};
        exec->setConstant("ENightDayFactor", nd, 1);
        exec->setConstant("EInteriorFactor", in, 1);
        exec->setConstant("FieldOfView", fov, 1);
        exec->setImage("TextureOriginal", scene);
        exec->setImage("TextureDepth", depth_);
    }
    // Stages that do not run this frame leave neutral images, so the effects that sample them still work.
    for (int image = 0; image <= ENB_EFFECT_LENS; ++image) {
        m.produced[image] = false;
        if (!m.neutral[image].get()) {
            Ref<IDirect3DSurface9> surface;
            if (FAILED(r = d->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, target.Format, D3DPOOL_DEFAULT, m.neutral[image].put(), nullptr)) ||
                FAILED(r = m.neutral[image].get()->GetSurfaceLevel(0, surface.put())) ||
                FAILED(r = d->ColorFill(surface.get(), nullptr, image == ENB_EFFECT_ADAPTATION ? 0xffffffff : 0xff000000)))
                return r;
        }
    }
    auto bindImages = [&]() {
        static const char* const names[] = {"TextureAdaptation", "TextureBloom", "TextureLens"};
        for (int e = 0; e < ENB_EFFECT_COUNT; ++e) {
            EnbEffectExec* exec = m.stages[e].exec.get();
            if (!exec || !m.stages[e].loaded) continue;
            for (int image = 0; image <= ENB_EFFECT_LENS; ++image)
                exec->setImage(names[image], m.produced[image] && m.images[image].get() ? static_cast<IDirect3DBaseTexture9*>(m.images[image].get())
                                                                                    : static_cast<IDirect3DBaseTexture9*>(m.neutral[image].get()));
        }
    };
    bindImages();   // a stage's image is real only after its pass ran; until then effects see the stand-in

    // Frame-sized chain textures for effects that follow one another.
    unsigned chainStages = 0;
    for (unsigned i = 0; i < plan.count; ++i) if (plan.steps[i].effect >= ENB_EFFECT_PREPASS) ++chainStages;
    if (chainStages > 1 && (!m.chain[0].get() || m.chainDesc.Width != target.Width || m.chainDesc.Height != target.Height ||
                            m.chainDesc.Format != target.Format)) {
        for (auto& c : m.chain) if (FAILED(r = d->CreateTexture(target.Width, target.Height, 1, D3DUSAGE_RENDERTARGET, target.Format,
                                                                D3DPOOL_DEFAULT, c.put(), nullptr))) return r;
        m.chain[0].get()->GetLevelDesc(0, &m.chainDesc);
    }
    IDirect3DTexture9* current = scene;
    unsigned chainIndex = 0, chainDone = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        const EnbPlanStep& step = plan.steps[i];
        EnbEffectExec* exec = m.stages[step.effect].exec.get();
        EnbEffectExec::Options options;
        options.scaleDiv = 1;
        Ref<IDirect3DSurface9> surface;
        if (step.effect <= ENB_EFFECT_LENS) {   // an image stage: its result is an image, not the frame
            const unsigned w = step.effect == ENB_EFFECT_ADAPTATION ? 1u : std::max(1u, target.Width / step.scale_div),
                           h = step.effect == ENB_EFFECT_ADAPTATION ? 1u : std::max(1u, target.Height / step.scale_div);
            D3DSURFACE_DESC have{};
            if (m.images[step.effect].get()) m.images[step.effect].get()->GetLevelDesc(0, &have);
            if (!m.images[step.effect].get() || have.Width != w || have.Height != h || have.Format != target.Format)
                if (FAILED(r = d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, target.Format, D3DPOOL_DEFAULT,
                                                m.images[step.effect].put(), nullptr))) return r;
            if (FAILED(r = m.images[step.effect].get()->GetSurfaceLevel(0, surface.put()))) return r;
            if (FAILED(r = exec->run(d, current, surface.get(), options))) return r;
            m.produced[step.effect] = true;
            bindImages();
            continue;
        }
        const bool last = chainDone + 1 == chainStages;
        IDirect3DSurface9* destination = output;
        if (!last) {
            if (FAILED(r = m.chain[chainIndex].get()->GetSurfaceLevel(0, surface.put()))) return r;
            destination = surface.get();
        }
        if (FAILED(r = exec->run(d, current, destination, options))) return r;
        if (!last) { current = m.chain[chainIndex].get(); chainIndex ^= 1; }
        ++chainDone;
    }
    return D3D_OK;
}
