"""Game-free native x86 comparison of flags across actual emitted C calls.

Uses pinned public sources plus patches 0001--0005; never downloads dependencies.
Run on x86_64 POSIX with existing capstone, pefile and a C compiler.
"""
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
PATCHES = (
    "0001-auxiliary-carry.patch", "0002-report-empty-jump-tables.patch",
    "0003-negative-index-candidate-roots.patch", "0004-preserve-native-audio-source.patch",
)
SOURCES = (
    "runtime/x86.h", "runtime/lift_prelude.h", "runtime/rt_table.h",
    "tools/lift.py", "tools/peimg.py", "tools/discover.py", "desktop/CMakeLists.txt",
)
HAS_DECODER = all(importlib.util.find_spec(x) is not None for x in ("capstone", "pefile"))
HAS_NATIVE = os.name == "posix" and platform.machine().lower() in ("x86_64", "amd64")
NAMES = ("incoming_cf", "returned_zf", "returned_af", "preserved_flags",
         "indirect_tail", "nested_indirect_call", "direct_tail", "host_entry")
MASK = 0x8D5  # Only the six defined arithmetic flags, including AF.


def fixture(path):
    """A minimal PE containing only hand-written synthetic instruction bytes."""
    data = bytearray(0x1200)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x14C, 1, 0, 0, 0, 224, 0x102)
    opt = 0x98
    struct.pack_into("<H", data, opt, 0x10B)
    struct.pack_into("<III", data, opt + 16, 0x1000, 0x1000, 0x1000)
    struct.pack_into("<III", data, opt + 28, 0x400000, 0x1000, 0x200)
    struct.pack_into("<II", data, opt + 56, 0x2000, 0x200)
    struct.pack_into("<H", data, opt + 68, 3)
    struct.pack_into("<I", data, opt + 92, 16)
    struct.pack_into("<8sIIIIIIHHI", data, opt + 224, b".text", 0x1000,
                     0x1000, 0x1000, 0x200, 0, 0, 0, 0, 0x60000020)
    data[0x200:] = b"\xCC" * 0x1000
    functions = {}

    def emit(addr, code):
        offset = 0x200 + addr - 0x401000
        data[offset:offset + len(code)] = code
        functions[hex(addr)] = [[addr, addr + len(code)]]

    def rel(opcode, addr, target):
        return bytes([opcode]) + struct.pack("<i", target - addr - 5)

    def entry(addr, prefix, suffix=b""):
        emit(addr, prefix + rel(0xE8, addr + len(prefix), addr + 0x40)
             + suffix + b"\x9C\x5A\xC3")  # pushfd; pop edx; ret

    a = 0x401000
    entry(a, bytes.fromhex("b80f000000 f9"))  # CF into ADC in the callee.
    emit(a + 0x40, bytes.fromhex("83d000 c3"))
    a += 0x100
    entry(a, bytes.fromhex("b801000000 85c0"), bytes.fromhex("0f94c0"))
    emit(a + 0x40, bytes.fromhex("83e801 c3"))  # Callee ZF consumed by SETZ.
    a += 0x100
    entry(a, bytes.fromhex("b80f000000 83c000"), b"\x9F")
    emit(a + 0x40, bytes.fromhex("0401 c3"))  # Callee AF consumed by LAHF.
    a += 0x100
    entry(a, bytes.fromhex("68d7080000 9d"))
    emit(a + 0x40, bytes.fromhex("9f c3"))  # All six flags reach callee intact.
    a += 0x100
    entry(a, bytes.fromhex("b80f000000 f9"))
    emit(a + 0x40, b"\xBA" + struct.pack("<I", a + 0x80) + b"\xFF\xE2")
    emit(a + 0x80, bytes.fromhex("83d000 c3"))
    a += 0x100
    entry(a, bytes.fromhex("b80f000000 f9"))
    emit(a + 0x40, b"\xB9" + struct.pack("<I", a + 0x80) + bytes.fromhex("ffd1 9f c3"))
    emit(a + 0x80, bytes.fromhex("83d000 c3"))
    a += 0x100
    entry(a, bytes.fromhex("b80f000000 f9"))
    emit(a + 0x40, rel(0xE9, a + 0x40, a + 0x80))
    emit(a + 0x80, bytes.fromhex("83d000 c3"))
    a += 0x100
    emit(a, bytes.fromhex("9f 9c 5a c3"))  # CPU flags at outermost entry.
    path.write_bytes(data)
    return {"functions": functions, "iat": {}, "jump_tables": {}}


DRIVER = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "funcs_exe.h"
#include "rt_table.h"
extern const FnEntry g_fn_table_exe[];
extern const unsigned g_fn_count_exe;
static uint8_t memory[4096];
uint8_t *MEM = memory;
static const FnEntry *lookup(uint32_t target) {
    for (unsigned k = 0; k < g_fn_count_exe; ++k)
        if (g_fn_table_exe[k].addr == target) return &g_fn_table_exe[k];
    fprintf(stderr, "unresolved synthetic target: %x\n", target); abort();
}
void rt_call(CPU *c, uint32_t target) { lookup(target)->fn(c); }
void rt_call_ic(CPU *c, uint32_t target, const FnEntry **cache) {
    const FnEntry *entry = lookup(target);
    __atomic_store_n(cache, entry, __ATOMIC_RELAXED);
    entry->fn(c);
}
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c; fprintf(stderr, "unsupported at %x: %s\n", addr, what); abort();
}
/* No red zone: native CALL and PUSHFQ use the stack inside this assembly.
 * Restore host RFLAGS after every reference; TF, DF and AC are never seeded. */
#define REF(body) __asm__ volatile( \
    "pushfq\n\tpopq %%r11\n\t" body \
    "\n\tpushfq\n\tpopq %[flags]\n\tpushq %%r11\n\tpopfq" \
    : "+&a"(eax), [flags] "=&r"(flags) : : "rcx", "rdx", "r11", "cc", "memory")
static void native(unsigned id, uint32_t *result, uint32_t *out_flags) {
    uint32_t eax = 0;
    uint64_t flags;
    switch (id) {
    case 0: REF("movl $15, %%eax; stc; call 1f; jmp 2f; 1: adcl $0, %%eax; ret; 2:"); break;
    case 1: REF("movl $1, %%eax; testl %%eax, %%eax; call 1f; setz %%al; jmp 2f; 1: subl $1, %%eax; ret; 2:"); break;
    case 2: REF("movl $15, %%eax; addl $0, %%eax; call 1f; lahf; jmp 2f; 1: addb $1, %%al; ret; 2:"); break;
    case 3: REF("pushq $0x8d7; popfq; call 1f; jmp 2f; 1: lahf; ret; 2:"); break;
    case 4: REF("movl $15, %%eax; stc; call 1f; jmp 3f; 1: leaq 2f(%%rip), %%rdx; jmp *%%rdx; 2: adcl $0, %%eax; ret; 3:"); break;
    case 5: REF("movl $15, %%eax; stc; call 1f; jmp 3f; 1: leaq 2f(%%rip), %%rcx; call *%%rcx; lahf; ret; 2: adcl $0, %%eax; ret; 3:"); break;
    case 6: REF("movl $15, %%eax; stc; call 1f; jmp 3f; 1: jmp 2f; 2: adcl $0, %%eax; ret; 3:"); break;
    case 7: REF("pushq $0x8d7; popfq; lahf"); break;
    default: abort();
    }
    *result = eax; *out_flags = (uint32_t)flags & 0x8d5u;
}
int main(void) {
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned id = 0; id < 8; ++id) {
            CPU c = {0}; memset(memory, 0, sizeof(memory)); c.r[4] = 0x100;
#ifdef HAVE_CPU_FLAGS
            if (id == 7) c.flags = (Flags){1, 1, 1, 1, 1, 1};
#endif
            uint32_t expected_eax, expected_flags;
            native(id, &expected_eax, &expected_flags);
            lookup(0x401000u + id * 0x100u)->fn(&c);
            uint32_t cpu_flags = 0;
#ifdef HAVE_CPU_FLAGS
            cpu_flags = c.flags.cf | (c.flags.pf << 2) | (c.flags.af << 4)
                | (c.flags.zf << 6) | (c.flags.sf << 7) | (c.flags.of << 11);
#endif
            printf("%u %u %u %u %u %u %u %u\n", pass, id, expected_eax,
                expected_flags, c.r[0], c.r[2] & 0x8d5u, c.r[4], cpu_flags);
        }
    }
    return 0;
}
'''


@unittest.skipUnless(shutil.which("git") and shutil.which("cc") and HAS_NATIVE and HAS_DECODER,
                     "Existing Git, cc, capstone, pefile and native x86_64 POSIX required")
class CallFlagsTests(unittest.TestCase):
    def test_native_calls_returns_tailcalls_and_cache_reuse(self):
        with tempfile.TemporaryDirectory(prefix="call-flags-", dir=os.environ.get("CALL_FLAGS_WORK_ROOT")) as tmp:
            folder = Path(tmp)
            baseline, changed = folder / "baseline", folder / "patched"
            for name in SOURCES:
                result = subprocess.run(["git", "-C", str(ROOT / "vendor/skydream"), "show", PIN + ":" + name],
                                        capture_output=True)
                if result.returncode:
                    self.skipTest("Pinned public source unavailable; tests never download it")
                target = baseline / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(result.stdout)
            self.run_ok(["git", "init", "--quiet"], cwd=baseline)
            for patch in PATCHES:
                self.apply(baseline, patch)
            shutil.copytree(baseline, changed, ignore=shutil.ignore_patterns(".git"))
            self.run_ok(["git", "init", "--quiet"], cwd=changed)
            self.apply(changed, "0005-call-flags.patch")
            exe = folder / "synthetic.exe"
            graph = folder / "functions.json"
            graph.write_text(json.dumps(fixture(exe)), encoding="utf-8")
            driver = folder / "driver.c"
            driver.write_text(DRIVER, encoding="utf-8")
            outputs = {}
            for label, stage in (("baseline", baseline), ("patched", changed)):
                out = folder / (label + "-generated")
                lifted = self.run_ok([sys.executable, str(stage / "tools/lift.py"), str(exe), str(graph), str(out)],
                                     env={**os.environ, "LIFT_FILES": "1", "LIFT_TSO": "0"})
                self.assertIn("rejected 0", lifted.stdout)
                self.assertEqual(json.loads((out / "exe_rejected.json").read_text()), {})
                binary = folder / (label + "-check")
                self.run_ok(["cc", "-O2", "-std=gnu11", "-mno-red-zone", "-I", str(stage / "runtime"),
                             "-I", str(out), *(["-DHAVE_CPU_FLAGS"] if label == "patched" else []),
                             str(driver), str(out / "exe_00.c"), str(out / "exe_table.c"), "-lm", "-o", str(binary)])
                outputs[label] = [tuple(map(int, row.split())) for row in self.run_ok([str(binary)]).stdout.splitlines()]
            # The native oracle has independent, concrete sanity checks.
            expected = ((16, 0x10), (1, 0x44), (0x1210, 0x10), (0xD700, MASK),
                        (16, 0x10), (0x1210, 0x10), (16, 0x10), (0xD700, MASK))
            self.assertEqual(len(outputs["patched"]), 16)
            for row in outputs["patched"]:
                turn, case, native_eax, native_flags, eax, flags, esp, cpu_flags = row
                with self.subTest(case=NAMES[case], cache_pass=turn):
                    self.assertEqual((native_eax, native_flags), expected[case])
                    self.assertEqual((eax, flags), (native_eax, native_flags))
                    self.assertEqual(cpu_flags, native_flags)  # RET exposes flags to host.
                    self.assertEqual(esp, 0x104)
            for row in outputs["baseline"]:
                turn, case, native_eax, native_flags, eax, flags, esp, _ = row
                with self.subTest(baseline_case=NAMES[case], cache_pass=turn):
                    self.assertNotEqual((eax, flags), (native_eax, native_flags))
                    self.assertEqual(esp, 0x104)

    def run_ok(self, argv, **kwargs):
        result = subprocess.run(argv, capture_output=True, text=True, timeout=60, **kwargs)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def apply(self, stage, name):
        for flags in (["--check"], []):
            self.run_ok(["git", "-c", "core.autocrlf=false", "apply", *flags,
                         str(ROOT / "patches/skydream" / name)], cwd=stage)


if __name__ == "__main__":
    unittest.main()
