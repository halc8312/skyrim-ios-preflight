"""Check that selected local DLLs are read, diagnosed and left intact."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from test_pe_image import BASE, DATA, RD, add_named_clock, make_image, zero_string

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
            sources = ("pe_image.c", "x86_call.c", "plugin_trial.c", "probe_local_file.c", "test_probe_local_file.c")
            subprocess.run([cc, *flags, *(HERE / source for source in sources), "-o", binary], check=True, timeout=60)
            good, unknown = root / "検査.dll", root / "unknown.dll"
            fixture = supported_fixture()
            good.write_bytes(fixture)
            with_import = supported_fixture()
            add_named_clock(with_import)
            zero_string(with_import, DATA + 0xa2, b"OwnedUnsupported")
            unknown.write_bytes(with_import)
            invalid, empty, big = root / "invalid.dll", root / "empty.dll", root / "large.dll"
            invalid.write_bytes(b"not a PE file")
            empty.touch()
            with big.open("wb") as file:
                file.truncate(64 * 1024 * 1024 + 1)
            link, fifo = root / "link.dll", root / "fifo.dll"
            link.symlink_to(good)
            os.mkfifo(fifo, mode=0o600)
            result = subprocess.check_output([binary, good, unknown, invalid, empty, big,
                                              link, fifo, root / "absent.dll", root],
                                             text=True, timeout=3).splitlines()
            self.assertEqual(result, ["ready,7,Owned test plugin", "stopped,1,import absent from the service list,1",
                                      "stopped,1,truncated headers,0", "2", "3", "1", "2", "1", "2"])
            self.assertEqual(good.read_bytes(), fixture)
            self.assertEqual(unknown.read_bytes(), with_import)


if __name__ == "__main__":
    unittest.main()
