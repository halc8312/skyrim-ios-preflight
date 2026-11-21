"""Exercise the optional MOD hook interface with authored guests and real C binding code."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ABI = ROOT / "ios/private-abi"
FUZ = ROOT / "experiments/fuz-voice"
FIXTURE = ROOT / "tests/native/test_mod_hooks_binding.c"


def write_fixture_headers(stage):
    # Same layout as the Classic binding fixture, with the named struct the
    # pinned runtime declares so the Fuz seam's struct CPU matches.
    (stage / "x86.h").write_text("""#pragma once
#include <stdint.h>
typedef struct { uint32_t bits; } Flags;
typedef struct CPU {
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


class GroupRules(unittest.TestCase):
    def test_header_bits_match_the_plan(self):
        text = (ABI / "PcGuestModHooksABI.h").read_text()
        for name, value in (("FUZ_MENU", 1), ("FUZ_WORLD", 2), ("VM_LIFECYCLE", 4), ("CHARGEN_PRECACHE", 8),
                            ("CAMERA_OBSERVE", 16), ("CHARGEN_SLIDER", 32), ("SAVE_LIFECYCLE", 64),
                            ("LOADING_MENU_OBSERVE", 128), ("INVENTORY_FIT", 256), ("SAFETY_LOAD", 512),
                            ("CAMERA_RENDER_OBSERVE", 1024), ("ECE_TRI_OBSERVE", 2048), ("FRESH_LOAD", 4096)):
            self.assertIn(f"PC_GUEST_MOD_HOOK_{name} = {value}u", text)
        self.assertIn('"skyrim_guest_bind_mod_hooks_v1"', text)


@unittest.skipUnless(sys.platform in ("darwin", "linux") and shutil.which(os.environ.get("CC", "cc")),
                     "Unix native compiler and dynamic loader required")
class NativeBinding(unittest.TestCase):
    def test_bind_select_once_and_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            write_fixture_headers(stage)
            flags = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-I", str(stage), "-I", str(ABI), "-I", str(FUZ), "-DSKYRIM_IOS_CLASSIC_SKSE=1"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            mac = sys.platform == "darwin"
            guest = [*flags, "-fPIC", "-fvisibility=hidden", "-dynamiclib" if mac else "-shared",
                     "-DMOD_FIXTURE_GUEST", FIXTURE, ABI / "pc_guest_library.c", ABI / "pc_guest_skse_library.c"]
            guests = {}
            for groups in (15, 3, 8, 4, 16, 32, 64, 128, 256, 640, 1024, 2048, 4095, 4164, 8191):
                guests[groups] = stage / f"guest-{groups}"
                subprocess.run([*guest, ABI / "pc_guest_mod_hooks_library.c",
                                f"-DPC_GUEST_MOD_HOOKS_COMPILED={groups}u", "-o", guests[groups]], check=True)
            plain = stage / "guest-without-hooks"
            subprocess.run([*guest, "-DMOD_FIXTURE_NO_HOOKS", "-o", plain], check=True)
            bad = stage / "guest-bad-reply"
            subprocess.run([*guest, "-DMOD_FIXTURE_NO_HOOKS", "-DMOD_FIXTURE_BAD_REPLY", "-o", bad], check=True)
            original = stage / "guest-original-table"
            subprocess.run([*guest, "-DMOD_FIXTURE_NO_HOOKS", "-DMOD_FIXTURE_ORIGINAL_TABLE", "-o", original], check=True)
            # Unknown groups and dependencies that aren't built are rejected.
            for groups in (512, 4096, 8192, 2):
                with self.subTest(compiled=groups):
                    rejected = subprocess.run([*guest, ABI / "pc_guest_mod_hooks_library.c",
                                               f"-DPC_GUEST_MOD_HOOKS_COMPILED={groups}u",
                                               "-o", stage / "rejected"], capture_output=True, text=True)
                    self.assertNotEqual(rejected.returncode, 0)
                    self.assertIn("static assertion", rejected.stderr)
            host = stage / "host"
            subprocess.run([*flags, "-DSKYRIM_IOS_MOD_HOOKS=1", "-pthread", FIXTURE, ABI / "pc_guest_host.c",
                            ABI / "pc_guest_skse_host.c", ABI / "pc_guest_mod_hooks_host.c",
                            *([] if mac else ["-ldl"]), "-o", host], check=True)
            for compiled, choice in ((15, 0), (15, 15), (15, 1), (15, 4), (3, 3), (3, 1), (8, 8), (4, 4),
                                     (16, 0), (16, 16), (32, 0), (32, 32), (128, 0), (128, 128),
                                     (64, 0), (64, 64), (256, 0), (256, 256), (640, 128), (640, 640),
                                     (1024, 0), (1024, 1024), (2048, 0), (2048, 2048), (4095, 4095),
                                     (4164, 68), (4164, 4164), (8191, 8191)):
                with self.subTest(compiled=compiled, choice=choice):
                    run = subprocess.run([host, guests[compiled], str(compiled), str(choice), plain, bad],
                                         capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertIn(f"PRIVATE_GUEST MOD hook groups {compiled}\n", run.stderr)
                    self.assertIn("PRIVATE_GUEST incompatible MOD hook interface", run.stderr)
            run = subprocess.run([host, guests[15], "15", "no-hooks", plain, bad], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertNotIn("MOD hook groups", run.stderr)
            for binary, groups, mode in ((original, 16, "original-guest"), (guests[4095], 4095, "original-host"),
                                         (guests[4095], 4095, "observer-host"), (guests[4095], 4095, "safety-host"),
                                         (guests[4095], 4095, "save-host"), (guests[4095], 4095, "camera-host"),
                                         (guests[8191], 8191, "ece-host")):
                with self.subTest(mode=mode):
                    run = subprocess.run([host, binary, str(groups), mode, plain, bad], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
