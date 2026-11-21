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
from stage_save_notifications import HEADER, PRELUDE, SAVE_SITES, stage_save_notifications


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
                    path.write_bytes(data)
                    subprocess.run([cc, "-std=gnu11", "-DGAME_SKYRIM", "-fsyntax-only",
                                    "-Wno-parentheses-equality",
                                    "-I", str(original / "source" / "runtime"), "-I", str(original / "gen"),
                                    "-I", str(RUNTIME_SAVE), str(path)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
