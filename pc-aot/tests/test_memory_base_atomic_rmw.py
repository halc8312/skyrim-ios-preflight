"""Patch 0010: guest memory base in a local, and single atomic read-modify-write for LOCK add/sub/and/or/xor/inc/dec.

The actual translator is run twice on the same hand-written instructions, once with patches 0001-0009 and once with
0010 added. The emitted C of both is compiled natively and run on identical register, flag and memory inputs:
registers, the six arithmetic flags and guest memory must agree for every vector. No game input is used.
This proves the translated semantics on x86_64 only; it neither measures speed nor runs ARM64 or an iPhone.
"""
import json
import os
from pathlib import Path
import platform
import random
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
VENDOR = ROOT / "vendor/skydream"
PATCHES = ROOT / "patches/skydream"
OLD = [PATCHES / n for n in (
    "0001-auxiliary-carry.patch", "0002-report-empty-jump-tables.patch", "0003-negative-index-candidate-roots.patch",
    "0005-call-flags.patch", "0006-inline-ordered-stores.patch",
    "0007-skip-fences-implied-by-strict-order.patch", "0008-reduce-call-flag-traffic.patch",
    "0009-keep-x87-top-in-a-local.patch")]
PATCH_0010 = PATCHES / "0010-memory-base-and-atomic-rmw.patch"
NEW = OLD + [PATCH_0010]
# Patch 0010 is independent of 0008 and 0009: the build that applies only 0007 must work too.
MINIMAL = OLD[:6]
MINIMAL_NEW = MINIMAL + [PATCH_0010]
CANDIDATES = [os.environ.get("SKYRIM_LIFTER_PYTHON", ""), sys.executable]
TOOLS = ("as", "ld", "objcopy", "nm", "gcc")

# One function per entry: "name": instruction lines (a trailing ret is added).
FUNCTIONS = {
    "lock_add32_reg": ["lock add dword ptr [ecx], eax"],
    "lock_add32_imm8": ["lock add dword ptr [ecx + 4], -1"],
    "lock_add32_imm32": ["lock add dword ptr [ecx], 0x12345678"],
    "lock_add16_reg": ["lock add word ptr [ecx], ax"],
    "lock_add16_imm": ["lock add word ptr [ecx], 0x7fff"],
    "lock_add8_reg": ["lock add byte ptr [ecx], al"],
    "lock_add8_imm": ["lock add byte ptr [ecx], 0x80"],
    "lock_sub32_reg": ["lock sub dword ptr [ecx + 8], edx"],
    "lock_sub16_reg": ["lock sub word ptr [ecx], dx"],
    "lock_sub8_imm": ["lock sub byte ptr [ecx], 1"],
    "lock_and32_reg": ["lock and dword ptr [ecx], eax"],
    "lock_and32_imm": ["lock and dword ptr [ecx], 0xff00ff00"],
    "lock_and8_imm": ["lock and byte ptr [ecx], 0x0f"],
    "lock_or32_reg": ["lock or dword ptr [ecx], edx"],
    "lock_or16_imm": ["lock or word ptr [ecx], 0x8000"],
    "lock_xor32_reg": ["lock xor dword ptr [ecx], eax"],
    "lock_xor8_reg": ["lock xor byte ptr [ecx], dl"],
    "lock_inc32": ["lock inc dword ptr [ecx]"],
    "lock_inc16": ["lock inc word ptr [ecx]"],
    "lock_inc8": ["lock inc byte ptr [ecx]"],
    "lock_dec32": ["lock dec dword ptr [ecx]"],
    "lock_dec16": ["lock dec word ptr [ecx]"],
    "lock_dec8": ["lock dec byte ptr [ecx]"],
    "lock_add_indexed": ["lock add dword ptr [ecx + edx*4], eax"],
    # Kept as compare-exchange loops (no single atomic instruction with the same flags).
    "lock_adc32": ["lock adc dword ptr [ecx], eax"],
    "lock_neg32": ["lock neg dword ptr [ecx]"],
    "lock_not32": ["lock not dword ptr [ecx]"],
    # Plain accesses through the guest base: pointer chain, mixed widths, unaligned, stores.
    "chain": ["mov eax, [ecx + 8]", "mov edx, [eax + 0x10]", "mov [ecx + 4], edx", "mov eax, [ecx + 0x14]"],
    "narrow": ["movzx eax, byte ptr [ecx + 1]", "mov word ptr [ecx + 2], ax", "mov byte ptr [ecx + 7], dl"],
    "unaligned": ["mov eax, [ecx + 1]", "mov [ecx + 6], eax", "add word ptr [ecx + 3], dx"],
    "rmw_plain": ["add dword ptr [ecx], eax", "sub word ptr [ecx + 4], dx", "inc byte ptr [ecx + 8]"],
    "xchg_cmpxchg": ["xchg dword ptr [ecx], eax", "cmpxchg dword ptr [ecx + 4], edx", "xadd dword ptr [ecx + 8], eax"],
}
MEM_BYTES = 0x2000


def lifter_python():
    for candidate in CANDIDATES:
        if candidate and Path(candidate).is_file():
            probe = subprocess.run([candidate, "-c", "import capstone, pefile"], capture_output=True)
            if probe.returncode == 0:
                return candidate
    return None


def run(command, cwd=None, **kw):
    return subprocess.run([str(c) for c in command], cwd=cwd, check=True, capture_output=True, timeout=600, **kw)


def build_tree(directory, patches):
    directory.mkdir()
    archive = directory / "source.tar"
    subprocess.run(["git", "-c", "core.autocrlf=false", "-C", str(VENDOR), "archive", "--format=tar", "-o", str(archive),
                    PIN, "tools", "runtime"], check=True, timeout=120)
    with tarfile.open(archive) as f:
        f.extractall(directory / "source", filter="data")
    subprocess.run(["git", "init", "--quiet"], cwd=directory / "source", check=True)
    for patch in patches:
        subprocess.run(["git", "-c", "core.autocrlf=false", "apply", str(patch)], cwd=directory / "source", check=True)
    return directory / "source"


def assemble(base):
    lines = [".intel_syntax noprefix", ".text"]
    for name, body in FUNCTIONS.items():
        lines += [".balign 16", ".globl %s\n%s:" % (name, name)] + ["    " + b for b in body] + ["    ret"]
        lines.append(".globl %s__end\n%s__end:" % (name, name))
    (base / "f.s").write_text("\n".join(lines) + "\n")
    run(["as", "--32", "-o", "f.o", "f.s"], cwd=base)
    run(["ld", "-m", "elf_i386", "-Ttext=0x401000", "-e", "0x401000", "-o", "f.elf", "f.o"], cwd=base)
    run(["objcopy", "-O", "binary", "-j", ".text", "f.elf", "f.bin"], cwd=base)
    symbols = {}
    for line in run(["nm", "f.elf"], cwd=base).stdout.decode().splitlines():
        address, _, name = line.split()
        symbols[name] = int(address, 16)
    functions = {hex(symbols[n]): [[symbols[n], symbols[n + "__end"]]] for n in FUNCTIONS}
    return functions, (base / "f.bin").read_bytes(), {n: symbols[n] for n in FUNCTIONS}


def synthetic_pe(directory, functions, code):
    size = (len(code) + 511) // 512 * 512
    assert size <= 0x1000
    image = bytearray(512 + size)
    image[:2] = b"MZ"; struct.pack_into("<I", image, 0x3c, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x84, 0x14c, 1, 0, 0, 0, 224, 0x102)
    optional = 0x98
    struct.pack_into("<H", image, optional, 0x10b)
    for offset, value in {4: size, 16: 0x1000, 20: 0x1000, 28: 0x400000, 32: 0x1000, 36: 512, 56: 0x2000,
                          60: 512, 72: 0x100000, 76: 0x1000, 80: 0x100000, 84: 0x1000, 92: 16}.items():
        struct.pack_into("<I", image, optional + offset, value)
    struct.pack_into("<H", image, optional + 68, 3)
    struct.pack_into("<8sIIIIIIHHI", image, optional + 224, b".text\0\0\0", size, 0x1000, size, 512, 0, 0, 0, 0,
                     0x60000020)
    image[512:512 + len(code)] = code
    (directory / "synthetic.exe").write_bytes(image)
    (directory / "functions.json").write_text(json.dumps({"functions": functions, "iat": {}, "jump_tables": {}}))


def lift(python, tree, work, out):
    out.mkdir()
    env = dict(os.environ, LIFT_GAME="skyrim", LIFT_TSO="1", LIFT_MEMORDER="1", LIFT_FILES="1")
    subprocess.run([python, "-E", "-s", "-B", str(tree / "tools/lift.py"), str(work / "synthetic.exe"),
                    str(work / "functions.json"), str(out), "exe"], cwd=tree, env=env, check=True,
                   capture_output=True, timeout=300)
    rejected = json.loads((out / "exe_rejected.json").read_text())
    if rejected:
        raise RuntimeError("synthetic functions rejected: %s" % rejected)


DRIVER = r"""
#include "x86.h"
#include "funcs_exe.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
uint8_t *MEM;
void rt_call(CPU *c, uint32_t t) { (void)c; fprintf(stderr, "rt_call %x\n", t); abort(); }
void rt_call_ic(CPU *c, uint32_t t, const void **cache) { (void)c; (void)cache; fprintf(stderr, "ic %x\n", t); abort(); }
void rt_unimpl(CPU *c, uint32_t a, const char *w) { (void)c; fprintf(stderr, "unimpl %x %s\n", a, w); abort(); }
void rt_int3(CPU *c, uint32_t a) { (void)c; (void)a; abort(); }
void rt_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *d, uint32_t *e) { (void)c; (void)a; (void)b; (void)d; (void)e; }
uint64_t rt_rdtsc(void) { return 0; }
double f80_load(uint32_t a) { (void)a; return 0; }
void f80_store(uint32_t a, double v) { (void)a; (void)v; }
void fxam(CPU *c) { (void)c; }
void fprem(CPU *c, int i) { (void)c; (void)i; }
void fpu_env(CPU *c, uint32_t a, int o) { (void)c; (void)a; (void)o; }
void *rt_setjmp_slot(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
void rt_setjmp3(CPU *c) { (void)c; }
static const struct { const char *name; void (*fn)(CPU *); } table[] = { TABLE };
int main(int argc, char **argv) {
    (void)argc;
    MEM = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) return 2;
    FILE *in = fopen(argv[1], "r");
    unsigned f, eax, ecx, edx, bits, seed;
    while (fscanf(in, "%u %x %x %x %x %u", &f, &eax, &ecx, &edx, &bits, &seed) == 6) {
        uint64_t s = seed * 2654435761u + 1;
        for (unsigned i = 0; i < MEM_BYTES; i++) { s = s * 6364136223846793005ull + 1442695040888963407ull; MEM[i] = (uint8_t)(s >> 33); }
        CPU c; memset(&c, 0, sizeof c);
        c.r[0] = eax; c.r[1] = ecx; c.r[2] = edx; c.r[3] = 0x33; c.r[4] = 0x1F00; c.r[5] = 0x55; c.r[6] = 0x66; c.r[7] = 0x77;
        c.flags.cf = bits & 1; c.flags.zf = (bits >> 1) & 1; c.flags.sf = (bits >> 2) & 1;
        c.flags.of = (bits >> 3) & 1; c.flags.pf = (bits >> 4) & 1; c.flags.af = (bits >> 5) & 1;
        table[f].fn(&c);
        uint64_t h = 1469598103934665603ull;
        for (unsigned i = 0; i < MEM_BYTES; i++) { h ^= MEM[i]; h *= 1099511628211ull; }
        printf("%u:", f);
        for (int i = 0; i < 8; i++) printf(" %08x", c.r[i]);
        printf(" fl=%u%u%u%u%u%u mem=%016llx\n", c.flags.cf, c.flags.zf, c.flags.sf, c.flags.of, c.flags.pf, c.flags.af,
               (unsigned long long)h);
    }
    return 0;
}
"""


@unittest.skipUnless(VENDOR.is_dir() and shutil.which("git") and all(shutil.which(t) for t in TOOLS) and
                     os.name == "posix" and platform.machine().lower() in ("x86_64", "amd64") and
                     all(p.is_file() for p in NEW), "pinned translator checkout, x86_64 GNU tools and patches are needed")
@unittest.skipUnless(lifter_python(), "translator Python environment (capstone, pefile) is needed")
class MemoryBaseAtomicRmwTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        python = lifter_python()
        work = base / "work"
        work.mkdir()
        functions, code, cls.addresses = assemble(work)
        synthetic_pe(work, functions, code)
        cls.out = {}
        for label, patches in (("old", OLD), ("new", NEW), ("old-min", MINIMAL), ("new-min", MINIMAL_NEW)):
            tree = build_tree(base / ("tree-" + label), patches)
            out = base / ("out-" + label)
            lift(python, tree, work, out)
            cls.out[label] = (tree, out)
        rng = random.Random(0x10)
        rows = []
        names = list(FUNCTIONS)
        for index, name in enumerate(names):
            for n in range(96):
                base_address = rng.choice((0x100, 0x104, 0x200, 0x1001, 0x1003, 0x17fe))
                eax = rng.choice((0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff,
                                  rng.getrandbits(32)))
                edx = rng.choice((0, 1, 2, 0x10, 0xffffffff, 0x80000000, rng.getrandbits(32) & 0x3f))
                rows.append("%d %x %x %x %x %d\n" % (index, eax, base_address, edx, rng.getrandbits(6), rng.getrandbits(16)))
        (base / "vectors.txt").write_text("".join(rows))
        table = ", ".join('{"%s", f_%X}' % (n, cls.addresses[n]) for n in names)
        cls.results = {}
        for label, (tree, out) in cls.out.items():
            (out / "driver.c").write_text(DRIVER.replace("TABLE", table).replace("MEM_BYTES", str(MEM_BYTES)))
            sources = sorted(out.glob("exe_[0-9]*.c"))
            run(["gcc", "-O1", "-fno-strict-aliasing", "-fwrapv", "-w", "-DLB_STRICT_ORDER", "-I", tree / "runtime", "-I", out,
                 out / "driver.c", *sources, "-lm", "-o", out / "driver"])
            cls.results[label] = run([out / "driver", base / "vectors.txt"]).stdout.decode().splitlines()
        cls.names = names

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def body(self, label, name):
        out = self.out[label][1]
        text = "\n".join(p.read_text() for p in sorted(out.glob("exe_[0-9]*.c")))
        start = text.index("void f_%X(CPU *c) {" % self.addresses[name])
        end = text.index("\n}\n", start) + 2
        if text.startswith("\n#undef MEM", end):
            end += len("\n#undef MEM")
        return text[start:end]

    def test_every_vector_agrees_between_old_and_new_translation(self):
        for before, after in (("old", "new"), ("old-min", "new-min")):
            self.assertEqual(len(self.results[before]), len(self.results[after]))
            self.assertEqual(len(self.results[before]), len(FUNCTIONS) * 96)
            for old, new in zip(self.results[before], self.results[after]):
                self.assertTrue(old == new, "%s: %s != %s" % (self.names[int(old.split(":")[0])], old, new))
        # and the two patch sets agree with each other: 0008/0009 do not matter for these instructions
        self.assertEqual(self.results["old"], self.results["old-min"])

    def test_vectors_exercise_changes(self):
        # The comparison must not be vacuous: memory contents and flags actually differ between vectors.
        rows = [r for r in self.results["new"] if r.startswith("%d:" % self.names.index("lock_add32_reg"))]
        self.assertGreater(len({r.split("mem=")[1] for r in rows}), 20)
        self.assertGreater(len({r.split("fl=")[1].split()[0] for r in rows}), 3)

    def test_supported_lock_operations_become_one_atomic_instruction(self):
        for name in FUNCTIONS:
            if not re.fullmatch(r"lock_(add|sub|and|or|xor|inc|dec)\w*", name):
                continue
            old, new = self.body("old", name), self.body("new", name)
            self.assertIn("do {", old, name)
            self.assertNotIn("do {", new, name)
            self.assertNotIn("compare_exchange", new, name)
            self.assertRegex(new, r"__atomic_fetch_(add|sub|and|or|xor)\(", name)
            self.assertIn("FULL_FENCE();", new, name)  # the surrounding full fences are kept

    def test_other_lock_operations_keep_the_compare_exchange_loop(self):
        drop = ("    uint8_t *const mem_ = MEM;", "#define MEM mem_", "#undef MEM")
        for name in ("lock_adc32", "lock_neg32", "lock_not32"):
            new = "\n".join(l for l in self.body("new", name).splitlines() if l not in drop)
            self.assertTrue(self.body("old", name) == new, name)
            self.assertIn("do {", new, name)

    def test_function_binds_the_guest_base_once_and_restores_the_global(self):
        for name in ("chain", "lock_add32_reg"):
            body = self.body("new", name)
            lines = body.splitlines()
            self.assertEqual(lines[1], "    uint8_t *const mem_ = MEM;")
            self.assertEqual(lines[2], "#define MEM mem_")
            self.assertEqual(lines[-1], "#undef MEM")
            self.assertEqual(body.count("uint8_t *const mem_"), 1)
            self.assertNotIn("mem_", self.body("old", name))
        # The global is visible again between functions: the next function starts with its own binding.
        text = "\n".join(p.read_text() for p in sorted(self.out["new"][1].glob("exe_[0-9]*.c")))
        self.assertEqual(text.count("#define MEM mem_"), text.count("#undef MEM"))

    def test_switches_disable_each_change(self):
        python = lifter_python()
        tree, _ = self.out["new"]
        work = Path(self.temp.name) / "work"
        for key, env in (("memlocal", {"LIFT_MEMLOCAL": "0"}), ("rmw", {"LIFT_LOCK_RMW": "0"})):
            out = Path(self.temp.name) / ("switch-" + key)
            out.mkdir()
            subprocess.run([python, "-E", "-s", "-B", str(tree / "tools/lift.py"), str(work / "synthetic.exe"),
                            str(work / "functions.json"), str(out), "exe"], cwd=tree, check=True, capture_output=True,
                           env=dict(os.environ, LIFT_GAME="skyrim", LIFT_TSO="1", LIFT_MEMORDER="1", LIFT_FILES="1", **env))
            text = "\n".join(p.read_text() for p in sorted(out.glob("exe_[0-9]*.c")))
            start = text.index("void f_%X(CPU *c) {" % self.addresses["lock_add32_reg"])
            body = text[start:text.index("\n}\n", start)]
            if key == "memlocal":
                self.assertTrue("mem_" not in text and "#define MEM" not in text)
                self.assertTrue("__atomic_fetch_add" in body)
            else:
                self.assertTrue("do {" in body and "__atomic_fetch_add" not in body)
                self.assertTrue("mem_" in text)


if __name__ == "__main__":
    unittest.main()
