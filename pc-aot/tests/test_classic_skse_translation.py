"""Read-only classic translation lookup using synthetic profiles and public source."""
import codecs
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
sys.path.insert(0, str(PORT))
from classic_skse_translation import adapt


class ClassicTranslationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="classic-translation-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def compile_driver(self, code):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required; run on WSL/Mac")
        source = self.root / "fixture.c"
        source.write_text('#include "classic_skse_translation_list.h"\n' + code, encoding="utf-8")
        exe = self.root / "inspect"
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, "-I", str(PORT), str(source), str(PORT / "mod_plugins.c"),
                        "-o", str(exe)], check=True, timeout=60)
        return exe

    def test_selected_profile_casing_bom_and_missing_list(self):
        exe = self.compile_driver('''
int main(int argc, char **argv) {
    FILE *f = skyrim_skse_open_plugin_list(argc > 1 ? argv[1] : NULL);
    if (!f) return 3;
    int c;
    while ((c = fgetc(f)) != EOF) putchar(c);
    return fclose(f);
}
''')
        def read(profile):
            result = subprocess.run([str(exe), str(profile)], cwd=self.root,
                                    capture_output=True, timeout=3)
            return result.returncode, result.stdout

        default_list = self.root / "Documents/Skyrim/plugins.txt"
        default_list.parent.mkdir(parents=True)
        default_list.write_bytes(b"Default.esp\r\n")
        profile = self.root / "profiles/日本語"
        target = profile / "dOcUmEnTs/SKYRIM/pLuGiNs.TxT"
        target.parent.mkdir(parents=True)
        self.assertEqual(read(profile), (3, b""))
        self.assertEqual(list(target.parent.iterdir()), [])
        self.assertEqual(read(self.root), (0, default_list.read_bytes()))

        body = "*魔法.ESP\r\n# Comment\r\nAnother.esm\r\n".encode("utf-8")
        for contents in [b"", b"ab", body, codecs.BOM_UTF8 + body]:
            with self.subTest(contents=contents):
                target.write_bytes(contents)
                self.assertEqual(read(profile), (0, contents.removeprefix(codecs.BOM_UTF8)))
                self.assertEqual(target.read_bytes(), contents)
        target.unlink()
        target.symlink_to(default_list)
        self.assertEqual(read(profile), (3, b""))
        target.unlink()
        os.mkfifo(target)
        self.assertEqual(read(profile), (3, b""))
        target.unlink()
        target.write_bytes(b"x" * (1024 * 1024 + 1))
        self.assertEqual(read(profile), (3, b""))
        target.write_bytes(b"First.esp\n")
        alternative = target.with_name("plugins.txt")
        if not alternative.exists():  # A case-sensitive test volume can contain aliases.
            alternative.write_bytes(b"Second.esp\n")
            self.assertEqual(read(profile), (3, b""))

    def test_importer_uses_ready_inputs_once_in_plugin_order(self):
        vendor = Path(os.environ.get("SKYDREAM_SOURCE_DIR", ROOT / "vendor/skydream"))
        source = vendor / "runtime/skse_translation.c"
        if not source.exists():
            self.skipTest("pinned public Skydream checkout required")
        original = source.read_text(encoding="utf-8")
        with self.assertRaises(ValueError):
            adapt(original + "\n")
        staged = adapt(original)
        self.assertNotIn("/Documents/Skyrim/Plugins.txt", staged)
        start = staged.index('#include "classic_skse_translation_import.h"')
        importer = staged[start:staged.rindex("#endif")]
        fixture = (ROOT / "tests/native/classic_translation_import_fixture.c").read_text(encoding="utf-8")
        exe = self.compile_driver(fixture + "\n" + importer + r'''
int main(int argc, char **argv) {
    assert(argc == 3);
    const char *mode = argv[1], *profile = argv[2];
    g_save_dir = profile;
    expected_language = "JAPANESE";
    guest_language = "ENGLISH";  /* The guest's early default. */
    trial = 1;
    resource_ready = 1;
    if (!strcmp(mode, "auto")) { expected_language = NULL; guest_language = NULL; }
    if (!strcmp(mode, "english")) expected_language = "ENGLISH";
    if (!strcmp(mode, "late-list")) g_save_dir = NULL;
    if (!strcmp(mode, "late-bsa") || !strcmp(mode, "missing-bsa")) resource_ready = 0;
    if (!strcmp(mode, "no-trial")) trial = 0;
    skse_import_translations(NULL);
    unsigned first = calls, first_requests = skyui_requests;
    for (int i = 0; i < 100; ++i) skse_import_translations(NULL);
    assert(calls == first && skyui_requests == first_requests && !reserved);
    if (!strcmp(mode, "auto")) guest_language = "JAPANESE";
    if (!strcmp(mode, "late-list")) g_save_dir = profile;
    if (!strcmp(mode, "late-bsa")) resource_ready = 1;
    if (!strcmp(mode, "missing-bsa")) {
        const unsigned deadlines[] = {1, 3, 7, 15, 31, 63};
        unsigned due = 0;
        for (ticks = 1; ticks <= 100; ++ticks) {
            if (due < sizeof deadlines / sizeof deadlines[0] && ticks == deadlines[due]) ++due;
            skse_import_translations(NULL);
            assert(skyui_requests == 1 + due && !reserved && references == 2);
        }
    }
    unsigned attempts_before_stop = 0;
    for (int i = 0; i < 20; ++i) {
        ticks += 65;
        skse_import_translations(NULL);
        if (i == 9) attempts_before_stop = skyui_requests;
        assert(references == 2 && !reserved);
    }
    assert(attempts_before_stop == skyui_requests);
    fprintf(stderr, "%u %u %u %u\n", first, calls, inserted, skyui_requests);
    if (!strcmp(mode, "fresh-loader")) {
        mock_loader = 2;
        skse_import_translations(NULL);
        assert(calls == first * 2 && skyui_requests == 2 && references == 2 && !reserved);
    }
    return 0;
}
''')
        profile = self.root / "selected"
        target = profile / "documents/SkYRim/PLUGINS.TXT"
        target.parent.mkdir(parents=True)
        target.write_text("*雪.ESP\r\n# ignored.esp\r\nNone.esm\r\n../Other.esm\r\n"
                          "SkyUI.esp\r\nLore.esm\r\n", encoding="utf-8-sig")
        for mode, expected in {
            "ready": (4, 4, 3, 1), "english": (4, 4, 3, 1), "auto": (0, 4, 3, 1),
            "late-list": (0, 4, 3, 1), "late-bsa": (0, 4, 3, 2),
            "missing-bsa": (0, 3, 2, 7), "no-trial": (4, 4, 3, 0),
            "fresh-loader": (4, 4, 3, 1),
        }.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(exe), mode, str(profile)], check=True,
                                        capture_output=True, timeout=3)
                self.assertEqual(tuple(map(int, result.stderr.split())), expected)
                language = "ENGLISH" if mode == "english" else "JAPANESE"
                names = ["雪", "Lore"] if mode == "missing-bsa" else ["雪", "SkyUI", "Lore"]
                output = [f"Interface\\Translations\\{name}_{language}.txt" for name in names]
                self.assertEqual(result.stdout.decode("utf-8").splitlines(),
                                 output * (2 if mode == "fresh-loader" else 1))


if __name__ == "__main__":
    unittest.main()
