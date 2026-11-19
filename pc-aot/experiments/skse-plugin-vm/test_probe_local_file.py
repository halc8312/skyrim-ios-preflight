"""Check that selected local DLLs are read, diagnosed and left intact."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from test_pe_image import BASE, DATA, RD, SECTIONS, add_named_clock, make_image, put, zero_string

HERE = Path(__file__).resolve().parent


def supported_fixture():
    dll = make_image()
    struct.pack_into("<HH", dll, RD + 0x70, 0, 1)
    dll[0x200:0x206] = bytes.fromhex("b8 01 00 00 00 c3")
    # SKSEPlugin_Query fills PluginInfo in the scratch pointer at [esp+8].
    query = bytes.fromhex("8b 54 24 08 c7 02 01 00 00 00 c7 42 04")
    query += struct.pack("<I", BASE + 0x20c4)
    query += bytes.fromhex("c7 42 08 07 00 00 00 b8 01 00 00 00 c3")
    dll[0x210:0x210 + len(query)] = query
    zero_string(dll, RD + 0xc4, b"Owned test plugin")
    return dll


def serializing_fixture():
    dll = supported_fixture()
    put(dll, SECTIONS + 8, "I", 0x200)
    put(dll, RD + 0x60, "I", 0x1080)
    zero_string(dll, RD + 0x120, b"trial record")
    # The authored load thunk asks for v4 serialization, then registers a
    # UID plus save and load callbacks. EBX and ESI retain the interface
    # table and the owned handle across cdecl function calls.
    code = bytearray.fromhex(
        "53 56 8b 44 24 0c 6a 03 ff 50 10 83 c4 04 85 c0 74 00 "
        "89 c3 8b 44 24 0c ff 50 14 89 c6"
    )
    rejected = 17
    for field, number in ((4, 0x191d77), (12, BASE + 0x1100), (16, BASE + 0x1140)):
        code.extend(b"\x68" + struct.pack("<I", number))
        code.extend(bytes((0x56, 0xff, 0x53, field, 0x83, 0xc4, 8)))
    code.extend(bytes.fromhex("b8 01 00 00 00 5e 5b c3"))
    code[rejected] = len(code) - rejected - 1
    code.extend(bytes.fromhex("31 c0 5e 5b c3"))
    dll[0x280:0x280 + len(code)] = code

    # Save copies an authored byte into one record. Load checks that its
    # metadata was delivered before consuming the first byte.
    save = bytes.fromhex("8b 44 24 04 6a 01") + b"\x68" + struct.pack("<I", BASE + 0x2120)
    save += bytes.fromhex("6a 03 6a 17 ff 50 18 83 c4 10 c3")
    dll[0x300:0x300 + len(save)] = save
    load = bytearray.fromhex("53 8b 5c 24 08")
    for address in (BASE + 0x3128, BASE + 0x3124, BASE + 0x3120):
        load.extend(b"\x68" + struct.pack("<I", address))
    load.extend(bytes.fromhex("ff 53 24 83 c4 0c 85 c0 75 02 0f 0b"))
    for address, expected in ((BASE + 0x3120, 0x17), (BASE + 0x3124, 3), (BASE + 0x3128, 1)):
        load.extend(b"\x83\x3d" + struct.pack("<I", address) + bytes((expected, 0x74, 2, 0x0f, 0x0b)))
    load.extend(bytes.fromhex("6a 01"))
    load.extend(b"\x68" + struct.pack("<I", BASE + 0x312c))
    load.extend(bytes.fromhex("ff 53 28 83 c4 08 83 f8 01 74 02 0f 0b"))
    load.extend(b"\x80\x3d" + struct.pack("<I", BASE + 0x312c) + b"t")
    load.extend(bytes.fromhex("74 02 0f 0b 5b c3"))
    dll[0x340:0x340 + len(load)] = load
    return dll


@unittest.skipIf(sys.platform == "win32", "the selected-file wrapper uses Darwin/POSIX descriptors")
class LocalFileProbeTest(unittest.TestCase):
    def test_regular_file_boundaries_symlinks_and_dll_results(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required")
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        with tempfile.TemporaryDirectory(prefix="dll-local-file-") as directory:
            root = Path(directory)
            binary = root / "probe"
            sources = ("pe_image.c", "x86_call.c", "x86_heap.c", "x86_winthread.c", "x86_winfile.c", "x86_winevent.c",
                       "x86_skse_records.c", "dependency_plan.c",
                       "plugin_trial.c", "probe_local_file.c",
                       "test_probe_local_file.c")
            subprocess.run([cc, *flags, *(HERE / source for source in sources), "-o", binary], check=True, timeout=60)
            good, unknown, serialized = root / "検査.dll", root / "unknown.dll", root / "serialized.dll"
            fixture = supported_fixture()
            good.write_bytes(fixture)
            with_import = supported_fixture()
            add_named_clock(with_import)
            zero_string(with_import, DATA + 0xa2, b"OwnedUnsupported")
            unknown.write_bytes(with_import)
            serialized.write_bytes(serializing_fixture())
            invalid, empty, big = root / "invalid.dll", root / "empty.dll", root / "large.dll"
            invalid.write_bytes(b"not a PE file")
            empty.touch()
            with big.open("wb") as file:
                file.truncate(64 * 1024 * 1024 + 1)
            link, fifo = root / "link.dll", root / "fifo.dll"
            link.symlink_to(good)
            os.mkfifo(fifo, mode=0o600)
            result = subprocess.check_output([binary, good, unknown, serialized, invalid, empty, big,
                                              link, fifo, root / "absent.dll", root],
                                             text=True, timeout=3).splitlines()
            self.assertEqual(result, ["ready,7,Owned test plugin",
                                      "stopped,1,import absent from the service list,1,kernel32.DLL,OwnedUnsupported",
                                      "ready,7,Owned test plugin,2,1",
                                      "stopped,1,truncated headers,0", "2", "3", "1", "2", "1", "2"])
            self.assertEqual(good.read_bytes(), fixture)
            self.assertEqual(unknown.read_bytes(), with_import)


if __name__ == "__main__":
    unittest.main()
