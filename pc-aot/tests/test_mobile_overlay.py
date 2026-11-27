"""Exercise actual upstream staging and preserve unrelated profile/CPU behavior.

UIKit compilation and real game input are deliberately not inferred here.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("mobile_stage", ROOT / "ios/runtime-port/prepare_runtime.py")
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class NativeOverlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = stage.prepare(ROOT / "vendor/skydream")

    def test_actual_input_replaces_only_legacy_virtual_merge(self):
        source = self.sources["input.c"]
        override, normal = source.split("if (skyrim_benchmark_active() &&", 1)[1].split("} else {", 1)
        self.assertIn("skyrim_benchmark_pad(skyrim_mobile_now(), &native)", override)
        self.assertIn("b = native.buttons; lt = native.lt; rt = native.rt;", override)
        self.assertIn("lx = native.lx; ly = native.ly; rx = native.rx; ry = native.ry;", override)
        self.assertNotIn("skip_tap()", override)
        self.assertIn("native = skyrim_mobile_pad();", normal)
        self.assertIn("b |= native.buttons | skip_tap();", normal)
        self.assertNotIn("b |= g_virt.buttons | skip_tap();", source)
        for operation in ("SDL_GameControllerGetAxis", "SDL_GameControllerGetButton", "M16(g + 12)", "M16(g + 14)"):
            self.assertIn(operation, source)
        self.assertNotIn("touch_handle_event(&e);", self.sources["platform_sdl.c"])
        self.assertIn("mobile_keys[SDL_SCANCODE_F5] = 1", self.sources["platform_sdl.c"])

    def test_both_present_routes_preserve_physical_destination_and_result(self):
        source = self.sources["d3d9_bridge.cpp"]
        self.assertEqual(source.count("skyrim_ios_pace_frame();"), 2)
        self.assertEqual(source.count("skyrim_ios_present_result((int32_t)hr);"), 2)
        self.assertNotIn("touch_draw(d);", source)
        self.assertNotIn("(vr.left || vr.top) ? &vr : nullptr", source)
        self.assertIn("skyrim_mobile_viewport(ww, wh, width, height)", source)
        for call in ("d->Present(", "s->Present("):
            self.assertIn(call, source)

    def test_only_selected_profile_preferences_can_override_existing_data(self):
        source = self.sources["skyrim_shims.c"]
        self.assertIn("if (existing && !skyrim_mobile_graphics_changed()) {\n        log_water_prefs(f);\n        pthread_mutex_unlock", source)
        self.assertIn("if (!existing) {\n    char v[16];", source)
        self.assertIn("skyrim_mobile_ini(i, &section, &key, &value)", source)
        upstream = (ROOT / "vendor/skydream/runtime/skyrim_shims.c").read_text(encoding="utf-8")
        start = "static const char *ini_find("
        self.assertEqual(source.split("/* The chosen dimensions")[0].replace('#include "mobile_ui.h"\n', ''),
                         stage.mobile_conditions(upstream.split(start)[0]))
        self.assertIn("if (render_display_value(f, app, key)) {\n        pthread_mutex_unlock", source)
        self.assertIn("skyrim_mobile_render_size(&w, &h);\n        skyrim_seed_prefs(w, h);", self.sources["main.c"])

    def test_selected_size_is_advertised_as_an_exact_guest_mode(self):
        cxx = shutil.which(os.environ.get("CXX", "c++"))
        if not cxx:
            self.skipTest("A C++ compiler is required")
        source = self.sources["d3d9_bridge.cpp"]
        modes = source[source.index("static uint32_t mode_count()"):
                       source.index("// The back buffer size")]
        self.assertIn("bridge_scaled_mode(i, &w, &h);", source)
        self.assertIn("bridge_scaled_mode(0, &native_w, &native_h);", source)
        for operation in range(2):
            for begin in range(2):
                self.assertIn(f"IOS_RENDER_DEVICE schema=1 op={operation} begin={begin}", source)
        fixture = r'''
#include <cassert>
#include <cstdint>
static const uint32_t k_mode_heights[] = {720, 540, 360};
static bool configured;
static int skyrim_mobile_render_override(int *w, int *h) {
    if (!configured) return 0;
    *w = 1564;
    *h = 720;
    return 1;
}
static uint32_t mode_count(int *ww, int *wh, int64_t *an, int64_t *ad, uint32_t *top) {
    *ww = 956; *wh = 440; *an = *ww; *ad = *wh; *top = *wh;
    return 2;
}
''' + modes + r'''
int main() {
    uint32_t w = 0, h = 0;
    assert(mode_count() == 2);
    bridge_scaled_mode(0, &w, &h);
    assert(w == 956 && h == 440);
    bridge_scaled_mode(1, &w, &h);
    assert(w == 782 && h == 360);
    configured = true;
    assert(mode_count() == 1);
    bridge_scaled_mode(0, &w, &h);
    assert(w == 1564 && h == 720);
}
'''
        with tempfile.TemporaryDirectory(prefix="mobile-render-modes-") as directory:
            test = Path(directory) / "modes.cpp"
            binary = Path(directory) / "modes"
            test.write_text(fixture, encoding="utf-8")
            subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=15)

    def test_create_and_reset_report_allocated_dimensions_and_release_surface(self):
        cxx = shutil.which(os.environ.get("CXX", "c++"))
        if not cxx:
            self.skipTest("A C++ compiler is required")
        source = self.sources["d3d9_bridge.cpp"]
        self.assertIn("if (SUCCEEDED(hr)) observe_render_surface(dev, pp);", source)
        self.assertIn("if (SUCCEEDED(hr)) observe_render_surface(d, pp);", source)
        observe = source[source.index("static void observe_render_surface("):source.index("static RECT view_rect()")]
        fixture = r'''
#include <cassert>
#include <cstdio>
using HRESULT = int;
#define SUCCEEDED(result) ((result) >= 0)
enum { D3DBACKBUFFER_TYPE_MONO };
struct D3DPRESENT_PARAMETERS { unsigned BackBufferWidth, BackBufferHeight; };
struct D3DSURFACE_DESC { unsigned Width, Height; };
struct IDirect3DSurface9 {
    bool canDescribe = true;
    int references = 0;
    int GetDesc(D3DSURFACE_DESC* description) {
        if (!canDescribe) return -2;
        *description = {1564, 720};
        return 0;
    }
    void Release() { --references; }
};
struct IDirect3DDevice9 {
    IDirect3DSurface9 primary;
    bool canAcquire = true;
    int GetBackBuffer(int swapChain, int index, int type, IDirect3DSurface9** surface) {
        assert(swapChain == 0 && index == 0 && type == D3DBACKBUFFER_TYPE_MONO);
        if (!canAcquire) return -1;
        *surface = &primary;
        ++primary.references;
        return 0;
    }
};
static unsigned reportedWidth, reportedHeight;
static void skyrim_benchmark_render_size(unsigned w, unsigned h) { reportedWidth = w; reportedHeight = h; }
''' + observe + r'''
int main() {
    IDirect3DDevice9 device;
    D3DPRESENT_PARAMETERS parameters{956, 440};
    observe_render_surface(&device, parameters);
    assert(reportedWidth == 1564 && reportedHeight == 720);
    assert(device.primary.references == 0);
    device.primary.canDescribe = false;
    observe_render_surface(&device, parameters);
    assert(reportedWidth == 956 && reportedHeight == 440);
    assert(device.primary.references == 0);
    device.canAcquire = false;
    observe_render_surface(&device, parameters);
    assert(reportedWidth == 956 && reportedHeight == 440);
    assert(device.primary.references == 0);
}
'''
        with tempfile.TemporaryDirectory(prefix="mobile-render-surface-") as directory:
            test = Path(directory) / "surface.cpp"
            binary = Path(directory) / "surface"
            test.write_text(fixture, encoding="utf-8")
            subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            run = subprocess.run([str(binary)], check=True, timeout=15, text=True, capture_output=True)
            self.assertEqual(run.stderr.splitlines(), [
                "IOS_RENDER_SURFACE schema=1 measured=1 width=1564 height=720 result=00000000",
                "IOS_RENDER_SURFACE schema=1 measured=0 width=956 height=440 result=fffffffe",
                "IOS_RENDER_SURFACE schema=1 measured=0 width=956 height=440 result=ffffffff",
            ])

    def test_cpu_layout_headers_stay_byte_equal_after_newline_normalization(self):
        for name in ("x86.h", "rt_table.h"):
            original = (ROOT / "vendor/skydream/runtime" / name).read_text(encoding="utf-8")
            self.assertEqual(self.sources[name], original)

    def test_changed_anchor_refuses_instead_of_silently_omitting_input(self):
        original = (ROOT / "vendor/skydream/runtime/input.c").read_text(encoding="utf-8")
        with self.assertRaises(ValueError):
            stage.patch_input(original.replace("b |= g_virt.buttons | skip_tap();", "b |= skip_tap();"))


if __name__ == "__main__":
    unittest.main()
