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

    def test_pinned_importer_requests_the_selected_localized_files(self):
        source = Path(os.environ.get("SKYDREAM_SOURCE_DIR", ROOT / "vendor/skydream")) / "runtime/skse_translation.c"
        if not source.exists():
            self.skipTest("pinned public Skydream checkout required")
        original = source.read_text(encoding="utf-8")
        with self.assertRaises(ValueError):
            adapt(original + "\n")
        staged = adapt(original)
        self.assertIn('#ifdef SKYDREAM_SKSE\n#include "classic_skse_translation_list.h"', staged)
        self.assertNotIn('/Documents/Skyrim/Plugins.txt', staged)
        # Exercise the real public import loop. Only the game-memory accessors
        # and final resource open are substituted by this host test.
        importer = staged[staged.index("void skse_import_translations(CPU *c) {"):staged.rindex("#endif")]
        exe = self.compile_driver('''
#include <stdint.h>
#include <strings.h>
#include <pthread.h>
typedef int CPU;
static const char *g_save_dir;
static uint32_t translated_loader, references = 2;
static pthread_mutex_t translation_lock = PTHREAD_MUTEX_INITIALIZER;
#define M32(x) ((void)(x), 1u)
#define GPTR(x) ((void)(x), &references)
#define GSTR(x) ((void)(x), "JAPANESE")
static uint32_t skse_virtual(CPU *c, uint32_t obj, int slot, int count, const uint32_t *args) {
    (void)c; (void)obj; (void)slot; (void)count; (void)args; return 1;
}
#define skse_ini_setting(c, name) ((void)(c), (void)(name), 1u)
static void import_file(CPU *c, uint32_t translator, const char *path) {
    (void)c; (void)translator; puts(path);
}
''' + importer + '''
int main(int argc, char **argv) { if (argc != 2) return 1; g_save_dir = argv[1]; skse_import_translations(NULL); }
''')
        profile = self.root / "selected"
        target = profile / "documents/SkYRim/PLUGINS.TXT"
        target.parent.mkdir(parents=True)
        target.write_text("*雪.ESP\r\n# ignored.esp\r\n../Other.esm\r\nLore.esm\r\n", encoding="utf-8-sig")
        result = subprocess.run([str(exe), str(profile)], check=True, capture_output=True, timeout=3)
        self.assertEqual(result.stdout.decode("utf-8").splitlines(),
                         [r"Interface\Translations\雪_JAPANESE.txt", r"Interface\Translations\Lore_JAPANESE.txt"])


if __name__ == "__main__":
    unittest.main()
