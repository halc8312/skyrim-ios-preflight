"""Exercise the TESV native declaration inventory with authored scripts."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
LLVM = Path(os.environ.get("SKYRIM_LLVM_BIN", "C:/Program Files/LLVM/bin"))
STRINGS = (
    "", "Demo", "None", "Run", "String", "Path", "Actor[]", "Actors",
    "Int", "Count", "Helper", "Mode", "cOuNt", "Guard", "Temp", "CustomType",
)


def words(*values):
    return struct.pack(f">{len(values)}H", *values)


def raw_string(value):
    data = value.encode("ascii")
    return words(len(data)) + data


def function(name, result, args=(), flags=3, instructions=b"", instruction_count=0):
    params = b"".join(words(*item) for item in args)
    return (words(name, result, 0) + bytes(4) + bytes((flags,)) +
            words(len(args)) + params + words(0, instruction_count) + instructions)


def script(*, function_body=None, object_size_bias=4, truncated_ref=False, opcode=25,
           debug=True, state=0, declaration_args=((5, 4), (7, 6)), strings=STRINGS):
    # callstatic with two operands after the fixed class/member/destination.
    call = bytes((opcode, 1)) + words(1) + b"\x01" + words(3) + b"\x01" + words(14)
    call += b"\x03\x00\x00\x00\x02\x02" + words(3) + b"\x00"
    native = function(3, 8, declaration_args) if function_body is None else function_body
    normal = function(10, 2, flags=1, instructions=call, instruction_count=1)
    header = bytes.fromhex("fa57c0de03020001") + bytes(8)
    header += b"".join(raw_string(item) for item in ("Demo.psc", "", ""))
    header += words(len(strings)) + b"".join(raw_string(s) for s in strings)
    if debug:
        header += b"\x01" + bytes(8) + words(1, 1, 0, 10) + b"\x00" + words(1, 18)
    else:
        header += b"\x00"
    # User flag table has one known name and bit number.
    header += words(1, 13) + b"\x00" + words(1, 1)
    obj = words(0, 0) + bytes(4) + words(0, 0, 0, 1)
    obj += words(400 if truncated_ref else state, 2) + normal + native
    return header + struct.pack(">I", len(obj) + object_size_bias) + obj


class ClassicPexTest(unittest.TestCase):
    def test_types_shape_and_bounded_traversal(self):
        compiler = LLVM / "clang.exe" if os.name == "nt" else "cc"
        cc = shutil.which(os.environ.get("CC", str(compiler)))
        if not cc:
            self.skipTest("C compiler required")
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.name == "nt":
            flags += ["-D_CRT_SECURE_NO_WARNINGS"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        with tempfile.TemporaryDirectory(prefix="classic-pex-") as directory:
            root = Path(directory)
            binary = root / ("pex.exe" if os.name == "nt" else "pex")
            sources = [HERE / "classic_pex.c", HERE / "test_classic_pex.c"]
            subprocess.run([cc, *flags, *sources, "-o", binary], check=True, timeout=45)
            cases = {
                "stock": script(),
                "caprica": script(object_size_bias=0, debug=False),
                "named": script(state=11),
                "body": script(function_body=function(3, 8, ((5, 4), (7, 6)), flags=1)),
                "bad-reference": script(truncated_ref=True),
                "bad-length": script(object_size_bias=1),
                "bad-opcode": script(opcode=36),
                "bad-native-body": script(function_body=function(3, 8, instruction_count=1)),
                "bad-nul": script(strings=(*STRINGS[:3], "Run\0injected", *STRINGS[4:])),
                "too-many-args": script(declaration_args=((5, 4),) * 9),
                "instance-call": script(opcode=23),
            }
            paths = []
            for name, data in cases.items():
                path = root / (name + ".pex")
                path.write_bytes(data)
                paths.append(path)
            subprocess.run([binary, *paths], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
