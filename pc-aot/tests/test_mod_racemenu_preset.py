"""Parse authored RaceMenu .jslot text for the stock face sliders."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
OK, TOO_LARGE, MALFORMED, NO_FACE, BAD_MORPHS, BAD_PRESETS = range(6)
CUSTOM, OVERRIDES, SKIN, BODY, TINT, PARTS, SCULPT = (1, 2, 4, 8, 16, 32, 64)


def preset(**changes):
    morphs = [0.0] * 19
    morphs[1] = 0.25
    morphs[18] = -1.5
    document = {"actor": {"weight": 50.0}, "headParts": [], "tintInfo": [],
                "morphs": {"default": {"presets": [1, 2, 3, 4], "morphs": morphs},
                           "custom": [], "sculptDivisor": 1000.0, "sculpt": []},
                "version": 1, "mods": []}
    document.update(changes)
    return document


@unittest.skipUnless(os.name == "posix", "Requires POSIX paths")
class PresetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / "preset"
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, ROOT / "tests/native/test_mod_racemenu_preset.c",
                        PORT / "mod_racemenu_preset.c", "-lm", "-o", cls.binary], check=True, timeout=60)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.file = Path(tmp.name) / "p.jslot"

    def parse(self, data):
        self.file.write_bytes(data if isinstance(data, bytes) else data.encode())
        return subprocess.check_output([self.binary, self.file], text=True, timeout=10).split()

    def status(self, data):
        return int(self.parse(data)[0])

    def test_stock_sliders_presets_and_unapplied_parts(self):
        self.assertEqual(self.parse(json.dumps(preset())), ["0", "0", "1", "2", "3", "4", "0.25", "-1.5"])
        full = preset(headParts=[{"x": 1}], tintInfo=[1], overrides=[1], skinOverrides=[1], bodyMorphs=[1])
        full["morphs"]["custom"] = [{"a": 1}]
        full["morphs"]["sculpt"] = [1]
        self.assertEqual(self.parse(json.dumps(full))[1], str(CUSTOM | OVERRIDES | SKIN | BODY | TINT | PARTS | SCULPT))

    def test_bom_whitespace_numbers_and_escapes(self):
        text = "﻿ \n" + json.dumps(preset(note="あ \"q\" \\ /", other=[1e2, -0.0, 3E-2]), indent=2) + "\n "
        self.assertEqual(self.status(text), OK)

    def test_missing_or_wrong_face_morphs(self):
        for document in ({}, {"morphs": {}}, {"morphs": {"default": {}}}, {"morphs": []}):
            self.assertEqual(self.status(json.dumps(document)), NO_FACE, document)
        for values in ([0.0] * 18, [0.0] * 20, "x", {"a": 1}, [None] * 19, ["1"] * 19, [True] * 19):
            document = preset()
            document["morphs"]["default"]["morphs"] = values
            self.assertEqual(self.status(json.dumps(document)), BAD_MORPHS, values)

    def test_non_finite_and_huge_numbers_are_refused(self):
        base = json.dumps(preset())
        for bad in ("NaN", "Infinity", "-Infinity", "1e999", "2e6"):
            self.assertIn(self.status(base.replace("0.25", bad, 1)), (MALFORMED, BAD_MORPHS), bad)

    def test_bad_presets_and_duplicate_keys(self):
        for presets in ([1, 2, 3], [1, 2, 3, -1], [1, 2, 3, 1.5], [1, 2, 3, "x"], [1, 2, 3, 4294967296], 7):
            document = preset()
            document["morphs"]["default"]["presets"] = presets
            self.assertEqual(self.status(json.dumps(document)), BAD_PRESETS, presets)
        text = json.dumps(preset())
        self.assertEqual(self.status(text.replace('"morphs": {"default"', '"morphs": {"default": {}, "default"', 1)),
                         MALFORMED)
        self.assertEqual(self.status(text.replace('"presets"', '"morphs": [], "presets"', 1)), MALFORMED)

    def test_malformed_text(self):
        good = json.dumps(preset())
        for text in ("", "[]", "{", "{}}", good + "x", good[:-1], good.replace(":", "=", 1), "{'a': 1}",
                     '{"a": "\\x"}', '{"a": "\\u12G4"}', '{"a": "tab\there"}', '{"a": 01}', '{"a": 1.}', '{"a": +1}',
                     '{"a": tru}', '{"a": [1,]}', '{"a" 1}'):
            self.assertEqual(self.status(text), MALFORMED, text)

    def test_depth_nodes_and_size_limits(self):
        self.assertEqual(self.status("[" * 100 + "]" * 100), MALFORMED)
        nested = '{"a":' * 30 + "1" + "}" * 30
        self.assertEqual(self.status(nested), MALFORMED)
        many = json.dumps(preset(extra=[0] * 20000))
        self.assertEqual(self.status(many), TOO_LARGE)
        self.assertEqual(self.status(json.dumps(preset(pad="x" * (1024 * 1024)))), TOO_LARGE)


if __name__ == "__main__":
    unittest.main()
