"""Exercise the build-time Classic binding with authored data and real C callbacks."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import classic_skse_guest as classic


class GeneratedSources(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.output = self.base / "experiment"
        self.units = [self.base / f"exe_{n}.c" for n in range(3)]
        keys = [b"f_" + k for k in classic.ENTRIES]
        self.defs = [b"void " + k + b"(CPU *c) { return; }\n" for k in keys]
        self.calls = b"void caller(CPU *c) { " + b" ".join(k + b"(c);" for k in keys) + b" }\n"
        self.units[0].write_bytes(b"".join(self.defs[:2]) + self.calls)
        self.units[1].write_bytes(b"".join(self.defs[2:]))
        self.units[2].write_bytes(self.calls)
        self.table = self.base / "exe_table.c"
        self.table.write_bytes(b"\n".join(b"{0x" + k + b"u, f_" + k + b"}," for k in classic.ENTRIES))
        self.sources = [*self.units, self.table]

    def test_changes_only_private_definitions_not_callers_or_table(self):
        before = [p.read_bytes() for p in self.sources]
        result = classic.prepare_sources(self.sources, self.output)
        self.assertEqual([p.read_bytes() for p in self.sources], before)
        self.assertEqual([p.parent for p in result], [self.output, self.output, self.base, self.base])
        self.assertIn(self.calls, result[0].read_bytes())
        for path in result[:2]:
            self.assertEqual(path.read_bytes().count(b"_guest(CPU *c) {"), 2)
        self.assertEqual(classic.prepare_sources(self.sources, self.output), result)

    def test_missing_table_entry_duplicate_and_nonoriginal_fail_before_writing(self):
        for which in ("table", "missing", "duplicate", "renamed"):
            with self.subTest(which=which):
                path = self.table if which == "table" else self.units[1]
                original = path.read_bytes()
                if which == "table":
                    changed = original.replace(b"{0xA60200u, f_A60200}", b"{0xA60200u, other}")
                elif which == "missing":
                    changed = original.replace(self.defs[-1], b"")
                elif which == "duplicate":
                    changed = original + self.defs[0]
                else:
                    changed = original.replace(b"f_873D70(", b"f_873D70_guest(")
                path.write_bytes(changed)
                with self.assertRaises(ValueError):
                    classic.prepare_sources(self.sources, self.output)
                self.assertFalse(self.output.exists())
                path.write_bytes(original)

    def test_accepts_only_matching_revision_candidate_and_discovery(self):
        state = dict(source_commit=classic.PIN, candidate_sha256=classic.CANDIDATE_SHA,
                     discovery_sha256=classic.DISCOVERY_SHA)
        classic.validate_generation(state)
        for key in state:
            with self.subTest(key=key), self.assertRaises(ValueError):
                classic.validate_generation(dict(state, **{key: "different"}))


@unittest.skipUnless(sys.platform in ("darwin", "linux") and shutil.which("cc"),
                     "Unix native compiler and dynamic loader required")
class NativeCallbacks(unittest.TestCase):
    def test_rejects_incomplete_tables_and_calls_all_four_originals(self):
        abi = ROOT / "ios/private-abi"
        fixture = ROOT / "tests/native/test_classic_skse_binding.c"
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            (stage / "x86.h").write_text(
                "#pragma once\n#include <stdint.h>\ntypedef struct { unsigned value; } CPU;\n")
            (stage / "rt_table.h").write_text("#pragma once\ntypedef struct FnEntry FnEntry;\n")
            flags = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-I", str(stage), "-I", str(abi)]
            guest = stage / "fixture-guest"
            mac = sys.platform == "darwin"
            subprocess.run([*flags, "-fPIC", "-fvisibility=hidden", "-dynamiclib" if mac else "-shared",
                            "-DCLASSIC_FIXTURE_GUEST", str(fixture), str(abi/"pc_guest_skse_library.c"),
                            "-o", str(guest)], check=True)
            host = stage / "fixture-host"
            subprocess.run([*flags, "-DSKYRIM_IOS_CLASSIC_SKSE=1", str(fixture),
                            str(abi/"pc_guest_skse_host.c"), *([] if mac else ["-ldl"]),
                            "-o", str(host)], check=True)
            env = os.environ.copy()
            env.pop("LB_SKSE_SKYUI_PROBE", None)
            subprocess.run([str(host), str(guest)], env=env, check=True)


if __name__ == "__main__":
    unittest.main()
