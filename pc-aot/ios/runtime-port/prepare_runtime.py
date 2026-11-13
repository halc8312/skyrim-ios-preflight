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
    "shims.c": "7478c4f7ee7b9e75e3ec17acba5825b5f594a7966cfa747155252b935131c6f0",
    "prof.c": "2c0910c1620e844d47e2e1d2d1910091245f062144ce3253159e9f4e10f12c0c",
    "skyrim_shims.c": "3b1955df82fcd791e907ced07dfd8365d89f417f705d9f51e39f4fb04d1bd9c7",
    "touch.cpp": "ada0d43ab81ba5a37580004ec8e59835b4aa617baf6a9cae98276374b96fe027",
    "input.c": "d0c549822ae87ab22009b60854ca84fc15c69151e8fd5063bd9fe56c1b38f193",
    "kernel32.c": "c6360da8a234421feac620c5db529d7a488ff6fc16747a0ad275f6dc987a0dfc",
    "user32.c": "e4970ef14067b9779415a6872895b57f007c904f69682a1e395b1d96b61ef335",
    "dispatch.c": "17c9508a10b2174a7efc24b26f94de0f6c9ce58eee9fa966fe74b68140ba7ac0",
    "xaudio2.c": "91f0e31e583b8e03d1ecf35632fba2cd24abb42f986c3c796aa879d711bdfc42",
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
        '#include <SDL2/SDL_vulkan.h>\n#include "ios_runtime.h"', "bridge include")
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
    return s


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


def patch_kernel(s: str) -> str:
    return patch_main_pump(patch_asset_reads(s))


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


def patch_shader(s: str) -> str:
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
    s = replace_once(s, '#include "rt.h"', '#include "rt.h"\n#include "mobile_core.h"', "native pad include")
    s = replace_once(s,
        '        b |= g_virt.buttons | skip_tap();\n'
        '        if (g_virt.lt) lt = g_virt.lt;\n'
        '        if (g_virt.rt) rt = g_virt.rt;\n'
        '        if (g_virt.lx || g_virt.ly) { lx = g_virt.lx; ly = g_virt.ly; }\n'
        '        if (g_virt.rx || g_virt.ry) { rx = g_virt.rx; ry = g_virt.ry; }',
        '        SkyrimMobilePad native = skyrim_mobile_pad();\n'
        '        b |= native.buttons | skip_tap();\n'
        '        if (native.lt) lt = native.lt;\n'
        '        if (native.rt) rt = native.rt;\n'
        '        if (native.lx || native.ly) { lx = native.lx; ly = native.ly; }\n'
        '        if (native.rx || native.ry) { rx = native.rx; ry = native.ry; }',
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


PATCHERS = {
    "main.c": patch_main,
    "platform_sdl.c": patch_platform,
    "d3d9_bridge.cpp": patch_bridge,
    "d3dx_shader.cpp": patch_shader,
    "d3d9_bridge_gen.cpp": patch_shader_creation,
    "d3dx_native.cpp": patch_texture_loading,
    "shims.c": patch_shims,
    "prof.c": patch_prof,
    "skyrim_shims.c": patch_preferences,
    "touch.cpp": mobile_conditions,
    "input.c": patch_input,
    "kernel32.c": patch_kernel,
    "user32.c": patch_main_pump,
    "dispatch.c": patch_dispatch,
    "xaudio2.c": patch_audio_submix,
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
