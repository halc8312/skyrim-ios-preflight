"""Patch 0007 (skip fences implied by strict ordering): the real translator on small synthetic code.

Builds two fresh translator trees from the pinned archive (patches 0001-0006, and 0001-0007), lifts the same
synthetic PE with both, and checks which fence each instruction gets. No game data, no dependency install.
Needs the approved translator Python environment (capstone, pefile); skipped when it is absent.
"""
import difflib
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
# Same selection as scripts/prepare_title_guest.py: a patch that only touches the desktop build is not
# part of the guest-only translator tree.
BASE_PATCHES = [ROOT / p["path"] for p in SOURCE["patches"]
                if b"desktop/CMakeLists.txt" not in (ROOT / p["path"]).read_bytes()]
NEW_PATCH = PATCHES / "0007-skip-fences-implied-by-strict-order.patch"
CANDIDATES = [os.environ.get("SKYRIM_LIFTER_PYTHON", ""),
              "/mnt/f/SkyrimIOS/private/tooling/skydream-py312-20261112/bin/python3",
              str(ROOT.parents[1] / "private/tooling/skydream-py312-20261112/bin/python3")]
FENCES = re.compile(r"^\s*(ST_FENCE_TSO|LD_FENCE_TSO|ST_FENCE|LD_FENCE|FULL_FENCE)\(\);\s*$")

# name: (machine code, expected fences with patch 0007, expected fences without it)
CASES = {
    "store32": ("8901c3", ["ST_FENCE_TSO"], ["ST_FENCE"]),                       # mov [ecx], eax
    "store16": ("668901c3", ["ST_FENCE_TSO"], ["ST_FENCE"]),                      # mov [ecx], ax
    "store8": ("8801c3", ["ST_FENCE_TSO"], ["ST_FENCE"]),                         # mov [ecx], al
    "stack_store": ("89442404c3", [], []),                                        # mov [esp+4], eax: never fenced
    # Without 0007 (existing behaviour): capstone reports the memory operand of fstp m64 and movq m64,mm as READ
    # only, so the translator emits no store fence for these shared stores (and a plain load fence instead).
    # 0007 v2 recognises the plain store from the emitted text and gives it a real ST_FENCE (the load fence stays).
    "x87_store": ("dd19c3", ["ST_FENCE", "LD_FENCE"], ["LD_FENCE"]),              # fstp qword [ecx]: plain MF64
    "mmx_store": ("0f7f01c3", ["ST_FENCE", "LD_FENCE"], ["LD_FENCE"]),            # movq [ecx], mm0: plain M64
    "rep_stos": ("f3abc3", ["ST_FENCE"], ["ST_FENCE"]),                           # string store
    "load32": ("8b01c3", ["LD_FENCE_TSO"], ["LD_FENCE"]),                         # mov eax, [ecx]
    "load16": ("668b01c3", ["LD_FENCE_TSO"], ["LD_FENCE"]),
    "load8": ("8a01c3", ["LD_FENCE_TSO"], ["LD_FENCE"]),
    "stack_load": ("8b442404c3", [], []),
    "x87_load": ("d901c3", ["LD_FENCE"], ["LD_FENCE"]),                           # fld dword [ecx]: plain MF32
    "load_then_x87_load": ("8b01d902c3", ["LD_FENCE"], ["LD_FENCE"]),             # one plain load keeps the fence
    "push_mem": ("ff31c3", ["LD_FENCE"], ["LD_FENCE"]),                           # TL read but a plain stack write
    "rmw_add": ("0101c3", ["ST_FENCE_TSO", "LD_FENCE_TSO"], ["ST_FENCE", "LD_FENCE"]),
    "load_store": ("8b018902c3", ["ST_FENCE_TSO", "LD_FENCE_TSO"], ["ST_FENCE", "LD_FENCE"]),
    "load_then_x87_store": ("8b01dd1ac3", ["ST_FENCE", "LD_FENCE"], ["LD_FENCE"]),  # plain x87 store: now fenced
    "lock_add": ("f00101c3", ["FULL_FENCE", "FULL_FENCE", "LD_FENCE"], ["FULL_FENCE", "FULL_FENCE", "LD_FENCE"]),
    "xchg_mem": ("8701c3", ["FULL_FENCE", "FULL_FENCE", "LD_FENCE"], ["FULL_FENCE", "FULL_FENCE", "LD_FENCE"]),
    "nop": ("90c3", [], []),
    # 0007 v2: every plain store to non-stack guest memory gets a real fence, whatever capstone says about it.
    "x87_store_m32": ("d911c3", ["ST_FENCE", "LD_FENCE"], ["LD_FENCE"]),          # fst dword [ecx]
    "x87_int_store_m32": ("db19c3", ["ST_FENCE", "LD_FENCE"], ["LD_FENCE"]),      # fistp dword [ecx]
    "sse_store": ("0f1101c3", ["ST_FENCE", "LD_FENCE"], ["LD_FENCE"]),            # movups [ecx], xmm0 (memcpy)
    "fpu_control_store": ("d939c3", ["ST_FENCE", "LD_FENCE"], ["LD_FENCE"]),      # fnstcw [ecx]
    "x87_store_to_stack": ("d9542404c3", [], []),                                 # fst dword [esp+4]: stack, no fence
    "x87_load_only": ("db01c3", ["LD_FENCE"], ["LD_FENCE"]),                      # fild dword [ecx]: no store fence
    "bts_mem": ("0fab01c3", ["ST_FENCE", "LD_FENCE"], ["ST_FENCE", "LD_FENCE"]),  # `|=` store: already fenced, unchanged
}


def lifter_python():
    for candidate in CANDIDATES:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def synthetic_pe(directory):
    code = bytearray(b"\xcc" * 1024)
    functions = {}
    for index, (name, (machine, _, _)) in enumerate(CASES.items()):
        body = bytes.fromhex(machine)
        address = 0x401000 + index * 32
        code[index * 32:index * 32 + len(body)] = body
        functions[hex(address)] = [[address, address + len(body)]]
    image = bytearray(1536)
    image[:2] = b"MZ"; struct.pack_into("<I", image, 0x3c, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x84, 0x14c, 1, 0, 0, 0, 224, 0x102)
    optional = 0x98
    struct.pack_into("<H", image, optional, 0x10b)
    for offset, value in {4: 1024, 16: 0x1000, 20: 0x1000, 28: 0x400000, 32: 0x1000, 36: 512, 56: 0x2000,
                          60: 512, 72: 0x100000, 76: 0x1000, 80: 0x100000, 84: 0x1000, 92: 16}.items():
        struct.pack_into("<I", image, optional + offset, value)
    struct.pack_into("<H", image, optional + 68, 3)
    struct.pack_into("<8sIIIIIIHHI", image, optional + 224, b".text\0\0\0", 1024, 0x1000, 1024, 512, 0, 0, 0, 0, 0x60000020)
    image[512:] = code
    (directory / "synthetic.exe").write_bytes(image)
    (directory / "functions.json").write_text(json.dumps({"functions": functions, "iat": {}, "jump_tables": {}}))
    return {name: 0x401000 + i * 32 for i, name in enumerate(CASES)}


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


def lift(python, tree, work, out):
    out.mkdir()
    env = dict(os.environ, LIFT_GAME="skyrim", LIFT_TSO="1", LIFT_MEMORDER="1", LIFT_FILES="1")
    subprocess.run([python, "-E", "-s", "-B", str(tree / "tools/lift.py"), str(work / "synthetic.exe"),
                    str(work / "functions.json"), str(out), "exe"], cwd=tree, env=env, check=True,
                   capture_output=True, timeout=300)


def function_fences(out, address):
    text = "\n".join(p.read_text() for p in sorted(out.glob("exe_*.c")))
    start = text.index("void f_%X(CPU *c) {" % address)
    body = text[start:text.index("\n}\n", start)]
    return [m[1] for m in (FENCES.match(line) for line in body.split("\n")) if m], body


@unittest.skipUnless(VENDOR.is_dir() and NEW_PATCH.is_file(), "translator checkout and patch 0007 are needed")
@unittest.skipUnless(lifter_python(), "approved translator Python environment (capstone, pefile) is needed")
class FencePatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="fence-patch-")
        base = Path(cls.tmp.name)
        python = lifter_python()
        cls.addresses = synthetic_pe(base)
        cls.old = base / "old"
        cls.new = base / "new"
        lift(python, build_tree(base / "old-tree", BASE_PATCHES), base, cls.old)
        cls.patched_tree = build_tree(base / "new-tree", BASE_PATCHES + [NEW_PATCH])
        lift(python, cls.patched_tree, base, cls.new)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_each_instruction_gets_exactly_the_expected_fence(self):
        for name, (_, with_patch, without) in CASES.items():
            with self.subTest(name=name):
                self.assertEqual(function_fences(self.new, self.addresses[name])[0], with_patch)
                self.assertEqual(function_fences(self.old, self.addresses[name])[0], without)

    def test_the_only_differences_are_fence_spellings_and_added_store_fences(self):
        # Two kinds of change are allowed, nothing else: a fence line respelled (ST_FENCE -> ST_FENCE_TSO,
        # LD_FENCE -> LD_FENCE_TSO), and ONE added `ST_FENCE();` line in front of a plain store that capstone
        # had reported as a read (patch 0007 v2).
        for name in CASES:
            with self.subTest(name=name):
                old = [l.strip() for l in function_fences(self.old, self.addresses[name])[1].split("\n")]
                new = [l.strip() for l in function_fences(self.new, self.addresses[name])[1].split("\n")]
                for tag, a0, a1, b0, b1 in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
                    if tag == "equal":
                        continue
                    if tag == "replace":
                        self.assertEqual(a1 - a0, b1 - b0)
                        for a, b in zip(old[a0:a1], new[b0:b1]):
                            self.assertIn((a, b), (("ST_FENCE();", "ST_FENCE_TSO();"), ("LD_FENCE();", "LD_FENCE_TSO();")))
                    else:
                        self.assertEqual((tag, new[b0:b1]), ("insert", ["ST_FENCE();"]))

    def test_prelude_keeps_the_real_fence_unless_strict_order_on_aarch64(self):
        prelude = (self.patched_tree / "runtime/lift_prelude.h").read_text()
        match = re.search(r"#if defined\(__aarch64__\) && defined\(LB_STRICT_ORDER\)\n#define ST_FENCE_TSO\(\) \(\(void\)0\)\n"
                          r"#define LD_FENCE_TSO\(\) \(\(void\)0\)\n#else\n#define ST_FENCE_TSO\(\) ST_FENCE\(\)\n"
                          r"#define LD_FENCE_TSO\(\) LD_FENCE\(\)\n#endif", prelude)
        self.assertIsNotNone(match)

    def test_generated_code_still_compiles_on_the_host(self):
        gcc = shutil.which(os.environ.get("CC", "cc"))
        if not gcc:
            self.skipTest("C compiler needed")
        runtime = self.patched_tree / "runtime"
        for path in sorted(self.new.glob("exe_*.c"))[:1]:
            result = subprocess.run([gcc, "-std=gnu11", "-c", "-w", "-I", str(runtime), "-I", str(self.new), str(path),
                                     "-o", os.devnull], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr[-2000:])

    def test_without_the_environment_flags_no_fence_is_emitted_at_all(self):
        # LIFT_MEMORDER unset: the established behaviour (no fences) is unchanged by the patch.
        python = lifter_python()
        base = Path(self.tmp.name)
        out = base / "no-memorder"
        out.mkdir()
        env = {k: v for k, v in os.environ.items() if k != "LIFT_MEMORDER"}
        env.update(LIFT_GAME="skyrim", LIFT_TSO="1", LIFT_FILES="1")
        subprocess.run([python, "-E", "-s", "-B", str(self.patched_tree / "tools/lift.py"), str(base / "synthetic.exe"),
                        str(base / "functions.json"), str(out), "exe"], cwd=self.patched_tree, env=env, check=True,
                       capture_output=True, timeout=300)
        text = "\n".join(p.read_text() for p in out.glob("exe_*.c"))
        self.assertNotRegex(text, r"(?m)^\s*(?:ST|LD)_FENCE")


if __name__ == "__main__":
    unittest.main()
