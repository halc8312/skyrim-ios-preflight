#!/usr/bin/env python3
"""Stage the pinned public runtime for an iOS original-guest diagnostic build.

This reads public source only. It never opens game inputs or edits the vendor
checkout. Changed files are fingerprinted after universal-newline conversion so
the same patch is reproducible on Windows and macOS. A supplied output directory
contains public runtime source, not generated guest code.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys

UPSTREAM_REVISION = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
EXPECTED = {
    "main.c": "b7fbb8009ccbb2f0a16dfdf161118a7e8eb7d8d4b7ab3f6fa61dc3f303297666",
    "platform_sdl.c": "0c991520a8ab9fffc1cb76e22f03fa05d020926ea9c952a6e30a43af4dc01842",
    "d3d9_bridge.cpp": "2cb0991887f72ec89d1e618035cdd08e7d956a5f2f4f139fcdf80154e32de91f",
    "d3dx_shader.cpp": "4593e3cad0bd38ee7866d1f715f0df0e189e8716179c9aba70e96b8b6e96a496",
    "d3d9_bridge_gen.cpp": "575127d0fce5620a92b40ecd828bad87f3adbcadbabe1f4e2a080dcd90458419",
    "d3dx_native.cpp": "8aac049c9bb24b138b5ade384e867ff0a2ff3941edf7323dd295e11aa97c1146",
    "d3dx_image.c": "d2ccab42393299a8a851e684903120507fc672b557bc0d0166e4e0cb90622b4f",
    "d3dx_surface.cpp": "bddcd59dc98890391ba941bba4db53ec377c30039f76a99ca93f7fdf5a61266f",
    "d3d9_bcemu.cpp": "1fd6626f3015e117b5694e664c1a6c18d97072efdc56210d4f81ec5f109301ea",
    "shims.c": "7478c4f7ee7b9e75e3ec17acba5825b5f594a7966cfa747155252b935131c6f0",
    "prof.c": "2c0910c1620e844d47e2e1d2d1910091245f062144ce3253159e9f4e10f12c0c",
    "skyrim_shims.c": "3b1955df82fcd791e907ced07dfd8365d89f417f705d9f51e39f4fb04d1bd9c7",
    "touch.cpp": "ada0d43ab81ba5a37580004ec8e59835b4aa617baf6a9cae98276374b96fe027",
    "input.c": "d0c549822ae87ab22009b60854ca84fc15c69151e8fd5063bd9fe56c1b38f193",
    "kernel32.c": "c6360da8a234421feac620c5db529d7a488ff6fc16747a0ad275f6dc987a0dfc",
    "user32.c": "e4970ef14067b9779415a6872895b57f007c904f69682a1e395b1d96b61ef335",
    "dispatch.c": "17c9508a10b2174a7efc24b26f94de0f6c9ce58eee9fa966fe74b68140ba7ac0",
    "xaudio2.c": "91f0e31e583b8e03d1ecf35632fba2cd24abb42f986c3c796aa879d711bdfc42",
    "xwma.c": "5c0bd198d43e71bbdb52d7499bda7dceac4b4bb7219c59b48b9541c5756a7b20",
}


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"{label}: expected one patch anchor; found {count}")
    return text.replace(old, new, 1)


def mobile_conditions(text: str) -> str:
    # Apply only to the selected UI files, never Android system/affinity code.
    return (text.replace("defined(__ANDROID__)", "defined(SKYDREAM_MOBILE)")
                .replace("#ifdef __ANDROID__", "#ifdef SKYDREAM_MOBILE")
                .replace("#ifndef __ANDROID__", "#ifndef SKYDREAM_MOBILE"))


def patch_main(s: str) -> str:
    s = replace_once(s, '#include <SDL2/SDL.h>',
        '#include <SDL2/SDL.h>\n#include "ios_runtime.h"\n#include "mobile_ui.h"\n#include "PcGuestABI.h"', "main includes")
    s = replace_once(s, "int main(int argc, char **argv) {",
        "int skyrim_ios_runtime_main(int argc, char **argv) {\n"
        '    skyrim_ios_stage("runtime-start");', "main entry")
    s = replace_once(s, "    rt_host_bundle_environment();",
        "    /* iOS dependencies are linked statically; no desktop dylib search. */",
        "desktop dependency search")
    s = replace_once(s, "    FILE *f = open_guest_image(path);",
        "    FILE *f;\n"
        "    if (!strcmp(modname, GAME_EXE)) {\n"
        '        f = fopen("TESV_unpacked.exe", "rb");\n'
        '        if (!f) rt_fatal("cannot open the bound TESV_unpacked.exe input: %s", strerror(errno));\n'
        '        LOG("using the bound TESV_unpacked.exe input");\n'
        "    } else f = open_guest_image(path);", "exact iOS guest image")
    s = replace_once(s, 'static const char *const k_need[] = {GAME_EXE, "Data", NULL};',
        'static const char *const k_need[] = {"TESV_unpacked.exe", "Data", NULL};', "iOS input name")
    s = replace_once(s,
        '    if (!MEM) rt_fatal("cannot reserve guest address space");',
        '    if (!MEM) rt_fatal("cannot reserve 4 GiB guest address space: %s", strerror(errno));\n'
        '    if (pc_guest_bind_memory(MEM) != 0) rt_fatal("private guest memory binding failed");\n'
        '    skyrim_ios_stage("guest-memory-bound");', "guest memory binding")
    s = replace_once(s,
        '            if (M32(d + 12) && M32(M32(d + 12))) LOG("warning: TLS callbacks not run");',
        '            if (M32(d + 12) && M32(M32(d + 12)))\n'
        '                rt_fatal("unsupported PE TLS callbacks at %08x; entry was not run", M32(d + 12));',
        "PE TLS callback failure")
    s = replace_once(s,
        '    { extern void hud_screen_setup_guest(void); hud_screen_setup_guest(); }',
        '    /* Original-guest diagnostics do not rewrite fixed CursorMenu/HUD vtables. */',
        "HUD guest hooks")
    s = replace_once(s, "    rt_call(c, g_entry);",
        '    if (!g_entry) rt_fatal("guest PE entry is zero");\n'
        '    skyrim_ios_stage("guest-entry");\n'
        '    rt_call(c, g_entry);', "real guest entry marker")
    # Avoid the desktop resolution override; the SDL mobile size seeded below is
    # chosen by the application's initial render-size policy.
    s = replace_once(s,
        '#ifndef __ANDROID__\n        extern void bridge_scaled_mode',
        '#if !defined(__ANDROID__) && !defined(SKYRIM_IOS)\n        extern void bridge_scaled_mode',
        "mobile initial size")
    s = replace_once(s, '        skyrim_seed_prefs(w, h);',
        '        skyrim_mobile_render_size(&w, &h);\n        skyrim_seed_prefs(w, h);', "selected render size")
    return s


def patch_platform(s: str) -> str:
    s = mobile_conditions(s)
    s = replace_once(s, '#include <SDL2/SDL.h>',
        '#include <SDL2/SDL.h>\n#include <SDL2/SDL_vulkan.h>\n#include "ios_runtime.h"\n#include "mobile_core.h"', "platform includes")
    s = replace_once(s,
        '    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");',
        '    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");\n'
        '    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");\n'
        '    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");', "iOS window hints")
    s = replace_once(s,
        '    int w = 1280, h = 720;\n    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;',
        '    if (SDL_Vulkan_LoadLibrary(NULL) != 0)\n'
        '        rt_fatal("SDL Vulkan loader: %s", SDL_GetError());\n'
        '    if (!SDL_Vulkan_GetVkGetInstanceProcAddr())\n'
        '        rt_fatal("MoltenVK vkGetInstanceProcAddr is unavailable");\n'
        '    int w = 1280, h = 720;\n'
        '    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;',
        "Vulkan initialization")
    s = replace_once(s,
        '    if (!g_window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());',
        '    if (!g_window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());\n'
        '    SDL_GetWindowSize(g_window, &w, &h);\n'
        '    int drawable_w = 0, drawable_h = 0;\n'
        '    SDL_Vulkan_GetDrawableSize(g_window, &drawable_w, &drawable_h);\n'
        '    if (w <= 0 || h <= 0 || drawable_w <= 0 || drawable_h <= 0)\n'
        '        rt_fatal("invalid iOS window/drawable dimensions");\n'
        '    LOG("iOS drawable: %dx%d, logical window: %dx%d", drawable_w, drawable_h, w, h);\n'
        '    skyrim_ios_stage("vulkan-window-ready");', "Vulkan window validation")
    s = replace_once(s, '    skyrim_ios_stage("vulkan-window-ready");',
        '    skyrim_ios_set_window(g_window);\n'
        '    skyrim_ios_stage("vulkan-window-ready");', "main-thread window registration")
    s = replace_once(s, '    while (SDL_PollEvent(&e)) {',
        '    /* UIKit pumping belongs to the main thread even when Present moves\n'
        '     * to the guest loading/render thread. PeepEvents only drains the\n'
        '     * thread-safe SDL queue and never runs a worker CFRunLoop. */\n'
        '    skyrim_ios_pump_events();\n'
        '    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {',
        "main-thread UIKit event ownership")
    s = replace_once(s,
        '#if defined(GAME_SKYRIM) && defined(SKYDREAM_MOBILE)\n',
        '#if defined(GAME_SKYRIM) && defined(SKYDREAM_MOBILE) && !defined(SKYRIM_IOS_GUEST_DIAGNOSTIC)\n',
        "fixed gamepad flag")
    s = replace_once(s,
        "    {   /* canary: the main thread's static TLS pointer (TEB slot 0 +0x2C) */",
        "#ifndef SKYRIM_IOS_GUEST_DIAGNOSTIC\n"
        "    {   /* canary: the main thread's static TLS pointer (TEB slot 0 +0x2C) */",
        "fixed TLS canary start")
    s = replace_once(s,
        '        last = v;\n    }\n}',
        '        last = v;\n    }\n#endif\n}', "fixed TLS canary end")
    s = replace_once(s,
        '        extern void touch_handle_event(const SDL_Event *e);\n        touch_handle_event(&e);',
        '        /* UIKit owns touch input; do not also feed the legacy pad. */', "native touch ownership")
    s = replace_once(s,
        '    if (!g_nscript || g_padonly) { if (n) *n = nk; return ks; }',
        '    /* A real held keyboard state, used by both DirectInput paths.\n'
        '     * Synthetic SDL events alone do not change SDL_GetKeyboardState. */\n'
        '    static _Thread_local Uint8 mobile_keys[SDL_NUM_SCANCODES];\n'
        '    memset(mobile_keys, 0, sizeof mobile_keys);\n'
        '    memcpy(mobile_keys, ks, nk < SDL_NUM_SCANCODES ? nk : SDL_NUM_SCANCODES);\n'
        '    if (skyrim_mobile_f5_down()) mobile_keys[SDL_SCANCODE_F5] = 1;\n'
        '    ks = mobile_keys; nk = SDL_NUM_SCANCODES;\n'
        '    if (!g_nscript || g_padonly) { if (n) *n = nk; return ks; }', "quicksave keyboard state")
    return s


def patch_bridge(s: str) -> str:
    s = mobile_conditions(s)
    s = replace_once(s, '#include <SDL2/SDL_vulkan.h>',
        '#include <SDL2/SDL_vulkan.h>\n#include "ios_runtime.h"\n#include "benchmark.h"', "bridge include")
    backbuffer = "    if (SUCCEEDED(hr)) { g_bb_w = pp.BackBufferWidth; g_bb_h = pp.BackBufferHeight; }"
    if s.count(backbuffer) != 2:
        raise ValueError("bridge: expected device create and reset backbuffer sizes")
    s = s.replace(backbuffer, backbuffer + "\n    if (SUCCEEDED(hr)) skyrim_benchmark_render_size(g_bb_w, g_bb_h);")
    drawable = '    SDL_Vulkan_GetDrawableSize((SDL_Window *)platform_window(), &ww, &wh);'
    if s.count(drawable) != 2:
        raise ValueError("bridge: expected two drawable size queries")
    s = s.replace(drawable, '    skyrim_ios_drawable_size(&ww, &wh);')
    dest = ' : (vr.left || vr.top) ? &vr : nullptr;'
    if s.count(dest) != 2:
        raise ValueError("bridge: expected two physical destination selections")
    # Zero-offset is still a physical-pixel destination on a Retina drawable.
    # Passing null asks DXVK to substitute the smaller logical window size.
    s = s.replace(dest, ' : &vr;')
    for fragment, label in (
        ('    if (SUCCEEDED(hr)) { extern void hud_screen_device(IDirect3DDevice9 *); hud_screen_device(dev); }', "HUD device setup"),
        ('    { extern void hud_screen_reset(void); hud_screen_reset(); }', "HUD reset"),
        ('    { extern void hud_screen_present(IDirect3DDevice9 *); hud_screen_present(d); }', "HUD present"),
    ):
        s = replace_once(s, fragment, "#ifndef SKYRIM_IOS_GUEST_DIAGNOSTIC\n" + fragment + "\n#endif", label)
    # The optional desktop bisect capture invokes ImageMagick through system(),
    # which the iPhoneOS SDK forbids. Keep unsupported capture explicit; normal
    # rendering never requests LB_BISECT and does not take this branch.
    s = replace_once(s,
        '        char cmd[256];\n'
        '        snprintf(cmd, sizeof cmd, "import -window root %s/skip_%03d.png",\n'
        '                 getenv("LB_BISECT_DIR") ? getenv("LB_BISECT_DIR") : ".", k / 2);\n'
        '        if (system(cmd)) {}',
        '        rt_fatal("LB_BISECT desktop screen capture is unavailable on iOS");',
        "unsupported desktop screenshot command")
    marker = "    platform_present_hook();"
    if s.count(marker) != 2:
        raise ValueError("bridge: expected device and swapchain present hooks")
    s = s.replace(marker, "    skyrim_ios_present_result((int32_t)hr);\n" + marker)
    s = replace_once(s, '    touch_draw(d);',
        '    /* Controls are native UIKit views; no D3D state capture or overlay draw. */\n'
        '    skyrim_ios_present_phase(1);', "native overlay replaces D3D overlay")
    s = s.replace('    RECT vr = view_rect();',
        '    skyrim_ios_present_phase(2);\n    RECT vr = view_rect();')
    for call in ('    HRESULT hr = d->Present(', '    HRESULT hr = s->Present('):
        s = replace_once(s, call, '    skyrim_ios_pace_frame();\n    skyrim_ios_present_phase(3);\n' + call, "driver present phase")
    s = s.replace('    skyrim_ios_present_result((int32_t)hr);',
        '    skyrim_ios_present_phase(4);\n    skyrim_ios_present_result((int32_t)hr);')
    s = s.replace(marker, marker + '\n    skyrim_ios_present_phase(5);')
    # The key is already ordered by (object, subresource). Preserve upload
    # order, lock scope and callbacks while visiting only this object's range.
    s = replace_once(s,
        '        for (auto &kv : g_persist)\n'
        '            if (kv.first.first == obj && !g_shadows.count(kv.first)) todo.push_back({MEM + kv.second.guest, kv.second.upload});',
        '        for (auto it = g_persist.lower_bound({obj, 0});\n'
        '             it != g_persist.end() && it->first.first == obj; ++it)\n'
        '            if (!g_shadows.count(it->first)) todo.push_back({MEM + it->second.guest, it->second.upload});',
        "persistent upload object range")
    s = replace_once(s,
        '    for (auto it = g_persist.begin(); it != g_persist.end();) {\n'
        '        if (it->first.first == obj) { gfree(it->second.guest); it = g_persist.erase(it); }\n'
        '        else ++it;\n'
        '    }',
        '    for (auto it = g_persist.lower_bound({obj, 0});\n'
        '         it != g_persist.end() && it->first.first == obj;) {\n'
        '        gfree(it->second.guest);\n'
        '        it = g_persist.erase(it);\n'
        '    }', "persistent release object range")
    return patch_shadow_queue(s)


def patch_shadow_queue(s: str) -> str:
    s = '#include <new>\n' + s
    s = replace_once(s,
        '    std::vector<std::pair<const uint8_t *, std::function<void(const uint8_t *)>>> todo;',
        '''    using Upload = std::pair<const uint8_t *, std::function<void(const uint8_t *)>>;
    struct Pending {
        alignas(Upload) unsigned char storage[8 * sizeof(Upload)];
        size_t used = 0;
        std::vector<Upload> overflow;
        Upload *at(size_t n) { return std::launder(reinterpret_cast<Upload *>(storage + n * sizeof(Upload))); }
        ~Pending() { for (size_t n = 0; n < used; ++n) at(n)->~Upload(); }
        void append(const uint8_t *bytes, const std::function<void(const uint8_t *)> &upload) {
            if (used < 8) { new (storage + used * sizeof(Upload)) Upload(bytes, upload); ++used; }
            else overflow.emplace_back(bytes, upload);
        }
    } todo;''', 'small pending uploads without vector growth')
    s = replace_once(s,
        'todo.push_back({MEM + it->second.guest, it->second.upload});',
        'todo.append(MEM + it->second.guest, it->second.upload);', 'retain callbacks before unlock')
    return replace_once(s, '    for (auto &t : todo) t.second(t.first);',
        '    for (size_t n = 0; n < todo.used; ++n) todo.at(n)->second(todo.at(n)->first);\n'
        '    for (auto &t : todo.overflow) t.second(t.first);', 'ordered callbacks outside lock')


def patch_main_pump(s: str) -> str:
    s = replace_once(s, '#include "rt.h"',
        '#include "rt.h"\n#include "ios_runtime.h"', "main pump include")
    if 'SHIM(Sleep) {' in s:
        return replace_once(s, 'SHIM(Sleep) {',
            'SHIM(Sleep) {\n    skyrim_ios_pump_events();', "UIKit servicing during guest sleep")
    return replace_once(s, 'SHIM(PeekMessageA) {',
        'SHIM(PeekMessageA) {\n    skyrim_ios_pump_events();', "UIKit servicing in guest message loop")


def patch_asset_reads(s: str) -> str:
    # Observe the existing syscall only. APC dispatch, file positions, guest
    # buffers and error mapping remain unchanged; this does not make I/O async.
    s = '#include "performance_metrics.h"\n' + s
    old = ('    ssize_t r;\n'
           '    if (ov) r = pread(f->fd, MEM + buf, n, (off_t)M32(ov + 8) | ((off_t)M32(ov + 12) << 32));\n'
           '    else if (f->shared) { r = pread(f->fd, MEM + buf, n, f->pos); if (r > 0) f->pos += r; }\n'
           '    else r = read(f->fd, MEM + buf, n);')
    s = replace_once(s, old,
        '    const uint64_t ios_started = skyrim_perf_now_ns();\n' + old + '\n'
        '    skyrim_perf_end(SKYRIM_PERF_READ_FILE, ios_started, r < 0);', 'synchronous read timing')
    old = '    ssize_t r = write ? pwrite(f->fd, MEM + buf, n, off) : pread(f->fd, MEM + buf, n, off);'
    return replace_once(s, old,
        '    const uint64_t ios_started = write ? 0 : skyrim_perf_now_ns();\n' + old + '\n'
        '    if (!write) skyrim_perf_end(SKYRIM_PERF_READ_FILE_EX, ios_started, r < 0);',
        'read-ex syscall timing')


def patch_shader_creation(s: str) -> str:
    s = '#include "performance_metrics.h"\n' + s
    for method in ('CreateVertexShader', 'CreatePixelShader'):
        old = f'    auto r = bridge_call(self, &IDirect3DDevice9::{method}, GPTR(ARG(1)), &out1);'
        s = replace_once(s, old,
            '    const uint64_t ios_started = skyrim_perf_now_ns();\n' + old + '\n'
            '    skyrim_perf_end(SKYRIM_PERF_D3D_SHADER_CREATE, ios_started, (int32_t)r < 0);',
            f'{method} host timing')
    return s


def patch_texture_loading(s: str) -> str:
    s = '#include "performance_metrics.h"\n' + s
    old = ('  HRESULT hr = load_texture((IDirect3DDevice9 *)com_host(ARG(0)), GPTR(ARG(1)),\n'
           '                            ARG(2), type, &out, 0);')
    s = replace_once(s, old,
        '  const uint64_t ios_started = skyrim_perf_now_ns();\n' + old + '\n'
        '  skyrim_perf_end(SKYRIM_PERF_TEXTURE_LOAD, ios_started, FAILED(hr));',
        'memory texture host timing')
    # The file variant includes its own host read before decode/upload. It is
    # not an extra guest ReadFile call and must not be summed as exclusive time.
    s = replace_once(s, '  auto bytes = read_file(ARG(1));',
        '  const uint64_t ios_started = skyrim_perf_now_ns();\n'
        '  auto bytes = read_file(ARG(1));', 'file texture timing start')
    old = '                   ARG(5), (D3DFORMAT)ARG(6), (D3DPOOL)ARG(7), ARG(8), ARG(10));'
    return replace_once(s, old, old + '\n'
        '  skyrim_perf_end(SKYRIM_PERF_TEXTURE_LOAD, ios_started, FAILED(hr));',
        'file texture timing end')


def patch_dds_layout(s: str) -> str:
    start = s.index('uint8_t *native_d3dx_decode_rgba(')
    return '#include "ios_dds_layout.h"\n' + s[:start] + (Path(__file__).parent / 'dds_level.c.in').read_text(encoding='utf-8') + '\n' + s[start:]


def patch_native_textures(s: str) -> str:
    s = '#include "ios_dds_layout.h"\n' + s
    s = replace_once(s, 'native_d3dx_dds_level(&i, bytes, face, l,',
                     'skyrim_ios_dds_cube_level(&i, bytes, face, l,', 'cube-only repeated layout reduction')
    return patch_texture_loading(s)


def patch_mutex_policy(s: str) -> str:
    """Darwin's default pthread mutex hands the lock to the next waiter in order (fairshare), so a contended
    guest critical section or wait-object lock makes each release wait for a context switch. First-fit lets
    a running thread take it again, which is what Windows critical sections and these short wait-object sections
    expect. Only the lock policy changes: recursion, ownership, wait order of the guest-visible events and every
    protected structure are untouched. The macros exist on Apple platforms; elsewhere (PC tests) nothing changes."""
    s = replace_once(s,
        'static pthread_mutex_t g_wlock = PTHREAD_MUTEX_INITIALIZER;',
        '#ifdef PTHREAD_FIRSTFIT_MUTEX_INITIALIZER\n'
        'static pthread_mutex_t g_wlock = PTHREAD_FIRSTFIT_MUTEX_INITIALIZER;\n'
        '#else\n'
        'static pthread_mutex_t g_wlock = PTHREAD_MUTEX_INITIALIZER;\n'
        '#endif', "first-fit wait-object lock")
    return replace_once(s,
        '                pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);\n',
        '                pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);\n'
        '#ifdef PTHREAD_MUTEX_POLICY_FIRSTFIT_NP\n'
        '                pthread_mutexattr_setpolicy_np(&a, PTHREAD_MUTEX_POLICY_FIRSTFIT_NP);\n'
        '#endif\n', "first-fit guest critical sections")


def patch_kernel(s: str) -> str:
    s = patch_file_writes(patch_asset_reads(patch_apc_queue(patch_directory_index(
        patch_waiter_membership(patch_mutex_policy(s))))))
    return patch_main_pump(patch_ece_read_witness(s))


def patch_ece_read_witness(s: str) -> str:
    old = '    skyrim_perf_end(SKYRIM_PERF_READ_FILE, ios_started, r < 0);'
    s = ('#ifdef SKYRIM_IOS_MOD_HOOKS\n#include "mod_ece_tri_host.h"\n'
         '#include "mod_highheel_pex_host.h"\n#endif\n' + s)
    return replace_once(s, old, old + '''
#ifdef SKYRIM_IOS_MOD_HOOKS
    if (skyrim_ece_tri_read_pending)
        skyrim_ece_tri_app_read(c, M32(c->r[4]), ARG(0), f->fd, r);
    if (skyrim_highheel_pex_read_pending)
        skyrim_highheel_pex_read(c, M32(c->r[4]), ARG(0), f->fd, r);
#endif''', 'read-only FaceGen TRI input witness')


def patch_file_writes(s: str) -> str:
    # Observe only; preserve the original write/close syscalls and all error,
    # guest-output, APC, and file-position behavior. No implicit fsync or retry.
    s = '#include "save_write_probe.h"\n' + s
    s = replace_once(s,
        'typedef struct { int fd; char path[1024]; int shared; off_t pos; } GFile;',
        'typedef struct { int fd; char path[1024]; int shared; off_t pos; SkyrimSaveWriteProbe ios_write; } GFile;',
        'host-only save candidate observation')
    old = '    snprintf(f->path, sizeof f->path, "%s", host);'
    s = replace_once(s, old, old + '\n    skyrim_save_write_init(&f->ios_write, f->path, sizeof f->path);',
        'classify filename once without retaining another path')
    old = '        r = write(f->fd, MEM + buf, n);'
    s = replace_once(s, old,
        '        const unsigned ios_save_candidate = f->ios_write.candidate;\n'
        '        const uint64_t ios_write_started = skyrim_save_write_begin(&f->ios_write);\n' + old + '\n'
        '        skyrim_save_write_end(ios_save_candidate, ios_write_started, r < 0, 0);',
        'synchronous file write timing excludes stdout and stderr')
    old = '    ssize_t r = write ? pwrite(f->fd, MEM + buf, n, off) : pread(f->fd, MEM + buf, n, off);'
    s = replace_once(s, old,
        '    const unsigned ios_save_candidate = write ? f->ios_write.candidate : 0;\n'
        '    const uint64_t ios_write_started = write ? skyrim_save_write_begin(&f->ios_write) : 0;\n' + old + '\n'
        '    if (write) skyrim_save_write_end(ios_save_candidate, ios_write_started, r < 0, 1);',
        'write-ex syscall observation before unchanged APC handling')
    old = '        if (!f->shared) close(f->fd);  /* shared BSA fds stay open */'
    return replace_once(s, old,
        '        const uint64_t ios_close_started = f->shared ? 0 : skyrim_save_write_close_begin(&f->ios_write);\n'
        + old + '\n        skyrim_save_write_close(&f->ios_write, ios_close_started);',
        'candidate handle timing around the original close call')


def patch_waiter_membership(s: str) -> str:
    # Private host objects only. All accesses stay inside the original g_wlock.
    # Saturation is sticky: an impossible-to-count object always takes the old
    # scan, rather than risking a false zero and a missed notification.
    s = replace_once(s, '    int refs;\n} Waitable;',
        '    int refs;\n    uint32_t waiting_entries; /* protected by g_wlock */\n} Waitable;',
        'wait set membership count on host waitable')
    s = replace_once(s, 'static void wake_waiters(Waitable *w) {',
        'static void wake_waiters(Waitable *w) {\n    if (!w->waiting_entries) return;',
        'skip unrelated waiter scan only for a proven empty membership')
    s = replace_once(s, '        g_waiters = me;\n        int timed_out = 0;',
        '        g_waiters = me;\n'
        '        for (uint32_t k = 0; k < n; ++k)\n'
        '            if (w[k]->waiting_entries != UINT32_MAX) ++w[k]->waiting_entries;\n'
        '        int timed_out = 0;', 'register before atomically releasing the wait mutex')
    return replace_once(s, '        if (me->prev) me->prev->next = me->next; else g_waiters = me->next;',
        '        for (uint32_t k = 0; k < n; ++k)\n'
        '            if (w[k]->waiting_entries != UINT32_MAX) --w[k]->waiting_entries;\n'
        '        if (me->prev) me->prev->next = me->next; else g_waiters = me->next;',
        'unregister after every condition return while holding the same mutex')


def patch_apc_queue(s: str) -> str:
    s = replace_once(s, 'static __thread int t_napc;',
        'static __thread int t_napc;\nstatic __thread unsigned t_apc_head;',
        'per-thread completion queue head')
    s = replace_once(s, '    t_apc[t_napc++] = (Apc){fn, err, (uint32_t)r, ov};',
        '    t_apc[(t_apc_head + (unsigned)t_napc) & 63u] = (Apc){fn, err, (uint32_t)r, ov};\n'
        '    ++t_napc;', 'append completion without shifting pending entries')
    return replace_once(s, '''        Apc a = t_apc[0];
        memmove(t_apc, t_apc + 1, --t_napc * sizeof(Apc));''', '''        Apc a = t_apc[t_apc_head];
        t_apc_head = (t_apc_head + 1u) & 63u;
        --t_napc;''', 'remove before callback, preserving recursive APC delivery')


def patch_directory_index(s: str) -> str:
    old = 'typedef struct { char dir[1024]; time_t mtime; long mtime_ns; char *names; size_t len; } DirCache;'
    new = '''typedef struct { uint32_t offset, length; } DirNameIndex;
typedef struct { char dir[1024]; time_t mtime; long mtime_ns; char *names; size_t len;
                 DirNameIndex *index; size_t index_count; } DirCache;'''
    s = replace_once(s, old, new + '\n' +
        (Path(__file__).parent / 'directory_index.c.in').read_text(encoding='utf-8'), 'length-only directory index')
    s = replace_once(s, '        free(dc->names);',
        '        free(dc->index); dc->index = NULL; dc->index_count = 0;\n        free(dc->names);',
        'retire directory index with its listing')
    s = replace_once(s, '        if (!dc->names) dc->names = calloc(1, 1);',
        '        if (!dc->names) dc->names = calloc(1, 1);\n        dir_index_build(dc);', 'index complete immutable listing')
    return replace_once(s, '''    for (size_t o = 0; o < dc->len; o += strlen(dc->names + o) + 1)
        if (!strcasecmp(dc->names + o, comp)) {
            snprintf(comp, compsz, "%s", dc->names + o);
            hit = 1;
            break;
        }''', '''    const char *found = dir_index_find(dc, comp);
    if (found) {
        snprintf(comp, compsz, "%s", found);
        hit = 1;
    }''', 'lookup only compatible name lengths')


def patch_dispatch(s: str) -> str:
    # Every import updates its thread's diagnostic slot. Separate neighboring
    # writers without altering recording frequency, fields or dispatch itself.
    s = patch_watchdog_layout(s)
    s = replace_once(s, '#include "rt.h"',
        '#include "rt.h"\n#include "ios_runtime.h"', "watchdog include")
    return replace_once(s,
        '            LOG("watchdog %ds: frames %d%s", tick * 5, f, f == lastf ? " (no new frames)" : "");',
        '            LOG("watchdog %ds: frames %d%s", tick * 5, f, f == lastf ? " (no new frames)" : "");\n'
        '            skyrim_ios_watchdog();', "present-stage watchdog")


def patch_watchdog_layout(s: str) -> str:
    old = 'static struct { const char *name; uint32_t ret; volatile uint32_t calls; uint32_t tid, gtid, esi, ebx, esp; pthread_t th; } g_wd[WD_MAX];'
    new = 'static struct __attribute__((aligned(128))) { const char *name; uint32_t ret; volatile uint32_t calls; uint32_t tid, gtid, esi, ebx, esp; pthread_t th; } g_wd[WD_MAX];'
    return replace_once(s, old, new, "separate frequently written watchdog slots")


def patch_constant_scratch(s: str) -> str:
    # One matrix (four float4 registers) fits on the calling thread's stack.
    # Build only the typed payload actually consumed by set_registers.
    s = replace_once(s, '''  std::vector<float> values(v->count * 4, 0);
  std::vector<int> ints(v->count * 4, 0);''', '''  const unsigned lanes = v->count * 4;
  float local_values[16];
  int local_ints[16];
  const bool float_payload = v->set == 2;
  std::vector<float> large_values(lanes > 16 && float_payload ? lanes : 0, 0);
  std::vector<int> large_ints(lanes > 16 && !float_payload ? lanes : 0, 0);
  float *values = lanes > 16 ? large_values.data() : local_values;
  int *ints = lanes > 16 ? large_ints.data() : local_ints;
  if (lanes <= 16) {
    if (float_payload) std::fill_n(values, lanes, 0.0f);
    else std::fill_n(ints, lanes, 0);
  }''', 'small constant scratch without heap allocation')
    s = replace_once(s, '        if (index >= values.size())',
                     '        if (index >= lanes)', 'same constant scratch bounds')
    s = replace_once(s, '''        values[index] = (float)value;
        ints[index] = v->set == 0 ? value != 0 : (int)value;''',
        '''        if (float_payload) values[index] = (float)value;
        else ints[index] = v->set == 0 ? value != 0 : (int)value;''',
        'omit the unconsumed constant representation')
    return replace_once(s,
        '      v->set == 2 ? (const void *)values.data() : (const void *)ints.data(),',
        '      v->set == 2 ? (const void *)values : (const void *)ints,',
        'same synchronous typed constant payload')


def patch_shader(s: str) -> str:
    s = replace_once(s, '      for (unsigned i = 0; i < constants; i++) {',
        '      t.constants.reserve(constants);\n      for (unsigned i = 0; i < constants; i++) {',
        'bounded CTAB capacity known before insertion')
    s = patch_constant_scratch(s)
    s = '#include "performance_metrics.h"\n' + s
    s = replace_once(s, '  int status = compile_hlsl(&info, &code, &messages);',
        '  const uint64_t ios_started = skyrim_perf_now_ns();\n'
        '  int status = compile_hlsl(&info, &code, &messages);\n'
        '  skyrim_perf_end(SKYRIM_PERF_HLSL, ios_started, status < 0);', 'HLSL host timing')
    start = s.index("static void init_compiler() {")
    end = s.index("\nstruct Constant {", start)
    return s[:start] + '''static void init_compiler() {
  compile_hlsl = &vkd3d_shader_compile;
  free_code = &vkd3d_shader_free_shader_code;
  free_messages = &vkd3d_shader_free_messages;
  LOG("D3DX: statically linked iOS vkd3d-shader compiler");
}
''' + s[end:]


def patch_shims(s: str) -> str:
    return replace_once(s,
        "#ifdef GAME_SKYRIM\n    {\n        extern void engine_primitives_init(void);",
        "#if defined(GAME_SKYRIM) && !defined(SKYRIM_IOS_GUEST_DIAGNOSTIC)\n"
        "    {\n        extern void engine_primitives_init(void);", "native game replacements")


def patch_prof(s: str) -> str:
    # All three engine reports refer to fixed-address replacement objects. None
    # are compiled into the original-body diagnostic runtime.
    return s.replace("#ifdef GAME_SKYRIM", "#if defined(GAME_SKYRIM) && !defined(SKYRIM_IOS_GUEST_DIAGNOSTIC)")


def patch_input(s: str) -> str:
    s = mobile_conditions(s)
    s = replace_once(s, '#include "rt.h"', '#include "rt.h"\n#include "mobile_core.h"\n#include "benchmark.h"', "native pad include")
    s = replace_once(s,
        '        b |= g_virt.buttons | skip_tap();\n'
        '        if (g_virt.lt) lt = g_virt.lt;\n'
        '        if (g_virt.rt) rt = g_virt.rt;\n'
        '        if (g_virt.lx || g_virt.ly) { lx = g_virt.lx; ly = g_virt.ly; }\n'
        '        if (g_virt.rx || g_virt.ry) { rx = g_virt.rx; ry = g_virt.ry; }',
        '        SkyrimMobilePad native;\n'
        '        if (skyrim_benchmark_active() && skyrim_benchmark_pad(skyrim_mobile_now(), &native)) {\n'
        '            b = native.buttons; lt = native.lt; rt = native.rt;\n'
        '            lx = native.lx; ly = native.ly; rx = native.rx; ry = native.ry;\n'
        '        } else {\n'
        '            native = skyrim_mobile_pad();\n'
        '            b |= native.buttons | skip_tap();\n'
        '            if (native.lt) lt = native.lt;\n'
        '            if (native.rt) rt = native.rt;\n'
        '            if (native.lx || native.ly) { lx = native.lx; ly = native.ly; }\n'
        '            if (native.rx || native.ry) { rx = native.rx; ry = native.ry; }\n'
        '        }',
        "thread-safe native input snapshot")
    return s


def patch_preferences(s: str) -> str:
    s = mobile_conditions(s)
    s = replace_once(s, '#include "rt.h"', '#include "rt.h"\n#include "mobile_ui.h"', "mobile preferences include")
    s = replace_once(s, '    if (existing) return;',
        '    if (existing && !skyrim_mobile_graphics_changed()) return;', "retain existing preferences")
    s = replace_once(s, '    char v[16];\n    snprintf(v, sizeof v, "%d", w);',
        '    if (!existing) {\n    char v[16];\n    snprintf(v, sizeof v, "%d", w);', "only seed absent size")
    s = replace_once(s, '    ini_set(f, "Display", "bFull Screen", "1");',
        '    ini_set(f, "Display", "bFull Screen", "1");\n    }', "preserve existing size")
    s = replace_once(s,
        '    ini_save(f);\n    pthread_mutex_unlock(&g_ini_lock);\n    LOG("seeded %s (low preset, %dx%d)", host, w, h);',
        '    const char *section, *key, *value;\n'
        '    for (unsigned i = 0; skyrim_mobile_ini(i, &section, &key, &value); ++i)\n'
        '        ini_set(f, section, key, value);\n'
        '    ini_save(f);\n    pthread_mutex_unlock(&g_ini_lock);\n'
        '    LOG("configured app profile %s (%dx%d)", host, w, h);', "explicit selected graphics overrides")
    return s


def patch_audio_submix(s: str) -> str:
    # The entire span stays under the existing mixer mutex. route() only writes
    # accumulation buffers; no callbacks or voice graph mutations occur here.
    old = '''    /* submixes in processing-stage order, then the master */
    for (int stage = 0; stage < 64; stage++)
        for (int i = 0; i < MAX_VOICES; i++) {
            Voice *v = g_voices[i];
            if (!v || v->dead || v->kind != V_SUBMIX || v->stage != stage) continue;
            route(v, v->acc, frames);
        }
'''
    new = '''    /* Stable stage buckets: same stage order and original slot order,
     * without scanning all voice slots 64 times under the mixer mutex.
     * Rebuild every pass, after callbacks, so no cached graph can go stale. */
    int stage_head[64], stage_tail[64], stage_next[MAX_VOICES];
    for (int stage = 0; stage < 64; stage++) stage_head[stage] = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (!v || v->dead || v->kind != V_SUBMIX || (unsigned)v->stage >= 64) continue;
        int stage = v->stage;
        stage_next[i] = -1;
        if (stage_head[stage] < 0) stage_head[stage] = i;
        else stage_next[stage_tail[stage]] = i;
        stage_tail[stage] = i;
    }
    for (int stage = 0; stage < 64; stage++)
        for (int i = stage_head[stage]; i >= 0; i = stage_next[i]) {
            Voice *v = g_voices[i];
            route(v, v->acc, frames);
        }
'''
    return replace_once(s, old, new, "stable audio submix stage traversal")


def patch_xwma_voice_profile(s: str) -> str:
    # This omitted-options profile is used by the owned Japanese LE voices.
    # Do not guess for other formats or override explicit WAVEFORMATEX options.
    s = replace_once(s, 'static int open_codec(XWmaDecoder *d) {',
        '''static int ios_xwma_mono_32k_20k(const XWmaDecoder *d) {
    return d->tag == 0x161 && !d->extra_size && d->channels == 1 &&
           d->rate == 32000 && d->average_bytes == 2500 &&
           d->block_align == 1280 && d->bits == 16;
}

static int ios_xwma_recreate_on_reset(const XWmaDecoder *d) {
    /* Both verified omitted-options profiles use WMAv2 noise coding. The
     * stereo profile also occurs in the existing English voice archives. */
    return ios_xwma_mono_32k_20k(d) ||
           (d->tag == 0x161 && !d->extra_size && d->channels == 2 &&
            d->rate == 44100 && d->average_bytes == 4000 &&
            d->block_align == 1485 && d->bits == 16);
}

static int open_codec(XWmaDecoder *d) {''', "xWMA exact omitted-options voice profile")
    s = replace_once(s, 'else if (id == AV_CODEC_ID_WMAV2) c->extradata[4] = 31;',
        '''else if (id == AV_CODEC_ID_WMAV2) {
        /* WMAv2 flags2 bits 3..4 select the variable block-length range.
         * 0x17 gives this low-bitrate profile two-bit block indices; the
         * generic 0x1f assumption reads three bits and rejects valid audio.
         * Other xWMA profiles retain the pinned upstream interpretation. */
        c->extradata[4] = ios_xwma_mono_32k_20k(d) ? 0x17 : 0x1f;
    }''', "xWMA 32k mono block-size configuration")
    return replace_once(s, '    avcodec_flush_buffers(d->codec);',
        '''    if (ios_xwma_recreate_on_reset(d)) {
        /* FFmpeg 7.1.1 WMAv2 flush leaves noise_index and overlap state.
         * Start a reused low-bitrate voice like a fresh decoder. Packet
         * streaming does not reset here; ordinary English mono voices keep the
         * existing flush path. Never return a partially opened codec. */
        avcodec_free_context(&d->codec);
        if (open_codec(d) < 0) avcodec_free_context(&d->codec);
    } else avcodec_flush_buffers(d->codec);''', "xWMA low-bitrate stream reset")


def patch_xwma_buffer(s: str) -> str:
    # Decode, resampling, packet/PTS order and caller ownership stay unchanged.
    # Capacity belongs only to this synchronous decode call, not to a voice.
    s = replace_once(s, '#include <limits.h>', '#include <limits.h>\n#include <errno.h>',
                     "xWMA optional allocation errno preservation")
    s = replace_once(s,
        'static int receive_pcm(XWmaDecoder *d, int16_t **pcm, uint32_t *frames) {',
        '''static int receive_pcm(XWmaDecoder *d, int16_t **pcm, uint32_t *frames,
                       size_t *capacity_bytes, size_t *extent_bytes, size_t spare_limit) {''',
        "xWMA local capacity arguments")
    s = replace_once(s,
        '''        int16_t *next = realloc(*pcm, samples * sizeof(int16_t));
        if (!next && samples) return -1;
        *pcm = next;
        uint8_t *out = (uint8_t *)(next + (size_t)*frames * d->channels);''',
        '''        size_t needed = samples * sizeof(int16_t);
        if (needed > *capacity_bytes) {
            /* Amortize frame appends only when more input packets remain,
             * with at most 64 KiB of speculative capacity. Never let this
             * turn an exact-fit success into a memory-pressure failure. */
            size_t spare = *capacity_bytes < spare_limit ? *capacity_bytes : spare_limit;
            size_t reserved = needed <= SIZE_MAX - spare ? needed + spare : needed;
            const int allocation_errno = errno;
            int16_t *next = realloc(*pcm, reserved);
            if (!next && reserved != needed) {
                reserved = needed;
                errno = allocation_errno;
                next = realloc(*pcm, needed);
            }
            if (!next) return -1;
            *pcm = next;
            *capacity_bytes = reserved;
        }
        *extent_bytes = needed;
        uint8_t *out = (uint8_t *)(*pcm + (size_t)*frames * d->channels);''',
        "xWMA bounded amortized allocation")
    s = replace_once(s, '    int rc = 0;\n    for (size_t offset',
        '    int rc = 0;\n    size_t capacity_bytes = 0, extent_bytes = 0;\n    for (size_t offset',
        "xWMA per-call capacity")
    s = replace_once(s,
        'if (rc < 0 || (rc = receive_pcm(d, pcm, frames)) < 0) break;',
        'if (rc < 0 || (rc = receive_pcm(d, pcm, frames, &capacity_bytes, &extent_bytes,\n'
        '                                        size < bytes - offset ? 65536 : 0)) < 0) break;',
        "xWMA reserve only for remaining input packets")
    s = replace_once(s, 'if (rc >= 0) rc = receive_pcm(d, pcm, frames);',
        'if (rc >= 0) rc = receive_pcm(d, pcm, frames, &capacity_bytes, &extent_bytes, 0);',
        "xWMA flush without speculative allocation")
    return replace_once(s,
        '    if (rc < 0) { free(*pcm); *pcm = NULL; *frames = 0; xwma_reset(d); }\n    return rc;',
        '''    if (rc < 0) { free(*pcm); *pcm = NULL; *frames = 0; xwma_reset(d); }
    else if (extent_bytes && capacity_bytes > extent_bytes) {
        /* Restore the final extent requested by the original decoder before
         * the caller queues PCM. A failed shrink leaves valid owned samples. */
        const int allocation_errno = errno;
        int16_t *trimmed = realloc(*pcm, extent_bytes);
        errno = allocation_errno;
        if (trimmed) *pcm = trimmed;
    }
    return rc;''', "xWMA return exact final extent")


def patch_audio_backpressure(s: str) -> str:
    helper = (Path(__file__).parent / 'audio_backpressure.c.in').read_text(encoding='utf-8')
    s = replace_once(s, 'static void *engine_main(void *arg) {',
                     helper + '\nstatic void *engine_main(void *arg) {', 'bounded audio queue polling helper')
    s = replace_once(s, '                usleep(1000);',
                     '                usleep(ios_audio_backpressure_delay(q, frames, ios_audio_device_rate));',
                     'only full queue polling uses the adaptive delay')
    return replace_once(s, '        SDL_PauseAudioDevice(g_dev, 0);\n    }\n    pthread_t t;',
                        '        ios_audio_device_samples = have.samples;\n        ios_audio_device_rate = have.freq;\n        SDL_PauseAudioDevice(g_dev, 0);\n    }\n    pthread_t t;',
                        'publish negotiated buffer quantum before creating mixer thread')


def patch_audio(s: str) -> str:
    s = '#include "performance_metrics.h"\n' + patch_audio_backpressure(patch_audio_route(patch_audio_submix(s)))
    helper = '''static int ios_decode_xwma(XWmaDecoder *decoder, const uint8_t *data, size_t bytes,
                           unsigned packets, int end, int16_t **pcm, uint32_t *frames) {
    const uint64_t started = skyrim_perf_now_ns();
    int result = xwma_decode(decoder, data, bytes, packets, end, pcm, frames);
    skyrim_perf_end(SKYRIM_PERF_XWMA_DECODE, started, result < 0);
    return result;
}
'''
    s = replace_once(s, 'SHIM(xs_SubmitSourceBuffer) {', helper + 'SHIM(xs_SubmitSourceBuffer) {',
                     "xWMA timing helper")
    return replace_once(s, 'if (xwma_decode(v->wma, MEM + x->data, x->bytes, packets,',
                        'if (ios_decode_xwma(v->wma, MEM + x->data, x->bytes, packets,',
                        "observe synchronous xWMA decode inside existing audio lock")


def patch_audio_route(s: str) -> str:
    # Preserve every multiply/add, including the initial +0 and zero weights.
    # No sparse mixing: zero times NaN/Inf must still participate.
    marker = '        for (int f = 0; f < frames; f++)\n            for (int dc = 0; dc < d->in_ch; dc++) {'
    return replace_once(s, marker, '''        if (v->in_ch == 2 && d->in_ch == 2) {
            for (int f = 0; f < frames; f++) {
                float left = 0, right = 0;
                left += m[0] * in[f * 2];
                left += m[1] * in[f * 2 + 1];
                d->acc[f * 2] += left;
                right += m[2] * in[f * 2];
                right += m[3] * in[f * 2 + 1];
                d->acc[f * 2 + 1] += right;
            }
        } else
''' + marker, 'stereo route with fixed strides and original arithmetic')


def patch_surface_point(s: str) -> str:
    return replace_once(s, '''    } else
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {''', '''    } else if ((filter & 7) <= 2 && w == sw && h == sh) {
      // Identity point sampling: every source coordinate is exactly (x,y).
      // Keep the completed snapshot, color key and format packing unchanged.
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          pack((uint8_t *)lock.pBits + (size_t)y * lock.Pitch + x * dp,
               desc.Format, &rgba[((size_t)y * sw + x) * 4]);
    } else if ((filter & 7) <= 2 && w <= 16384 && h <= 16384) {
      // Exact floor(x * sw / w), advanced without a division per pixel.
      // The existing input/output bounds keep all products and sums in range.
      const int whole = sw / w, part = sw % w;
      for (int y = 0; y < h; y++) {
        const size_t row = (size_t)(y * sh / h) * sw;
        int sx = 0, remainder = 0;
        for (int x = 0; x < w; x++) {
          pack((uint8_t *)lock.pBits + (size_t)y * lock.Pitch + x * dp,
               desc.Format, &rgba[(row + sx) * 4]);
          sx += whole;
          remainder += part;
          if (remainder >= w) { remainder -= w; ++sx; }
        }
      }
    } else
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {''', 'identity point sampling without per-pixel division')


def patch_bc_palette(s: str) -> str:
    marker = '// Decodes one 4x4 block into BGRA rows of `out` (pitch in bytes), clipped to w x h.'
    helper = (Path(__file__).parent / 'bc1_palette.cpp.in').read_text(encoding='utf-8')
    s = replace_once(s, marker, helper + '\n' + marker, 'packed BC1 palette')
    return replace_once(s,
        'void decode_block(D3DFORMAT f, const uint8_t *b, uint8_t *out, int pitch, int w, int h) {',
        'void decode_block(D3DFORMAT f, const uint8_t *b, uint8_t *out, int pitch, int w, int h) {\n'
        '    if (f == D3DFMT_DXT1) { ios_decode_bc1(b, out, pitch, w, h); return; }',
        'BC1 needs no independent alpha plane')


PATCHERS = {
    "main.c": patch_main,
    "platform_sdl.c": patch_platform,
    "d3d9_bridge.cpp": patch_bridge,
    "d3dx_shader.cpp": patch_shader,
    "d3d9_bridge_gen.cpp": patch_shader_creation,
    "d3dx_native.cpp": patch_native_textures,
    "shims.c": patch_shims,
    "prof.c": patch_prof,
    "skyrim_shims.c": patch_preferences,
    "touch.cpp": mobile_conditions,
    "input.c": patch_input,
    "kernel32.c": patch_kernel,
    "user32.c": patch_main_pump,
    "dispatch.c": patch_dispatch,
    "xaudio2.c": patch_audio,
    "xwma.c": lambda s: patch_xwma_buffer(patch_xwma_voice_profile(s)),
    "d3dx_image.c": patch_dds_layout,
    "d3dx_surface.cpp": patch_surface_point,
    "d3d9_bcemu.cpp": patch_bc_palette,
}


def prepare(source: Path) -> dict[str, str]:
    runtime = source / "runtime"
    staged: dict[str, str] = {}
    # Deliberately scoped: no gen/, Data/, private/, local/ or .git reads.
    for path in sorted(runtime.iterdir()):
        if path.is_file() and path.suffix in {".c", ".cpp", ".h", ".in"}:
            if path.is_symlink():
                raise ValueError(f"symlink not accepted in public runtime: {path.name}")
            staged[path.name] = path.read_text(encoding="utf-8")
    for name, expected in EXPECTED.items():
        original = staged[name]
        actual = hashlib.sha256(original.encode("utf-8")).hexdigest()
        if actual != expected:
            raise ValueError(f"{name}: differs from pinned public runtime {UPSTREAM_REVISION}; refusing patch")
        staged[name] = PATCHERS[name](original)
    # Admission is outside frontend D3D/D3DX calls. Downstream submit and
    # producer workers retain their original execution paths.
    sys.path.insert(0, str(Path(__file__).parent))
    from lifecycle_adapters import adapt as adapt_lifecycle
    for name in ("d3d9_bridge.cpp", "d3d9_bridge_gen.cpp", "d3d9_bcemu.cpp", "d3dx_native.cpp", "xaudio2.c"):
        staged[name] = adapt_lifecycle(name, staged[name])
    from runtime_lock_timing import adapt as adapt_lock_timing
    for name in ("d3d9_bridge.cpp", "xaudio2.c"):
        staged[name] = adapt_lock_timing(name, staged[name])
    return staged


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="pinned public skydream checkout")
    parser.add_argument("--output", type=Path, help="public runtime staging directory; never vendor")
    parser.add_argument("--check", action="store_true", help="validate all patch anchors without writing")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    staged = prepare(source)
    if not args.check:
        if not args.output:
            parser.error("--output is required unless --check is used")
        output = args.output.resolve()
        if output == source or source in output.parents or output in source.parents:
            raise ValueError("output must be separate from the vendor source checkout")
        output.mkdir(parents=True, exist_ok=True)
        for name, text in staged.items():
            target = output / name
            if target.is_symlink():
                raise ValueError(f"refusing to overwrite symlink: {target}")
            target.write_text(text, encoding="utf-8", newline="\n")
    print(f"Validated {len(EXPECTED)} iOS patches against {UPSTREAM_REVISION}; "
          + ("no files written." if args.check else f"staged {len(staged)} public runtime sources."))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError) as error:
        print(f"runtime staging failed: {error}", file=sys.stderr)
        raise SystemExit(1)
