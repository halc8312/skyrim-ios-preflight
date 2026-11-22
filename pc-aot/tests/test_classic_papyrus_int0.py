"""Compile the actual staged Classic registration path with an authored registry."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
VM = ROOT / "experiments/skse-plugin-vm"
sys.path.insert(0, str(PORT))
sys.path.insert(0, str(VM))
import prepare_classic_skse as classic
from test_skse_plugin_vm import coff_tools, compile_interpreter
from test_probe_local_file import (check_owned_json_sequence, check_owned_settings_sequence, compile_local_probe,
                                   owned_register_integer_method, prepare_owned_json_inputs)

PROBE_DEFINE = "-DSKYRIM_IOS_CLASSIC_INT0_PROBE=1"
RT = r"""
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { uint32_t r[8]; } CPU;
typedef void (*ShimFn)(CPU *);
uint32_t *fixture_word(uint32_t address);
void *fixture_span(uint32_t address);
#define M32(address) (*fixture_word(address))
#define GPTR(address) (fixture_span(address))
#define GSTR(address) ((const char *)fixture_span(address))
#define ARG(n) M32(c->r[4] + 4 + 4 * (n))
#define RET(value, nargs) do { c->r[0] = (uint32_t)(value); c->r[4] += 4 + 4 * (nargs); return; } while (0)
#define LOG(...) ((void)0)
#define rt_fatal(...) abort()
CPU *rt_current_cpu(void);
uint32_t gmalloc(uint32_t bytes);
void gfree(uint32_t address);
uint32_t gstrdup(const char *text);
uint32_t rt_thunk(ShimFn function, const char *name);
uint32_t rt_invoke(CPU *c, uint32_t address, int argc, const uint32_t *args);
"""
SKSE_RUNTIME = r"""
#pragma once
#include "skse_compat.h"
int skse_system_run(CPU *, const SkseNative *, uint32_t, const uint32_t *, uint32_t, uint32_t);
"""


class ClassicInt0StagingTests(unittest.TestCase):
    def setUp(self):
        self.vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        scratch = tempfile.TemporaryDirectory(prefix="classic-int0-")
        self.addCleanup(scratch.cleanup)
        self.stage = Path(scratch.name) / "runtime"
        self.stage.mkdir()
        self.files = (*classic.PINNED, "skse_translation.c", classic.PRESENT_FILE, classic.INPUT_FILE)
        for name in self.files:
            shutil.copyfile(self.vendor / "runtime" / name, self.stage / name)
        (self.stage / "rt.h").write_text(RT, encoding="utf-8")
        (self.stage / "skse_runtime.h").write_text(SKSE_RUNTIME, encoding="utf-8")

    def scenario_flags(self):
        return ["-DGAME_SKYRIM", "-DSKYDREAM_SKSE", "-DSKYDREAM_SKYUI", "-I", str(self.stage), "-I", str(PORT)]

    @staticmethod
    def sanitizer_flags():
        if os.environ.get("SKYRIM_TEST_SANITIZERS") != "1":
            return []
        flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        if sys.platform.startswith("linux"):
            flags += ["-fno-pie", "-no-pie"]
        return flags

    def run_fixture(self, binary, *args):
        env = {**os.environ, "LB_SKSE_SELFTEST": "0"}
        result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=10, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.splitlines(), ["IOS_CLASSIC_VM registered=1 primitive_passed=0",
                                                      "IOS_CLASSIC_RELEASE value=48"])

    def test_stage_preserves_vendor_and_upgrades_the_release_logging_overlay(self):
        originals = [(self.vendor / "runtime" / name).read_bytes() for name in self.files]
        self.assertEqual(classic.prepare(self.vendor, self.stage), len(self.files))
        target = self.stage / "skse_compat.c"
        source = target.read_text(encoding="utf-8")
        self.assertLess(source.index("gfree(name);gfree(cls);"), source.index("int0_publish(c, registry, has);"))
        self.assertIn('fprintf(stderr, "IOS_CLASSIC_RELEASE value=%u', source)
        self.assertIn('fprintf(stderr, "IOS_CLASSIC_VM registered=%u', source)
        self.assertEqual(classic.prepare(self.vendor, self.stage), 0)
        int0_only = source.replace('#include "classic_papyrus_values.c.in"\n', "").replace(
            "    classic_values_publish(c, registry, has);\n", "")
        target.write_text(int0_only, encoding="utf-8")
        self.assertEqual(classic.prepare(self.vendor, self.stage), 1)
        old_stage = int0_only.replace(
            '#ifdef SKYRIM_IOS_CLASSIC_INT0_PROBE\n#include "classic_papyrus_int0.c.in"\n#endif\n\n', ""
        ).replace('#ifdef SKYRIM_IOS_CLASSIC_INT0_PROBE\n    int0_publish(c, registry, has);\n#endif\n', "")
        target.write_text(old_stage, encoding="utf-8")
        self.assertEqual(classic.prepare(self.vendor, self.stage), 1)
        self.assertEqual(target.read_text(encoding="utf-8"), source)
        self.assertEqual([(self.vendor / "runtime" / name).read_bytes() for name in self.files], originals)

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler and local Classic headers")
    def test_staged_engine_registration_ownership_and_rollback(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required")
        classic.prepare(self.vendor, self.stage)
        fixture = ROOT / "tests/native/test_classic_papyrus_int0.c"
        flags = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *self.scenario_flags()]
        ordinary = subprocess.run([*flags, "-E", "-P", fixture], check=True, capture_output=True,
                                  text=True, timeout=30).stdout
        self.assertNotIn("skse_classic_int0_request", ordinary)
        self.assertNotIn("int0_publish", ordinary)
        self.assertNotIn("Classic.Int0Probe.Run", ordinary)
        self.assertNotIn("Classic.ValueProbe.Run", ordinary)
        flags += self.sanitizer_flags()
        for defines, modes in (([], ("default",)),
                               ([PROBE_DEFINE], ("default", "success", "names", "table", "object", "values",
                                                 "values-rollback", "values-string", "values-form",
                                                 "values-form-unavailable", "values-none",
                                                 "values-reference-unavailable"))):
            binary = self.stage.parent / ("int0-enabled" if defines else "int0-default")
            subprocess.run([*flags, *defines, fixture, "-o", binary], check=True, timeout=60)
            for mode in modes:
                with self.subTest(mode=mode, probe=bool(defines)):
                    self.run_fixture(binary, mode)

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler for the staged engine fixture")
    def test_staged_engine_runs_interpreted_papyrus_native_and_rejects_wrong_type(self):
        host_cc = shutil.which(os.environ.get("CC", "cc"))
        if not host_cc:
            self.skipTest("C compiler required")
        coff_cc, linker, code_path = coff_tools()
        classic.prepare(self.vendor, self.stage)
        fixture = ROOT / "tests/native/test_classic_papyrus_int0.c"
        root = self.stage.parent
        binary = root / "int0-interpreted"
        compile_interpreter(host_cc, binary, fixture,
                            [*self.scenario_flags(), *self.sanitizer_flags(), PROBE_DEFINE,
                             "-DCLASSIC_INT0_TEST_INTERPRETER", "-I", str(VM)])
        source = VM / "test_papyrus_native_dll.c"
        for optimization in ("-O0", "-O2"):
            for bad_type in (False, True):
                with self.subTest(optimization=optimization, bad_type=bad_type):
                    stem = optimization[1:] + ("-bad" if bad_type else "")
                    obj, dll = root / f"{stem}.obj", root / f"{stem}.dll"
                    defines = ["-DPAPYRUS_NATIVE_FIXTURE_BAD_RESULT=1"] if bad_type else []
                    subprocess.run([coff_cc, "--target=i686-pc-windows-msvc", "-std=c11", "-ffreestanding",
                                    "-fno-stack-protector", "-Wall", "-Wextra", "-Werror", optimization, *defines,
                                    "-c", code_path(source), "-o", code_path(obj)], check=True, timeout=60)
                    subprocess.run([linker, "/dll", "/nodefaultlib", "/machine:x86", "/noentry", "/base:0x10000000",
                                    f"/out:{code_path(dll)}", code_path(obj)], check=True, timeout=60)
                    self.run_fixture(binary, "interpreter-bad" if bad_type else "interpreter", dll)


    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler for the staged engine fixture")
    def test_staged_engine_returns_the_verified_owned_papyrus_version(self):
        archive = os.environ.get("SKYRIM_TEST_OWNED_PAPYRUS_ZIP")
        if not archive:
            self.skipTest("Optional locally owned PapyrusUtil 3.3 ZIP required")
        host_cc = shutil.which(os.environ.get("CC", "cc"))
        if not host_cc:
            self.skipTest("C compiler required")
        with zipfile.ZipFile(archive) as mod:
            members = [entry for entry in mod.infolist() if entry.filename == "SKSE/Plugins/StorageUtil.dll"]
            self.assertEqual(len(members), 1)
            self.assertGreater(members[0].file_size, 0)
            self.assertLessEqual(members[0].file_size, 64 * 1024 * 1024)
            _, leaf, wrapper = owned_register_integer_method(mod.read(members[0]), 0x10046460)
        descriptor = self.stage.parent / "owned-int.bin"
        descriptor.write_bytes(b"".join(offset.to_bytes(4, "little") + code for offset, code in (leaf, wrapper)))
        classic.prepare(self.vendor, self.stage)
        fixture = ROOT / "tests/native/test_classic_papyrus_int0.c"
        binary = self.stage.parent / "int0-observed"
        compile_interpreter(host_cc, binary, fixture,
                            [*self.scenario_flags(), *self.sanitizer_flags(), PROBE_DEFINE,
                             "-DCLASSIC_INT0_TEST_INTERPRETER", "-I", str(VM)])
        self.run_fixture(binary, "owned-wrapper", descriptor)


    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler for the staged engine fixture")
    def test_staged_game_values_reach_owned_dll_and_private_profile(self):
        archive = os.environ.get("SKYRIM_TEST_OWNED_PAPYRUS_ZIP")
        executable = os.environ.get("SKYRIM_TEST_OWNED_LE_EXE")
        if not archive or not executable:
            self.skipTest("Optional locally owned LE executable and PapyrusUtil 3.3 ZIP required")
        host_cc = shutil.which(os.environ.get("CC", "cc"))
        if not host_cc:
            self.skipTest("C compiler required")
        classic.prepare(self.vendor, self.stage)
        fixture = ROOT / "tests/native/test_classic_papyrus_int0.c"
        root = self.stage.parent
        binary = root / "owned-classic-values"
        dll, words, scripts = prepare_owned_json_inputs(archive, executable, root)
        compile_local_probe(host_cc, binary, json_trial=True,
                            extra_flags=(*self.scenario_flags(), PROBE_DEFINE, "-DSKYRIM_TEST_CLASSIC_VALUES_ENGINE",
                                         "-I", str(fixture.parent)), extra_sources=(fixture,))
        check_owned_json_sequence(self, binary, dll, words, root, scripts)

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler for the staged engine fixture")
    def test_staged_save_refusal_discards_the_settings_backend_transaction(self):
        archive = os.environ.get("SKYRIM_TEST_OWNED_PAPYRUS_ZIP")
        executable = os.environ.get("SKYRIM_TEST_OWNED_LE_EXE")
        if not archive or not executable:
            self.skipTest("Optional locally owned LE executable and PapyrusUtil 3.3 ZIP required")
        host_cc = shutil.which(os.environ.get("CC", "cc"))
        if not host_cc:
            self.skipTest("C compiler required")
        classic.prepare(self.vendor, self.stage)
        fixture = ROOT / "tests/native/test_classic_papyrus_int0.c"
        root = self.stage.parent
        binary = root / "owned-settings-values"
        dll, words, scripts = prepare_owned_json_inputs(archive, executable, root)
        compile_local_probe(host_cc, binary, settings_backend=True,
                            extra_flags=(*self.scenario_flags(), PROBE_DEFINE, "-DSKYRIM_TEST_CLASSIC_VALUES_ENGINE",
                                         "-I", str(fixture.parent)), extra_sources=(fixture,))
        check_owned_settings_sequence(self, binary, dll, words, root, scripts, executable)


if __name__ == "__main__":
    unittest.main()
