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
from stage_native_load import (HEADER as NATIVE_HEADER, NATIVE_READ, NATIVE_SLOT, SLOT_UNIT, UNIT, VM_LOAD,
                               VM_REVERT, VM_UNIT, edit_function, stage_native_load, vm_call)
from test_fresh_load import SLOT_INVOKE
from test_slot_read import READ_CALL
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
        selected = mod_hooks_guest.parse_groups("save_lifecycle")
        all_stages = mod_hooks_guest.compose(original, live | selected)
        self.assertEqual({name: all_stages[name] for name in staged}, staged)
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

    def test_rejects_function_overlap(self):
        original = PRELUDE + b"\nvoid f_67B720(CPU *c) {\n    return;\n}\n"
        changed = original.replace(b"    return;", b"    other(c);\n    return;")
        with self.assertRaisesRegex(ValueError, "already changed f_67B720"):
            edit_function(changed, original, "f_67B720", lambda body: body)

    def test_native_stage_preserves_full_original_units(self):
        owned = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not owned:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private generation")
        root = Path(owned).resolve()
        native = stage_native_load(root)
        baseline = stage_save_and_load_notifications(root) | mod_hooks_guest.stage_vm_lifecycle(root)
        self.assertEqual(native.keys(), baseline.keys())
        edits = {
            UNIT: [(NATIVE_SLOT, SLOT_INVOKE)] + [
                (b"classic_native_load_" + hook + b"(", b"classic_fresh_" + hook + b"(")
                for hook in (b"enter", b"open_enter", b"open_leave")
            ],
            SLOT_UNIT: [(NATIVE_READ, READ_CALL), (vm_call(VM_REVERT), VM_REVERT)],
            VM_UNIT: [(vm_call(VM_LOAD), VM_LOAD)],
        }
        for name, actual in native.items():
            with self.subTest(unit=name):
                if name in edits:
                    self.assertTrue(actual.startswith(NATIVE_HEADER))
                    actual = actual.removeprefix(NATIVE_HEADER)
                for added, restored in edits.get(name, []):
                    self.assertEqual(actual.count(added), 1)
                    actual = actual.replace(added, restored, 1)
                self.assertEqual(actual, baseline[name])


if __name__ == "__main__":
    unittest.main()
