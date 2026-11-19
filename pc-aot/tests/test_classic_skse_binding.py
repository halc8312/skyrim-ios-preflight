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


def write_fixture_headers(stage):
    (stage / "x86.h").write_text("""#pragma once
#include <stdint.h>
typedef struct { uint32_t bits; } Flags;
typedef struct {
    uint32_t r[8], fs_base, df, eflags_hi, st[8], top, c0, c1, c2, c3;
    uint32_t fpu_cw, fpu_tag, xmm[16], mxcsr, mm[8];
    void *thread;
    Flags flags;
} CPU;
void rt_call(CPU *, uint32_t);
void rt_unimpl(CPU *, uint32_t, const char *);
void rt_int3(CPU *, uint32_t);
void rt_cpuid(CPU *, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
uint64_t rt_rdtsc(void);
double f80_load(uint32_t);
void f80_store(uint32_t, double);
void fxam(CPU *);
void fprem(CPU *, int);
void fpu_env(CPU *, uint32_t, int);
""")
    (stage / "rt_table.h").write_text("""#pragma once
typedef struct { uint32_t addr; void (*fn)(CPU *); } FnEntry;
typedef struct { const char *name; const FnEntry *table; unsigned count; } FnModule;
extern FnModule g_modules[];
unsigned rt_module_count(unsigned index);
""")


@unittest.skipUnless(sys.platform in ("darwin", "linux") and shutil.which(os.environ.get("CC", "cc")),
                     "Unix native compiler and dynamic loader required")
class NativeCallbacks(unittest.TestCase):
    def test_single_startup_choice_and_ordinary_v1(self):
        abi = ROOT / "ios/private-abi"
        fixture = ROOT / "tests/native/test_classic_skse_binding.c"
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            write_fixture_headers(stage)
            flags = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-I", str(stage), "-I", str(abi)]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            classic_mode = "-DSKYRIM_IOS_CLASSIC_SKSE=1"
            mac = sys.platform == "darwin"
            guest_flags = [*flags, "-fPIC", "-fvisibility=hidden", "-dynamiclib" if mac else "-shared",
                           "-DCLASSIC_FIXTURE_GUEST", fixture, abi / "pc_guest_library.c"]
            plain, paired, malformed = (stage / name for name in ("plain", "paired", "malformed"))
            subprocess.run([*guest_flags, "-o", plain], check=True)
            subprocess.run([*guest_flags, classic_mode, abi / "pc_guest_skse_library.c",
                            "-o", paired], check=True)
            subprocess.run([*guest_flags, classic_mode, "-DCLASSIC_FIXTURE_BAD",
                            "-o", malformed], check=True)

            bridge = stage / "bridge.o"
            subprocess.run([*flags, classic_mode, "-Dsetenv=fixture_setenv",
                            "-c", abi / "pc_guest_skse_host.c", "-o", bridge], check=True)
            host_flags = [*flags, fixture, abi / "pc_guest_host.c"]
            loader_flags = [] if mac else ["-ldl"]
            trial_host, ordinary_host = stage / "trial-host", stage / "ordinary-host"
            subprocess.run([*host_flags, classic_mode, bridge, *loader_flags, "-o", trial_host], check=True)
            subprocess.run([*host_flags, *loader_flags, "-o", ordinary_host], check=True)
            for mode in ("off", "on", "fail-first", "fail-second"):
                with self.subTest(mode=mode):
                    subprocess.run([trial_host, paired, plain, malformed, mode], check=True)
            subprocess.run([ordinary_host, plain, "accepted"], check=True)
            subprocess.run([ordinary_host, paired, "wrong"], check=True)


if __name__ == "__main__":
    unittest.main()
