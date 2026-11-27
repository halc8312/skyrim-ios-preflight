"""Exercise actual upstream staging and preserve unrelated profile/CPU behavior.

UIKit compilation and real game input are deliberately not inferred here.
"""
import importlib.util
from pathlib import Path
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
        for call in ("d->Present(", "s->Present("):
            self.assertIn(call, source)

    def test_only_selected_profile_preferences_can_override_existing_data(self):
        source = self.sources["skyrim_shims.c"]
        self.assertIn("if (existing && !trial && !skyrim_mobile_graphics_changed()) return;", source)
        self.assertIn("if (!existing) {\n    char v[16];", source)
        self.assertIn("skyrim_mobile_ini(i, &section, &key, &value)", source)
        upstream = (ROOT / "vendor/skydream/runtime/skyrim_shims.c").read_text(encoding="utf-8")
        start = "static const char *ini_find("
        self.assertEqual(source.split("/* Trial values")[0].replace('#include "mobile_ui.h"\n', ''),
                         stage.mobile_conditions(upstream.split(start)[0]))
        self.assertIn("if (trial_display_value(f, app, key)) {\n        pthread_mutex_unlock", source)
        self.assertIn("skyrim_mobile_render_size(&w, &h);\n        skyrim_seed_prefs(w, h);", self.sources["main.c"])

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
