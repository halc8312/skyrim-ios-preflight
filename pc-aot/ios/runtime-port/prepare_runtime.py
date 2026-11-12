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
    "shims.c": "7478c4f7ee7b9e75e3ec17acba5825b5f594a7966cfa747155252b935131c6f0",
    "prof.c": "2c0910c1620e844d47e2e1d2d1910091245f062144ce3253159e9f4e10f12c0c",
    "skyrim_shims.c": "3b1955df82fcd791e907ced07dfd8365d89f417f705d9f51e39f4fb04d1bd9c7",
    "touch.cpp": "ada0d43ab81ba5a37580004ec8e59835b4aa617baf6a9cae98276374b96fe027",
    "input.c": "d0c549822ae87ab22009b60854ca84fc15c69151e8fd5063bd9fe56c1b38f193",
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
        '#include <SDL2/SDL.h>\n#include "ios_runtime.h"\n#include "PcGuestABI.h"', "main includes")
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
    return s


def patch_platform(s: str) -> str:
    s = mobile_conditions(s)
    s = replace_once(s, '#include <SDL2/SDL.h>',
        '#include <SDL2/SDL.h>\n#include <SDL2/SDL_vulkan.h>\n#include "ios_runtime.h"', "platform includes")
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
    return s


def patch_bridge(s: str) -> str:
    s = mobile_conditions(s)
    s = replace_once(s, '#include <SDL2/SDL_vulkan.h>',
        '#include <SDL2/SDL_vulkan.h>\n#include "ios_runtime.h"', "bridge include")
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
    return s


def patch_shader(s: str) -> str:
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


PATCHERS = {
    "main.c": patch_main,
    "platform_sdl.c": patch_platform,
    "d3d9_bridge.cpp": patch_bridge,
    "d3dx_shader.cpp": patch_shader,
    "shims.c": patch_shims,
    "prof.c": patch_prof,
    "skyrim_shims.c": mobile_conditions,
    "touch.cpp": mobile_conditions,
    "input.c": mobile_conditions,
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
