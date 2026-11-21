"""The app's Fuz hooks: reviewed experiment code, rt_call lookup and authored guest memory."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
FUZ = ROOT / "experiments/fuz-voice"
GUEST_HELPERS = ROOT / "experiments/skse-plugin-vm"


def write_runtime_headers(directory):
    # The runtime's CPU fields used here, MEM and the accessors of x86.h.
    (directory / "x86.h").write_text("""#pragma once
#include <stdint.h>
typedef struct { uint32_t bits; } Flags;
typedef struct CPU {
    uint32_t r[8], fs_base, df, eflags_hi, st[8], top, c0, c1, c2, c3;
    uint32_t fpu_cw, fpu_tag, xmm[16], mxcsr, mm[8];
    void *thread;
    Flags flags;
} CPU;
extern uint8_t *MEM;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
#define M8(a)  (*(uint8_t *)(MEM + (uint32_t)(a)))
#define M32(a) (*(u32u *)(MEM + (uint32_t)(a)))
""")
    (directory / "rt_table.h").write_text("""#pragma once
typedef struct { uint32_t addr; void (*fn)(CPU *); } FnEntry;
typedef struct { const char *name; const FnEntry *table; unsigned count; } FnModule;
""")


@unittest.skipUnless(sys.platform.startswith("linux") or sys.platform == "darwin", "POSIX host with mmap required")
class FuzHostTests(unittest.TestCase):
    def test_hooks_through_the_app_lookup_path(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("A C compiler is required.")
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            write_runtime_headers(stage)
            flags = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-pthread",
                     # The app's lookup headers first, as in the app build.
                     "-I", str(PORT / "fuz-runtime-abi"), "-I", str(stage), "-I", str(PORT),
                     "-I", str(ROOT / "ios/private-abi"), "-I", str(FUZ), "-I", str(GUEST_HELPERS)]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            elif os.environ.get("SKYRIM_TEST_SANITIZERS") == "thread":
                flags += ["-fsanitize=thread"]
            binary = stage / "fuz-host"
            sources = [ROOT / "tests/native/test_mod_fuz_host.c", PORT / "mod_fuz_host.c", PORT / "mod_fuz.c",
                       PORT / "classic_vm_epoch.c", PORT / "mod_plugins.c",
                       *(FUZ / n for n in ("voice_fallback.c", "lift_voice_seam.c", "lift_menu_seam.c",
                                           "lift_world_seam.c", "lift_resource_lookup.c")),
                       GUEST_HELPERS / "x86_winfile.c"]
            subprocess.run([*flags, *map(str, sources), "-o", str(binary)], check=True, timeout=120)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:allocator_may_return_null=1")
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60, env=env)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_layout_constants_match_the_pinned_runtime(self):
        vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream")) / "runtime"
        if not (vendor / "rt.h").is_file():
            self.skipTest("Pinned Skydream checkout required (MOD_FIRST_WAVE_PLAN.md section 3.1)")
        rt = (vendor / "rt.h").read_text()
        header = (PORT / "mod_fuz_host.h").read_text()
        for name, ours in (("GUEST_STACK_BASE", "SKYRIM_FUZ_STACK_BASE"), ("GUEST_STACK_SIZE", "SKYRIM_FUZ_STACK_SIZE"),
                           ("GUEST_STACK_SLOTS", "SKYRIM_FUZ_STACK_SLOTS"), ("GUEST_TEB_BASE", "SKYRIM_FUZ_TEB_BASE")):
            theirs = re.search(rf"#define {name}\s+(\w+)", rt).group(1).rstrip("u")
            mine = re.search(rf"{ours} = (\w+)", header).group(1).rstrip("u")
            self.assertEqual(int(theirs, 0), int(mine, 0), name)
        kernel = (vendor / "kernel32.c").read_text()
        self.assertIn("uint32_t stack_hi = stack_lo + GUEST_STACK_SIZE - 64;", kernel)
        self.assertIn("uint32_t teb = GUEST_TEB_BASE + slot * 0x2000;", kernel)
        self.assertIn("M32(teb + 0x04) = stack_hi;", kernel)
        self.assertIn("M32(teb + 0x08) = stack_lo;", kernel)
        self.assertIn("mprotect(p,0x10000,PROT_NONE)", (vendor / "host.c").read_text())


if __name__ == "__main__":
    unittest.main()
