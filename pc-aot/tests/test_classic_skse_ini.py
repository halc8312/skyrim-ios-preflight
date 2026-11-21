"""Check Classic Game and Utility queries against the staged registration table."""
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
import prepare_classic_skse as classic


class ClassicIniTests(unittest.TestCase):
    def setUp(self):
        self.vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.stage = Path(tmp.name) / "runtime"
        self.stage.mkdir()
        self.files = (*classic.PINNED, "skse_translation.c", classic.PRESENT_FILE)
        for name in self.files:
            shutil.copyfile(self.vendor / "runtime" / name, self.stage / name)

    def test_ordinary_table_and_sources_are_preserved_and_staging_is_repeatable(self):
        before = [(self.vendor / "runtime" / name).read_bytes() for name in self.files]
        self.assertNotIn('F("Utility","GetINIBool"', (self.stage / "skse_compat_values.c").read_text())
        self.assertEqual(classic.prepare(self.vendor, self.stage), len(self.files))
        self.assertEqual(classic.prepare(self.vendor, self.stage), 0)
        self.assertEqual([(self.vendor / "runtime" / name).read_bytes() for name in self.files], before)
        registration = (self.stage / "skse_compat.c").read_text()
        self.assertLess(registration.index("probe_native(c,object,fn);"), registration.index("++passed;"))
        self.assertLess(registration.index("++passed;"), registration.index("IOS_CLASSIC_VM registered="))
        staged = (self.stage / "skse_system.c").read_text()
        self.assertIn("skse_classic_ini_dispatch(c, op, args, result);", staged)
        self.assertIn("if (skse_classic_mod_query(c, op, args, result)) return 1;", staged)
        self.assertNotIn("double number=name[0]", staged)

    def test_existing_classic_overlay_is_upgraded_without_replacing_local_edits(self):
        from classic_skse_translation import adapt
        for name in self.files:
            if name == classic.PRESENT_FILE:
                continue
            target = self.stage / name
            original = target.read_text()
            previous = adapt(original) if name == "skse_translation.c" else classic.adapt_ini(name, original)
            target.write_text(previous)
        self.assertEqual(classic.prepare(self.vendor, self.stage), len(classic.PINNED) + 1)
        self.assertEqual(classic.prepare(self.vendor, self.stage), 0)
        target = self.stage / "skse_compat.c"
        original = (self.vendor / "runtime" / target.name).read_text()
        target.write_text(classic.adapt_mod_queries(target.name, original))
        self.assertEqual(classic.prepare(self.vendor, self.stage), 1)
        self.assertEqual(classic.prepare(self.vendor, self.stage), 0)

    def test_rejects_vendor_target_and_staged_drift_before_publishing(self):
        with self.assertRaisesRegex(ValueError, "outside the vendor"):
            classic.prepare(self.vendor, self.vendor / "runtime")
        (self.stage / "skse_system.c").write_text("staged local edit")
        with self.assertRaisesRegex(ValueError, "unrecognized contents"):
            classic.prepare(self.vendor, self.stage)
        self.assertEqual((self.stage / "skse_compat.h").read_bytes(),
                         (self.vendor / "runtime/skse_compat.h").read_bytes())

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler")
    def test_present_pumps_input_only_when_classic_is_active(self):
        classic.prepare(self.vendor, self.stage)
        staged = (self.stage / classic.PRESENT_FILE).read_text()
        start = staged.index("void platform_present_hook(void) {")
        end = staged.index("    SDL_Event e;", start)
        fixture = self.stage.parent / "classic-present.c"
        fixture.write_text(
            "#include <assert.h>\nint pc_guest_classic_skse_active;\n"
            "static unsigned pumps;\nvoid skse_ui_pump(void) { ++pumps; }\n"
            + staged[start:end] + "}\n" + r"""
int main(void) {
    platform_present_hook();
    assert(pumps == 0);
    pc_guest_classic_skse_active = 1;
    platform_present_hook();
#ifdef SKYDREAM_SKSE
    assert(pumps == 1);
#else
    assert(pumps == 0);
#endif
    return 0;
}
""", encoding="utf-8")
        exe = fixture.with_suffix("")
        for defines in ([], ["-DSKYDREAM_SKSE=1"]):
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            *defines, str(fixture), "-o", str(exe)], check=True, timeout=60)
            subprocess.run([str(exe)], check=True, timeout=10)

    def run_fixture(self, name):
        classic.prepare(self.vendor, self.stage)
        executable = self.stage.parent / name
        sanitizers = []
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        *sanitizers, "-I", str(self.stage), "-I", str(PORT),
                        str(ROOT / "tests/native" / (name + ".c")),
                        str(self.stage / "skse_compat_values.c"), "-lm", "-o", str(executable)],
                       check=True, timeout=60)
        return subprocess.run([str(executable)], check=True, timeout=30, capture_output=True,
                              text=True).stderr.splitlines()

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler")
    def test_real_registration_table_dispatches_typed_results(self):
        self.run_fixture("test_classic_skse_ini")

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler")
    def test_skyui_version_expectations_match_the_guest_dispatch(self):
        classic.prepare(self.vendor, self.stage)
        source = (self.stage / "skse_compat.c").read_text()
        mode = source[source.index("int skse_skyui_enabled("):source.index("/* Verified against TESV")]
        start = source.index("static uint32_t skyui_version_value(")
        dispatch = source.index("static void run_native(", start)
        probe = source.index("static void probe_native(", dispatch)
        self.assertIn("value = skyui_version_value(fn, value);", source[dispatch:probe])
        self.assertIn("note_skyui_release(fn, value);", source[dispatch:probe])
        self.assertIn("expected = skyui_version_value(fn, expected);", source[probe:])
        self.assertIn("++skse_probe_depth;\n    uint32_t dispatched = skse_virtual", source[probe:])
        self.assertIn("--skse_probe_depth;\n    if(!dispatched", source[probe:])
        recorder = source[source.index("static _Thread_local unsigned skse_probe_depth;"):start]
        fixture = self.stage.parent / "skyui-version.c"
        fixture.write_text('#include <assert.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
                           '#include "skse_compat.h"\n' + mode + recorder + source[start:dispatch] + r"""
int main(void) {
    const uint32_t expected[2][5] = {{0, 1, 0, 0, UINT32_MAX}, {1, 7, 3, 48, UINT32_MAX}};
    const char *names[] = {"GetVersion", "GetVersionMinor", "GetVersionBeta",
                           "GetVersionRelease", "GetPluginVersion"};
    unsigned count = 0;
    const SkseNative *table = skse_native_table(&count);
    for (unsigned on = 0; on < 2; ++on) {
        assert(setenv("LB_SKSE_SKYUI_PROBE", on ? "1" : "0", 1) == 0);
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
            const SkseNative *fn = NULL;
            for (unsigned j = 0; j < count; ++j)
                if (!strcmp(table[j].class_name, "SKSE") && !strcmp(table[j].name, names[i]))
                    fn = &table[j];
            const uint32_t args[4] = {0};
            const char *strings[4] = {"test plugin", NULL, NULL, NULL};
            uint32_t value = 0;
            char *text = NULL;
            assert(fn && skse_native_eval(fn, args, strings, &value, &text) && !text);
            value = skyui_version_value(fn, value);
            assert(value == expected[on][i]);
            skse_probe_depth = !on;
            note_skyui_release(fn, value);
            skse_probe_depth = 0;
            if (on) note_skyui_release(fn, 0); // Duplicate real call stays silent.
        }
    }
    return 0;
}
""", encoding="utf-8")
        executable = fixture.with_suffix("")
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(self.stage), str(fixture), str(self.stage / "skse_compat_values.c"),
                        "-lm", "-o", str(executable)], check=True, timeout=60)
        result = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.stderr.splitlines(), ["IOS_CLASSIC_RELEASE value=48"])

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler")
    def test_game_queries_read_loaded_files_and_master_indices(self):
        lines = self.run_fixture("test_classic_skse_mod_list")
        self.assertEqual(len(lines), len(set(lines)))
        for query in range(7):
            self.assertIn(f"IOS_CLASSIC_QUERY id={query} handled=1", lines)
        self.assertIn("IOS_CLASSIC_QUERY id=0 handled=0", lines)
        for line in lines:
            self.assertRegex(line, r"^IOS_CLASSIC_QUERY id=[0-6] handled=[01]$")


if __name__ == "__main__":
    unittest.main()
