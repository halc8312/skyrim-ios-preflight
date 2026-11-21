"""Check the authored notification insertions against the private owned lift."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_SAVE = ROOT / "experiments" / "classic-runtime-save"
sys.path.insert(0, str(RUNTIME_SAVE))
sys.path.insert(0, str(ROOT / "scripts"))
from stage_save_notifications import (HEADER, PRELUDE, SAVE_SITES, function_span,
                                      stage_save_and_load_notifications, stage_save_notifications)
import mod_hooks_guest


class SaveNotificationStageTests(unittest.TestCase):
    def test_rejects_unreviewed_units(self):
        with tempfile.TemporaryDirectory(prefix="save-notification-authorized-") as path:
            path = Path(path)
            (path / "gen").mkdir()
            first = next(iter(SAVE_SITES))
            (path / "gen" / first).write_bytes(PRELUDE + b"/* unreviewed test source */\n")
            with self.assertRaisesRegex(ValueError, "Generated unit differs"):
                stage_save_notifications(path)

    def test_private_pinned_stage_preserves_engine_code(self):
        owned = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not owned:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private generation")
        cc = shutil.which(os.environ.get("CC", "clang"))
        if not cc:
            self.skipTest("C compiler required")
        original = Path(owned).resolve()
        self.assertTrue(original.is_dir())
        staged = stage_save_notifications(original)
        self.assertEqual(set(staged), set(SAVE_SITES))
        live = mod_hooks_guest.parse_groups("fuz_menu,fuz_world,vm_lifecycle,chargen_precache,camera_observe,chargen_slider,"
                                          "loading_menu_observe,inventory_fit,safety_load")
        self.assertFalse(staged.keys() & mod_hooks_guest.compose(original, live).keys())
        combined = stage_save_and_load_notifications(original)
        self.assertEqual(staged.keys(), combined.keys())
        load_name = "f_6821C0"
        load_unit = "exe_103.c"
        before, after = staged[load_unit], combined[load_unit]
        lo, hi = function_span(before, load_name)
        new_lo, new_hi = function_span(after, load_name)
        self.assertEqual(after[:new_lo], b'#include "fresh_load.h"\n' + before[:lo])
        self.assertEqual(after[new_hi:], before[hi:])
        self.assertIn(b"classic_fresh_slot_enter", after[new_lo:new_hi])
        self.assertIn(b"classic_fresh_cached", after[new_lo:new_hi])
        self.assertEqual({k: v for k, v in combined.items() if k != load_unit},
                         {k: v for k, v in staged.items() if k != load_unit})
        private = ROOT / "private"
        if private.resolve() != private:
            raise ValueError("Generated sources must stay in private/")
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="save-notifications-", dir=private) as folder:
            for name, data in staged.items():
                with self.subTest(unit=name):
                    self.assertTrue(data.startswith(PRELUDE + HEADER))
                    rebuilt = PRELUDE + data[len(PRELUDE + HEADER):]
                    for before, after, count in reversed(SAVE_SITES[name][1]):
                        self.assertEqual(rebuilt.count(after), count)
                        rebuilt = rebuilt.replace(after, before)
                    self.assertEqual(rebuilt, (original / "gen" / name).read_bytes())
                    path = Path(folder) / name
                    path.write_bytes(combined[name])
                    subprocess.run([cc, "-std=gnu11", "-DGAME_SKYRIM", "-fsyntax-only",
                                    "-Wno-parentheses-equality",
                                    "-I", str(original / "source" / "runtime"), "-I", str(original / "gen"),
                                    "-I", str(RUNTIME_SAVE), "-I", str(RUNTIME_SAVE.parent / "classic-fresh-load"),
                                    str(path)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
