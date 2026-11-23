"""Patch 0008 (call-flag traffic): flags carried across calls, against real x86_64 execution.

Builds two fresh translator trees from the pinned archive (patches 0001-0007, and 0001-0008), assembles small
synthetic programs twice from the same text (32-bit code for the translator, 64-bit code run natively), lifts the
32-bit code with both trees, and compares result registers and the six arithmetic flags of every run with the
native run, for all 64 seed-flag combinations and several operand values. Also checks that the patched output
really contains the reduced SPILLF/RELOADF forms the analysis is expected to find (so the test is not vacuous)
and that the unpatched output contains none. No game data, no dependency install; skipped when the approved
translator Python environment or the GNU tools (as, ld, nm, objcopy, gcc) are absent.
(tests/test_call_flags.py is the older test of patch 0005.)
"""
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor/skydream"
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
PATCHES = ROOT / "patches/skydream"
LOCK = json.loads((ROOT / "source-lock.json").read_text())
SOURCE = next(s for s in LOCK["sources"] if s["name"] == "skydream")
LOCKED = [ROOT / p["path"] for p in SOURCE["patches"]
          if b"desktop/CMakeLists.txt" not in (ROOT / p["path"]).read_bytes()]
PATCH_0007 = PATCHES / "0007-skip-fences-implied-by-strict-order.patch"
# CALL_FLAG_TRAFFIC_PATCH points the test at a deliberately broken copy of patch 0008 (mutation check that this
# test really detects a wrong analysis); it is not used in normal runs.
PATCH_0008 = Path(os.environ.get("CALL_FLAG_TRAFFIC_PATCH") or PATCHES / "0008-reduce-call-flag-traffic.patch")
OLD_PATCHES = LOCKED + [p for p in (PATCH_0007,) if p not in LOCKED]
NEW_PATCHES = OLD_PATCHES + [p for p in (PATCH_0008,) if p not in OLD_PATCHES]
CANDIDATES = [os.environ.get("SKYRIM_LIFTER_PYTHON", ""),
              "/mnt/f/SkyrimIOS/private/tooling/skydream-py312-20261112/bin/python3",
              str(ROOT.parents[1] / "private/tooling/skydream-py312-20261112/bin/python3")]
TOOLS = ("as", "ld", "nm", "objcopy", "gcc")
SEEDS = [sum(v for b, v in enumerate((0x1, 0x4, 0x10, 0x40, 0x80, 0x800)) if i & (1 << b)) for i in range(64)]
VALUES = (0, 1, 5, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
PER_PROGRAM = len(SEEDS) * len(VALUES) ** 2

# Programs. A program is {function: [block, ...]}, a block is a list of instructions. `@fn` is the label of the
# function's first block, `@fn.N` the label of its block N (jump targets must be block starts). `%IND(@fn)` is an
# indirect call (assembled differently for the 32-bit and the 64-bit copy). "main" is the entry. `af` says whether
# the auxiliary-carry flag is architecturally defined at the end (logic instructions leave it undefined).
# `expect` lists strings the patched translator output of the whole program must contain (checked against my own
# derivation of each analysis result), `absent` strings it must not contain.
PROGRAMS = {
    "pass_through": dict(af=True, funcs={
        "main": [["cmp eax, ecx", "call @g", "jz @main.2"], ["mov eax, 1", "ret"], ["mov eax, 2", "ret"]],
        "g": [["mov edx, 5", "ret"]]},
        expect=[], absent=["SPILLF(", "RELOADF("]),      # g passes every flag through: nothing may be dropped
    "carry_from_callee": dict(af=True, funcs={
        "main": [["cmp eax, ecx", "call @g", "jc @main.2"], ["mov eax, 1", "ret"], ["mov eax, 2", "ret"]],
        "g": [["stc", "ret"]]},
        expect=["SPILLF(62);"], absent=[]),             # g writes cf on every path: cf is not saved before the call
    "callee_kills_all": dict(af=True, funcs={
        "main": [["stc", "mov edx, ecx", "call @g", "jc @main.3"], ["jz @main.4"], ["mov eax, 1", "ret"],
                 ["mov eax, 2", "ret"], ["mov eax, 3", "ret"]],
        "g": [["add edx, 1", "ret"]]},
        expect=["SPILLF(0);"], absent=[]),
    "dead_after_call": dict(af=True, funcs={
        "main": [["cmp eax, ecx", "call @g", "add eax, 0", "jz @main.2"], ["mov eax, 1", "ret"],
                 ["mov eax, 2", "ret"]],
        "g": [["sub edx, 1", "ret"]]},
        expect=["SPILLF(0);", "RELOADF(0);"], absent=[]),
    "inc_partial": dict(af=True, funcs={
        "main": [["cmp eax, ecx", "call @g", "jc @main.3"], ["jz @main.4"], ["mov eax, 1", "ret"],
                 ["mov eax, 2", "ret"], ["mov eax, 3", "ret"]],
        "g": [["inc edx", "ret"]]},
        expect=["SPILLF(1);"], absent=[]),              # inc leaves cf alone, so cf must reach the callee
    "callee_reads_carry": dict(af=True, funcs={
        "main": [["cmp eax, ecx", "call @g", "jz @main.2"], ["mov edx, 9", "ret"], ["mov edx, 8", "ret"]],
        "g": [["jc @g.2"], ["mov eax, 1", "ret"], ["mov eax, 2", "ret"]]},
        expect=[], absent=["SPILLF("]),                 # g reads cf, and passes the others through to the exit
    "conditional_kill": dict(af=True, funcs={
        "main": [["stc", "call @g", "jc @main.2"], ["mov eax, 1", "ret"], ["mov eax, 2", "ret"]],
        "g": [["jecxz @g.2"], ["add edx, 1", "ret"], ["ret"]]},
        expect=[], absent=["SPILLF("]),                 # with ecx == 0 the flags pass straight through g
    "recursion": dict(af=True, funcs={
        "main": [["stc", "mov edx, 3", "call @g", "jc @main.2"], ["mov eax, 1", "ret"], ["mov eax, 2", "ret"]],
        "g": [["cmp edx, 0", "jz @g.2"], ["dec edx", "call @g", "ret"], ["ret"]]},
        expect=["SPILLF(0);"], absent=[]),
    "tail_jump": dict(af=True, funcs={
        "main": [["cmp eax, ecx", "jmp @g"]],
        "g": [["inc edx", "ret"]]},
        expect=["SPILLF(1);"], absent=[]),
    "adc_in_callee": dict(af=True, funcs={
        "main": [["stc", "call @g", "ret"]],
        "g": [["adc eax, ecx", "ret"]]},
        expect=["SPILLF(1);"], absent=[]),
    "indirect_call": dict(af=True, funcs={
        "main": [["cmp eax, ecx", "%IND(@g)", "jz @main.2"], ["mov eax, 1", "ret"], ["mov eax, 2", "ret"]],
        "g": [["add edx, 1", "ret"]]},
        expect=[], absent=["SPILLF("]),                 # an indirect call keeps the full save
    "two_calls": dict(af=True, funcs={
        "main": [["cmp eax, ecx", "call @g1", "call @g2", "jz @main.2"], ["mov eax, 1", "ret"], ["mov eax, 2", "ret"]],
        "g1": [["add edx, 1", "ret"]], "g2": [["mov edx, 3", "ret"]]},
        expect=["SPILLF(0);"], absent=[]),
    "logic_in_callee": dict(af=False, funcs={
        "main": [["stc", "call @g", "jc @main.2"], ["mov eax, 1", "ret"], ["mov eax, 2", "ret"]],
        "g": [["test ecx, ecx", "ret"]]},
        expect=["SPILLF(32);"], absent=[]),            # test writes cf zf sf of pf but not af
}


def lifter_python():
    for candidate in CANDIDATES:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def label(program, name, block=0):
    return "%s_%s" % (program, name) + ("" if block == 0 else "__%d" % block)


def program_text(program, spec, bits):
    """GNU as source (Intel syntax) of one program: functions in a fixed order, every label global."""
    out = []
    for name in spec["funcs"]:
        out.append(".balign 16")
        for n, block in enumerate(spec["funcs"][name]):
            out.append(".globl %s\n%s:" % (label(program, name, n), label(program, name, n)))
            for insn in block:
                insn = re.sub(r"@(\w+)(?:\.(\d+))?", lambda m: label(program, m.group(1), int(m.group(2) or 0)), insn)
                m = re.fullmatch(r"%IND\((.+)\)", insn)
                if m:
                    # esi/rsi holds the pointer: it is scratch in the native wrapper, and an address that differs
                    # between the 32-bit image and the native PIE must not end up in a compared register
                    insn = ("mov esi, offset %s\ncall esi" % m.group(1)) if bits == 32 else \
                        ("lea rsi, [rip + %s]\ncall rsi" % m.group(1))
                out.append("    " + insn.replace("\n", "\n    "))
        out.append(".globl %s__end\n%s__end:" % (label(program, name), label(program, name)))
    return "\n".join(out) + "\n"


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
    mov edx, 0
    push rdi
    popfq
    call {entry}
    pushfq
    pop r10
    mov [r9], r10
    mov [r9 + 8], rdx
    push r11
    popfq
    ret
""".format(i=index, entry=label(program, "main"))


def run(command, cwd=None, **kw):
    return subprocess.run([str(c) for c in command], cwd=cwd, check=True, capture_output=True, timeout=600, **kw)


def build_programs(base):
    """Assemble, link and describe all programs. Returns (functions.json content, code bytes, program records)."""
    source32 = [".intel_syntax noprefix", ".text"]
    source64 = [".intel_syntax noprefix", ".text"]
    for index, (program, spec) in enumerate(PROGRAMS.items()):
        source32.append(program_text(program, spec, 32))
        source64.append(program_text(program, spec, 64))
        source64.append(native_wrapper(index, program))
    (base / "p32.s").write_text("\n".join(source32))
    (base / "p64.s").write_text("\n".join(source64))
    run(["as", "--32", "-o", "p32.o", "p32.s"], cwd=base)
    run(["ld", "-m", "elf_i386", "-Ttext=0x401000", "-e", "0x401000", "-o", "p32.elf", "p32.o"], cwd=base)
    run(["objcopy", "-O", "binary", "-j", ".text", "p32.elf", "p32.bin"], cwd=base)
    run(["as", "--64", "-o", "p64.o", "p64.s"], cwd=base)
    symbols = {}
    for line in run(["nm", "p32.elf"], cwd=base).stdout.decode().splitlines():
        address, kind, name = line.split()
        symbols[name] = int(address, 16)
    functions, records = {}, []
    for index, (program, spec) in enumerate(PROGRAMS.items()):
        addresses = []
        for name, blocks in spec["funcs"].items():
            starts = [symbols[label(program, name, n)] for n in range(len(blocks))]
            ends = starts[1:] + [symbols[label(program, name) + "__end"]]
            functions[hex(starts[0])] = [[a, b] for a, b in zip(starts, ends)]
            addresses.append(starts[0])
        records.append({"index": index, "program": program, "main": symbols[label(program, "main")],
                        "af": spec["af"], "addresses": addresses})
    return functions, (base / "p32.bin").read_bytes(), records


def synthetic_pe(directory, functions, code):
    size = (len(code) + 511) // 512 * 512
    assert size <= 0x1000, "test code too large for the one-page synthetic section"
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


def lift(python, tree, work, out, **extra_env):
    out.mkdir()
    env = dict(os.environ, LIFT_GAME="skyrim", LIFT_TSO="1", LIFT_MEMORDER="1", LIFT_FILES="1", **extra_env)
    subprocess.run([python, "-E", "-s", "-B", str(tree / "tools/lift.py"), str(work / "synthetic.exe"),
                    str(work / "functions.json"), str(out), "exe"], cwd=tree, env=env, check=True,
                   capture_output=True, timeout=300)
    rejected = json.loads((out / "exe_rejected.json").read_text())
    if rejected:
        raise RuntimeError("synthetic functions rejected: %s" % rejected)


def program_table(records):
    lines = ["typedef uint64_t (*nat_fn)(uint64_t, uint32_t, uint32_t, uint64_t *);"]
    for r in records:
        lines.append("extern uint64_t nat_prog_%d(uint64_t, uint32_t, uint32_t, uint64_t *);" % r["index"])
    lines.append("static const struct { nat_fn nat; GuestFn lifted; } programs[] = {")
    for r in records:
        lines.append("    { nat_prog_%d, f_%X }," % (r["index"], r["main"]))
    lines.append("};")
    return "\n".join(lines) + "\n"


def vectors(records):
    rows, n = [], 0
    for r in records:
        for seed in SEEDS:
            for a in VALUES:
                for c in VALUES:
                    rows.append("%d %d %x %x %x\n" % (n, r["index"], seed, a, c))
                    n += 1
    return rows


def body_of(out, address):
    text = "\n".join(p.read_text() for p in sorted(out.glob("exe_*.c")))
    start = text.index("void f_%X(CPU *c) {" % address)
    return text[start:text.index("\n}\n", start)]


@unittest.skipUnless(VENDOR.is_dir() and PATCH_0007.is_file() and PATCH_0008.is_file(),
                     "translator checkout and patches 0007/0008 are needed")
@unittest.skipUnless(lifter_python(), "approved translator Python environment (capstone, pefile) is needed")
@unittest.skipUnless(all(shutil.which(t) for t in TOOLS), "GNU as/ld/nm/objcopy/gcc are needed")
@unittest.skipUnless(os.name == "posix" and os.uname().machine == "x86_64", "native x86_64 execution is needed")
class CallFlagTrafficTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        keep = os.environ.get("CALL_FLAG_TRAFFIC_WORK")   # keep every intermediate file (debugging, evidence)
        if keep:
            cls.tmp = None
            base = Path(tempfile.mkdtemp(prefix="call-flag-traffic-", dir=keep))
        else:
            cls.tmp = tempfile.TemporaryDirectory(prefix="call-flag-traffic-")
            base = Path(cls.tmp.name)
        python = lifter_python()
        functions, code, cls.records = build_programs(base)
        synthetic_pe(base, functions, code)
        cls.trees = {"old": build_tree(base / "old", OLD_PATCHES), "new": build_tree(base / "new", NEW_PATCHES)}
        cls.out = {}
        for name, tree in cls.trees.items():
            cls.out[name] = base / ("emitted-" + name)
            lift(python, tree, base, cls.out[name],
                 **({"LIFT_CALLFLAGS_REPORT": str(base / "flags-report.json")} if name == "new" else {}))
        cls.report = base / "flags-report.json"
        # the patched translator with its documented off switch must reproduce the unpatched text exactly
        cls.out["new-off"] = base / "emitted-new-off"
        lift(python, cls.trees["new"], base, cls.out["new-off"], LIFT_CALLFLAGS="0")
        (base / "vectors.txt").write_text("".join(vectors(cls.records)))
        cls.rows = {}
        for name, tree in cls.trees.items():
            emitted = cls.out[name]
            (emitted / "program_table.inc").write_text(program_table(cls.records))
            for opt in ("O0", "O2"):
                exe = base / ("flags-%s-%s" % (name, opt))
                run(["gcc", "-" + opt, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                     "-isystem", tree / "runtime", "-I", emitted, ROOT / "tests/native_call_flags.c",
                     emitted / "exe_00.c", emitted / "exe_table.c", base / "p64.o", "-lm", "-o", exe], cwd=base)
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
            mask = 0x8d5 if program["af"] else 0x8c5
            if ((row["native_flags"] ^ row["lifted_flags"]) & mask or row["native_eax"] != row["lifted_eax"]
                    or row["native_edx"] != row["lifted_edx"] or row["lifted_esp"] != 0x8004):
                bad.append(dict(row, program=program["program"]))
        self.assertEqual(bad[:5], [], "%d of %d vectors differ from native execution" % (len(bad), len(rows)))

    def test_unpatched_matches_native_o0(self):
        self.compare("old", "O0")

    def test_unpatched_matches_native_o2(self):
        self.compare("old", "O2")

    def test_patched_matches_native_o0(self):
        self.compare("new", "O0")

    def test_patched_matches_native_o2(self):
        self.compare("new", "O2")

    def test_reduced_forms_are_present_where_expected(self):
        for r in self.records:
            spec = PROGRAMS[r["program"]]
            text = "".join(body_of(self.out["new"], a) for a in r["addresses"])
            old_text = "".join(body_of(self.out["old"], a) for a in r["addresses"])
            for want in spec["expect"]:
                self.assertIn(want, text, r["program"])
            for unwanted in spec["absent"]:
                self.assertNotIn(unwanted, text, r["program"])
            self.assertNotIn("SPILLF(", old_text)
            self.assertNotIn("RELOADF(", old_text)

    def test_off_switch_reproduces_the_unpatched_text(self):
        for old, off in zip(sorted(self.out["old"].iterdir()), sorted(self.out["new-off"].iterdir())):
            self.assertEqual(old.name, off.name)
            self.assertEqual(old.read_bytes(), off.read_bytes(), old.name)
        self.assertFalse((self.out["new"] / "exe_flags.json").exists())   # the report never lands in the tree
        self.assertFalse(any(p.suffix == ".json" and p.name != "exe_rejected.json" for p in self.out["new"].iterdir()))
        analysis = json.loads(self.report.read_text())
        self.assertEqual(analysis["stats"]["skipped_shape"], 0)

    def test_only_spill_and_reload_lines_change(self):
        old = "\n".join(p.read_text() for p in sorted(self.out["old"].glob("exe_*.c"))).split("\n")
        new = "\n".join(p.read_text() for p in sorted(self.out["new"].glob("exe_*.c"))).split("\n")
        self.assertEqual(len(old), len(new))
        changed = 0
        for a, b in zip(old, new):
            if a != b:
                changed += 1
                self.assertEqual(re.sub(r"(SPILL|RELOAD)F\(\d+\);", r"\1();", b), a)
        self.assertGreater(changed, 0)


if __name__ == "__main__":
    unittest.main()
