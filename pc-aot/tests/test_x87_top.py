"""Patch 0009 (x87 stack position in a local): translated x87 code against real x86_64 execution.

Builds two fresh translator trees from the pinned archive (patches 0001-0008, and 0001-0009), assembles small x87 test
programs twice from the same text (32-bit code for the translator, 64-bit code run natively), lifts the 32-bit code with
both trees, and compares what each program leaves in its output buffer, eax, the status flags and the x87 stack
position with the native run. Also checks that the patched output really uses `top_` where expected and that only the
x87 lines differ from the unpatched text. Operands are small dyadic doubles, so the translator's 64-bit doubles and the
CPU's 80-bit registers agree exactly. No game data, no dependency install; skipped when the approved translator Python
environment or the GNU tools are absent.
"""
import json
import os
from pathlib import Path
import random
import re
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_call_flag_traffic as T  # noqa: E402  (shared translator-tree and assembler helpers)

ROOT = T.ROOT
PATCH_0009 = Path(os.environ.get("X87_TOP_PATCH") or T.PATCHES / "0009-keep-x87-top-in-a-local.patch")
OLD_PATCHES = T.NEW_PATCHES                     # 0001-0008
NEW_PATCHES = OLD_PATCHES + [p for p in (PATCH_0009,) if p not in OLD_PATCHES]
VALUES = (1.0, -1.0, 2.0, 0.5, 3.25, -7.5, 100.0, 0.25, 8.0)
M = ["[edx]", "[edx+8]", "[edx+16]", "[edx+24]"]            # inputs
OUT = ["[edx+64]", "[edx+72]", "[edx+80]", "[edx+88]"]       # outputs
FLAGS_NONE, FLAGS_COMPARE = 0, 0x45                          # which status flags the program defines
EAX_NONE, EAX_RESULT, EAX_STATUS = 0, 0xFFFFFFFF, 0x7D00     # which bits of eax mean something

# name: dict(funcs=..., flags=mask, eax=mask, expect=[strings the patched output must contain])
# Instruction lists use the syntax of test_call_flag_traffic (`@fn`, `@fn.N` labels). Every program leaves the x87 stack
# empty. `mov eax, N` marks which branch ran.
PROGRAMS = {
    "add": dict(flags=0, eax=0, funcs={"main": [[
        "fld qword ptr " + M[0], "fld qword ptr " + M[1], "faddp st(1), st", "fstp qword ptr " + OUT[0], "ret"]]}),
    "square_minus": dict(flags=0, eax=0, funcs={"main": [[
        "fld qword ptr " + M[0], "fld st(0)", "fmulp st(1), st", "fld qword ptr " + M[1], "fsubp st(1), st",
        "fstp qword ptr " + OUT[0], "ret"]]}),
    "fxch_chain": dict(flags=0, eax=0, funcs={"main": [[
        "fld qword ptr " + M[0], "fld qword ptr " + M[1], "fld qword ptr " + M[2], "fxch st(2)", "fadd st, st(1)",
        "fxch st(1)", "fstp qword ptr " + OUT[0], "fstp qword ptr " + OUT[1], "fstp qword ptr " + OUT[2], "ret"]]}),
    "compare_branch": dict(flags=FLAGS_COMPARE, eax=EAX_RESULT, funcs={"main": [
        ["fld qword ptr " + M[0], "fld qword ptr " + M[1], "fcomip st, st(1)", "fstp st(0)", "jbe @main.2"],
        ["fld qword ptr " + M[2], "fstp qword ptr " + OUT[0], "mov eax, 1", "ret"],
        ["fld qword ptr " + M[3], "fstp qword ptr " + OUT[0], "mov eax, 2", "ret"]]}),
    "ucompare_parity": dict(flags=FLAGS_COMPARE, eax=EAX_RESULT, funcs={"main": [
        ["fld qword ptr " + M[1], "fld qword ptr " + M[0], "fucomip st, st(1)", "fstp st(0)", "jp @main.2"],
        ["fld qword ptr " + M[2], "fstp qword ptr " + OUT[0], "mov eax, 1", "ret"],
        ["mov eax, 3", "ret"]]}),
    "across_blocks": dict(flags=0, eax=0, funcs={"main": [
        ["fld qword ptr " + M[0], "fld qword ptr " + M[1], "jecxz @main.2"],
        ["faddp st(1), st", "jmp @main.3"],
        ["fmulp st(1), st"],
        ["fstp qword ptr " + OUT[0], "ret"]]}),
    "callee_consumes": dict(flags=0, eax=0, funcs={
        "main": [["fld qword ptr " + M[0], "fld qword ptr " + M[1], "call @g", "fstp qword ptr " + OUT[0], "ret"]],
        "g": [["fmulp st(1), st", "ret"]]}),
    "callee_produces": dict(flags=0, eax=0, funcs={
        "main": [["fld qword ptr " + M[0], "call @g", "fstp qword ptr " + OUT[0], "ret"]],
        "g": [["fld qword ptr " + M[1], "faddp st(1), st", "ret"]]}),
    "status_word": dict(flags=0, eax=EAX_STATUS, funcs={"main": [[
        "fld qword ptr " + M[0], "fld qword ptr " + M[1], "fcom st(1)", "fnstsw ax", "fstp st(0)", "fstp st(0)", "ret"]]}),
    "status_after_pushes": dict(flags=0, eax=EAX_STATUS, funcs={"main": [[
        "fld qword ptr " + M[0], "fld qword ptr " + M[1], "fld qword ptr " + M[2], "fnstsw ax", "fstp st(0)",
        "fstp st(0)", "fstp st(0)", "ret"]]}),
    "incstp_decstp": dict(flags=0, eax=0, funcs={"main": [[
        "fld qword ptr " + M[0], "fdecstp", "fincstp", "fstp qword ptr " + OUT[0], "ret"]]}),
    "ninit": dict(flags=0, eax=0, funcs={"main": [[
        "fld qword ptr " + M[0], "fld qword ptr " + M[1], "fninit", "fld qword ptr " + M[2], "fstp qword ptr " + OUT[0],
        "ret"]]}),
    "int_round_trip": dict(flags=0, eax=0, funcs={"main": [[
        "mov dword ptr [edx+48], eax", "fild dword ptr [edx+48]", "fld qword ptr " + M[0], "faddp st(1), st",
        "fistp dword ptr [edx+96]", "ret"]]}),
    "const_chs_abs": dict(flags=0, eax=0, funcs={"main": [[
        "fld1", "fldz", "faddp st(1), st", "fchs", "fabs", "fld qword ptr " + M[0], "fmulp st(1), st",
        "fstp qword ptr " + OUT[0], "ret"]]}),
    "store_then_load": dict(flags=0, eax=0, funcs={"main": [[
        "fld qword ptr " + M[0], "fstp qword ptr " + M[1], "fld qword ptr " + M[1], "fadd qword ptr " + M[2],
        "fstp qword ptr " + OUT[0], "ret"]]}),
    "four_deep": dict(flags=0, eax=0, funcs={"main": [[
        "fld qword ptr " + M[0], "fld qword ptr " + M[1], "fld qword ptr " + M[2], "fld qword ptr " + M[3],
        "faddp st(1), st", "faddp st(1), st", "faddp st(1), st", "fstp qword ptr " + OUT[0], "ret"]]}),
    "fxam_status": dict(flags=0, eax=EAX_STATUS, funcs={"main": [[
        "fld qword ptr " + M[0], "fxam", "fnstsw ax", "fstp st(0)", "ret"]]}),
    "rem": dict(flags=0, eax=0, funcs={"main": [[
        "fld qword ptr " + M[1], "fld qword ptr " + M[0], "fprem", "fstp qword ptr " + OUT[0], "fstp st(0)", "ret"]]}),
    "nested_calls": dict(flags=0, eax=0, funcs={
        "main": [["fld qword ptr " + M[0], "call @g", "call @h", "fstp qword ptr " + OUT[0], "ret"]],
        "g": [["fld qword ptr " + M[1], "fmulp st(1), st", "ret"]],
        "h": [["fld qword ptr " + M[2], "faddp st(1), st", "fld st(0)", "fmulp st(1), st", "ret"]]}),
    "branch_in_loop_body": dict(flags=FLAGS_COMPARE, eax=EAX_RESULT, funcs={"main": [
        ["fld qword ptr " + M[0], "fld qword ptr " + M[1], "fadd st, st(1)", "fld qword ptr " + M[2], "fcomip st, st(1)",
         "jb @main.2"],
        ["fmulp st(1), st", "fstp qword ptr " + OUT[0], "mov eax, 1", "ret"],
        ["faddp st(1), st", "fstp qword ptr " + OUT[1], "mov eax, 2", "ret"]]}),
}
for _name, _spec in PROGRAMS.items():
    _spec.setdefault("af", True)
    _spec.setdefault("expect", [])
    _spec.setdefault("absent", [])


def native_wrapper(index, program):
    return """
.balign 16
.globl nat_prog_{i}
nat_prog_{i}:
    pushfq
    pop r11
    mov r9, rcx
    mov eax, esi
    mov ecx, edx
    mov rdx, rdi
    push 0x202
    popfq
    call {entry}
    pushfq
    pop r10
    mov [r9], r10
    fnstsw word ptr [r9 + 8]
    push r11
    popfq
    ret
""".format(i=index, entry=T.label(program, "main"))


def build_programs(base):
    source32, source64 = [".intel_syntax noprefix", ".text"], [".intel_syntax noprefix", ".text"]
    for index, (program, spec) in enumerate(PROGRAMS.items()):
        source32.append(T.program_text(program, spec, 32))
        text64 = T.program_text(program, spec, 64)
        source64.append(re.sub(r"\[edx", "[rdx", text64))
        source64.append(native_wrapper(index, program))
    (base / "p32.s").write_text("\n".join(source32))
    (base / "p64.s").write_text("\n".join(source64))
    T.run(["as", "--32", "-o", "p32.o", "p32.s"], cwd=base)
    T.run(["ld", "-m", "elf_i386", "-Ttext=0x401000", "-e", "0x401000", "-o", "p32.elf", "p32.o"], cwd=base)
    T.run(["objcopy", "-O", "binary", "-j", ".text", "p32.elf", "p32.bin"], cwd=base)
    T.run(["as", "--64", "-o", "p64.o", "p64.s"], cwd=base)
    symbols = {}
    for line in T.run(["nm", "p32.elf"], cwd=base).stdout.decode().splitlines():
        address, kind, name = line.split()
        symbols[name] = int(address, 16)
    functions, records = {}, []
    for index, (program, spec) in enumerate(PROGRAMS.items()):
        addresses = []
        for name, blocks in spec["funcs"].items():
            starts = [symbols[T.label(program, name, n)] for n in range(len(blocks))]
            ends = starts[1:] + [symbols[T.label(program, name) + "__end"]]
            functions[hex(starts[0])] = [[a, b] for a, b in zip(starts, ends)]
            addresses.append(starts[0])
        records.append({"index": index, "program": program, "main": symbols[T.label(program, "main")],
                        "flags": spec["flags"], "eax": spec["eax"], "addresses": addresses})
    return functions, (base / "p32.bin").read_bytes(), records


def program_table(records):
    lines = ["typedef uint64_t (*nat_fn)(uint64_t *, uint32_t, uint32_t, uint64_t *);"]
    for r in records:
        lines.append("extern uint64_t nat_prog_%d(uint64_t *, uint32_t, uint32_t, uint64_t *);" % r["index"])
    lines.append("static const struct { nat_fn nat; GuestFn lifted; } programs[] = {")
    for r in records:
        lines.append("    { nat_prog_%d, f_%X }," % (r["index"], r["main"]))
    lines.append("};")
    return "\n".join(lines) + "\n"


def bits(value):
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def vectors(records, per_program=48, seed=20261124):
    rng = random.Random(seed)
    rows, n = [], 0
    for r in records:
        for _ in range(per_program):
            values = [rng.choice(VALUES) for _ in range(4)]
            rows.append("%d %d %x %x %x %x %x %x\n" % (n, r["index"], rng.choice((0, 1, 2, 5)), rng.choice((0, 1, 3)),
                                                      *[bits(v) for v in values]))
            n += 1
    return rows


def reverse_x87(line):
    """Undo the 0009 spelling so a patched line can be compared with the unpatched one."""
    line = re.sub(r"\bST87\(", "ST(", line)
    line = line.replace("FPOP87()", "fpop(c)").replace("FSTATUS87()", "fstatus(c)")
    while True:
        k = line.find("FPUSH87(")
        if k < 0:
            return line
        depth = 0
        for j in range(k + len("FPUSH87"), len(line)):
            depth += line[j] == "("
            depth -= line[j] == ")"
            if depth == 0:
                break
        line = line[:k] + "fpush(c, " + line[k + len("FPUSH87("):j] + ")" + line[j + 1:]


PER_PROGRAM = 48


@unittest.skipUnless(T.VENDOR.is_dir() and T.PATCH_0008.is_file() and PATCH_0009.is_file(),
                     "translator checkout and patches 0008/0009 are needed")
@unittest.skipUnless(T.lifter_python(), "approved translator Python environment (capstone, pefile) is needed")
@unittest.skipUnless(all(shutil.which(t) for t in T.TOOLS), "GNU as/ld/nm/objcopy/gcc are needed")
@unittest.skipUnless(os.name == "posix" and os.uname().machine == "x86_64", "native x86_64 execution is needed")
class X87TopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        keep = os.environ.get("X87_TOP_WORK")
        if keep:
            cls.tmp = None
            base = Path(tempfile.mkdtemp(prefix="x87-top-", dir=keep))
        else:
            cls.tmp = tempfile.TemporaryDirectory(prefix="x87-top-")
            base = Path(cls.tmp.name)
        python = T.lifter_python()
        functions, code, cls.records = build_programs(base)
        T.synthetic_pe(base, functions, code)
        cls.trees = {"old": T.build_tree(base / "old", OLD_PATCHES), "new": T.build_tree(base / "new", NEW_PATCHES)}
        cls.out = {}
        for name, tree in cls.trees.items():
            cls.out[name] = base / ("emitted-" + name)
            T.lift(python, tree, base, cls.out[name])
        cls.out["new-off"] = base / "emitted-new-off"
        T.lift(python, cls.trees["new"], base, cls.out["new-off"], LIFT_X87TOP="0")
        (base / "vectors.txt").write_text("".join(vectors(cls.records, PER_PROGRAM)))
        cls.rows = {}
        for name, tree in cls.trees.items():
            emitted = cls.out[name]
            (emitted / "program_table.inc").write_text(program_table(cls.records))
            for opt in ("O0", "O2"):
                exe = base / ("x87-%s-%s" % (name, opt))
                fpu = base / ("fpu-%s-%s.o" % (name, opt))    # the runtime's fxam/fprem/fpu_env/f80 helpers, as in the game
                T.run(["gcc", "-c", "-" + opt, "-std=gnu11", "-w", "-isystem", tree / "runtime", tree / "runtime/fpu.c",
                       "-o", fpu], cwd=base)
                T.run(["gcc", "-" + opt, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                       "-isystem", tree / "runtime", "-I", emitted, ROOT / "tests/native_x87_top.c",
                       emitted / "exe_00.c", emitted / "exe_table.c", fpu, base / "p64.o", "-lm", "-o", exe], cwd=base)
                import subprocess
                result = subprocess.run([str(exe)], input=(base / "vectors.txt").read_bytes(), capture_output=True,
                                        timeout=600, check=True)
                cls.rows[(name, opt)] = [json.loads(l) for l in result.stdout.decode().splitlines()]

    @classmethod
    def tearDownClass(cls):
        if cls.tmp:
            cls.tmp.cleanup()

    def compare(self, name, opt):
        rows = self.rows[(name, opt)]
        self.assertEqual(len(rows), len(PROGRAMS) * PER_PROGRAM)
        bad = []
        for row in rows:
            program = self.records[row["id"] // PER_PROGRAM]
            if (row["native_buffer"] != row["lifted_buffer"] or row["native_top"] != row["lifted_top"]
                    or (row["native_eax"] ^ row["lifted_eax"]) & program["eax"]
                    or (row["native_flags"] ^ row["lifted_flags"]) & program["flags"] or row["lifted_esp"] != 0x8004):
                bad.append(dict(row, program=program["program"]))
        self.assertEqual(bad[:3], [], "%d of %d vectors differ from native execution" % (len(bad), len(rows)))

    def test_unpatched_matches_native_o0(self):
        self.compare("old", "O0")

    def test_unpatched_matches_native_o2(self):
        self.compare("old", "O2")

    def test_patched_matches_native_o0(self):
        self.compare("new", "O0")

    def test_patched_matches_native_o2(self):
        self.compare("new", "O2")

    def text(self, which, record):
        return "".join(T.body_of(self.out[which], a) for a in record["addresses"])

    def test_patched_text_keeps_the_position_in_a_local(self):
        for r in self.records:
            new, old = self.text("new", r), self.text("old", r)
            self.assertIn("uint32_t top_ = 0;", new, r["program"])
            self.assertIn("top_ = c->top;", new, r["program"])
            self.assertNotIn("ST(", new.replace("ST87(", ""), r["program"])
            self.assertNotIn("top_", old, r["program"])

    def test_only_x87_lines_change(self):
        def lines(which):
            return "\n".join(p.read_text() for p in sorted(self.out[which].glob("exe_*.c"))).split("\n")
        old, new = lines("old"), lines("new")
        inserted = {"    top_ = c->top;", "    c->top = top_;", "    uint32_t top_ = 0; (void)top_;"}
        kept = [reverse_x87(l) for l in new if l not in inserted]
        self.assertEqual(kept, old)
        self.assertGreater(len(new), len(old))

    def test_off_switch_reproduces_the_unpatched_text(self):
        for old, off in zip(sorted(self.out["old"].iterdir()), sorted(self.out["new-off"].iterdir())):
            self.assertEqual(old.name, off.name)
            self.assertEqual(old.read_bytes(), off.read_bytes(), old.name)


if __name__ == "__main__":
    unittest.main()
