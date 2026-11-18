"""Real MOD analysis/optimizer code on authored DDS, NIF, BSA, PEX, ESP and save files."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
VENDOR = ROOT / "vendor"


def dds_legacy(w, h, mips, fourcc=b"DXT1", data=None, caps2=0):
    flags = 0x1007 | (0x20000 if mips > 1 else 0) | 0x80000
    head = bytearray(128)
    head[0:4] = b"DDS "
    struct.pack_into("<IIIIIII", head, 4, 124, flags, h, w, 0, 0, mips)
    struct.pack_into("<II4s", head, 76, 32, 4, fourcc)
    struct.pack_into("<II", head, 108, 0x1000, caps2)
    return bytes(head) + (data or b"")


def dds_dx10(w, h, mips, dxgi, data):
    head = bytearray(148)
    head[0:4] = b"DDS "
    struct.pack_into("<IIIIIII", head, 4, 124, 0x1007 | 0x20000, h, w, 0, 0, mips)
    struct.pack_into("<II4s", head, 76, 32, 4, b"DX10")
    struct.pack_into("<IIIII", head, 128, dxgi, 3, 0, 1, 0)
    return bytes(head) + data


def bgra_uncompressed(w, h, alpha=True):
    head = bytearray(128)
    head[0:4] = b"DDS "
    struct.pack_into("<IIIIIII", head, 4, 124, 0x100F, h, w, w * 4, 0, 1)
    struct.pack_into("<IIIIIIII", head, 76, 32, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    px = bytearray()
    for y in range(h):
        for x in range(w):
            px += bytes((x & 255, y & 255, (x ^ y) & 255, 128 if alpha else 255))
    return bytes(head) + bytes(px)


def chain(fmt_bytes, w, h, mips):
    total = 0
    for i in range(mips):
        lw, lh = max(1, w >> i), max(1, h >> i)
        total += ((lw + 3) // 4) * ((lh + 3) // 4) * fmt_bytes
    return total


def wstr(s):
    b = s.encode()
    return struct.pack("<H", len(b)) + b


class ModOptimizeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler required; run on WSL/Mac")
        if not (VENDOR / "bcdec/bcdec.h").exists() or not (VENDOR / "stb/stb_dxt.h").exists():
            raise unittest.SkipTest("vendor/bcdec and vendor/stb checkouts required (see BUILD_YOUR_COPY.md)")
        cls.tmp = tempfile.TemporaryDirectory(prefix="mod-opt-")
        cls.dir = Path(cls.tmp.name)
        cls.bin = cls.dir / "driver"
        flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, "-I", str(PORT), "-isystem", str(VENDOR / "bcdec"), "-isystem", str(VENDOR / "stb"),
                        str(PORT / "mod_optimize.c"), str(PORT / "mod_analyze.c"), str(PORT / "mod_archive.c"),
                        str(PORT / "mod_plugins.c"), str(ROOT / "tests/native/test_mod_optimize.c"),
                        "-lz", "-lm", "-o", str(cls.bin)], check=True, timeout=180)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def drive(self, *args):
        return subprocess.run([str(self.bin), *map(str, args)], check=True, text=True,
                              capture_output=True, timeout=120).stdout

    def write(self, name, data):
        p = self.dir / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def test_trim_mips_without_reencode(self):
        w = h = 4096
        data = bytes(range(256)) * (chain(8, w, h, 13) // 256 + 1)
        data = data[:chain(8, w, h, 13)]
        src = self.write("big.dds", dds_legacy(w, h, 13, b"DXT1", data))
        out = self.drive("convert", src, self.dir / "big.out.dds", 2048)
        self.assertIn("action 1", out)
        self.assertIn("out 2048x2048 mips 12 format 1 dx10 0 exact 1", out)
        dropped = chain(8, w, h, 1)
        self.assertEqual((self.dir / "big.out.dds").read_bytes()[128:], data[dropped:])

    def test_dx10_bc3_is_reheadered_and_bc7_is_transcoded(self):
        w = h = 256
        bc3 = os.urandom(chain(16, w, h, 9))
        out = self.drive("convert", self.write("d10.dds", dds_dx10(w, h, 9, 77, bc3)), self.dir / "d10.out", 2048)
        self.assertIn("action 2", out)
        self.assertIn("out 256x256 mips 9 format 3 dx10 0 exact 1", out)
        bc7 = os.urandom(chain(16, 512, 512, 1))
        out = self.drive("convert", self.write("bc7.dds", dds_dx10(512, 512, 1, 98, bc7)), self.dir / "bc7.out", 256)
        self.assertIn("action 3", out)
        self.assertRegex(out, r"out 256x256 mips 9 format [13] dx10 0 exact 1")

    def test_uncompressed_and_unmipped_are_transcoded_with_mips(self):
        out = self.drive("convert", self.write("raw.dds", bgra_uncompressed(1024, 512)), self.dir / "raw.out", 2048)
        self.assertIn("action 3", out)
        self.assertIn("out 1024x512 mips 11 format 3 dx10 0 exact 1", out)   # alpha 128 -> BC3
        opaque = self.drive("convert", self.write("op.dds", bgra_uncompressed(1024, 64, alpha=False)), self.dir / "op.out", 512)
        self.assertIn("out 512x32 mips 10 format 1 dx10 0 exact 1", opaque)
        # Odd, non-multiple-of-4 sizes stay well-formed.
        out = self.drive("convert", self.write("odd.dds", dds_legacy(300, 6, 1, b"DXT5", os.urandom(chain(16, 300, 6, 1)))), self.dir / "odd.out", 0)
        self.assertIn("out 300x6 mips 9 format 3 dx10 0 exact 1", out)

    def test_cube_small_and_bc6_are_left_alone(self):
        cube = dds_legacy(64, 64, 1, b"DXT1", os.urandom(chain(8, 64, 64, 1) * 6), caps2=0x200 | 0xFC00)
        self.assertIn("action 0", self.drive("convert", self.write("cube.dds", cube), self.dir / "x", 16))
        small = dds_legacy(128, 128, 8, b"DXT1", os.urandom(chain(8, 128, 128, 8)))
        self.assertIn("action 0", self.drive("convert", self.write("small.dds", small), self.dir / "x", 2048))
        bc6 = dds_dx10(64, 64, 1, 95, os.urandom(chain(16, 64, 64, 1)))
        self.assertIn("action 4", self.drive("convert", self.write("bc6.dds", bc6), self.dir / "x", 2048))
        truncated = dds_legacy(4096, 4096, 1, b"DXT1", b"x" * 100)
        self.assertIn("action 5", self.drive("convert", self.write("trunc.dds", truncated), self.dir / "x", 2048))

    def test_file_is_replaced_atomically(self):
        p = self.write("inplace/t.dds", dds_dx10(64, 64, 7, 71, os.urandom(chain(8, 64, 64, 7))))
        self.assertIn("action 2", self.drive("file", p, 2048))
        self.assertEqual(p.read_bytes()[84:88], b"DXT1")
        self.assertEqual(sorted(x.name for x in p.parent.iterdir()), ["t.dds"])

    def build_se_bsa(self, path):
        # LZ4 frame: one compressed block: "abc" + match(off 3, len 9) + final literals.
        block = bytes([0x35]) + b"abc" + bytes([3, 0]) + bytes([0x50]) + b"XYZWV"
        plain = b"abcabcabcabc" + b"XYZWV"
        frame = struct.pack("<I", 0x184D2204) + bytes([0x40, 0x40, 0x00]) + struct.pack("<I", len(block)) + block + struct.pack("<I", 0)
        files = [("textures\\se", "a.dds", struct.pack("<I", len(plain)) + frame, True),
                 ("textures\\se", "b.dds", b"RAWDATA", False),
                 ("meshes", "..\\evil.nif", b"bad", False)]
        folders = {}
        for folder, name, data, comp in files:
            folders.setdefault(folder, []).append((name, data, comp))
        flags = 1 | 2 | 4          # dir names, file names, compressed by default
        head_len, folder_rec = 36, 24 * len(folders)
        names_block = b"".join(n.encode() + b"\0" for f in folders.values() for n, _, _ in f)
        dir_block = b""
        for folder, items in folders.items():
            dir_block += bytes([len(folder) + 1]) + folder.encode() + b"\0" + b"\0" * 16 * len(items)
        data_start = head_len + folder_rec + len(dir_block) + len(names_block)
        records, blobs, offset = [], b"", data_start
        for folder, items in folders.items():
            for name, data, comp in items:
                size = len(data) | (0 if comp else 0x40000000)
                records.append(struct.pack("<QII", 0, size, offset))
                blobs += data
                offset += len(data)
        dir_block = b""
        i = 0
        for folder, items in folders.items():
            dir_block += bytes([len(folder) + 1]) + folder.encode() + b"\0"
            for _ in items:
                dir_block += records[i]; i += 1
        out = struct.pack("<4sIIIIIIII", b"BSA\0", 105, 36, flags, len(folders), len(files),
                          sum(len(f) + 1 for f in folders), len(names_block), 0)
        out += b"".join(struct.pack("<QIIQ", 0, len(items), 0, 0) for items in folders.values())
        out += dir_block + names_block + blobs
        path.write_bytes(out)
        return plain

    def test_se_bsa_to_loose_files_with_lz4(self):
        bsa = self.dir / "SE.bsa"
        plain = self.build_se_bsa(bsa)
        dest = self.dir / "bsa-out"; dest.mkdir()
        (dest / "textures/se").mkdir(parents=True)
        (dest / "textures/se/b.dds").write_bytes(b"LOOSE")      # existing loose file wins
        out = self.drive("bsa", bsa, dest)
        self.assertIn("bsa 0 files 1", out)
        self.assertEqual((dest / "textures/se/a.dds").read_bytes(), plain)
        self.assertEqual((dest / "textures/se/b.dds").read_bytes(), b"LOOSE")
        self.assertFalse((self.dir / "evil.nif").exists())

    def test_analyze_package_and_save(self):
        d = self.dir / "pkg"
        (d / "textures").mkdir(parents=True); (d / "meshes").mkdir(); (d / "scripts").mkdir(); (d / "SKSE/Plugins").mkdir(parents=True)
        tes4 = lambda form, flags: b"TES4" + struct.pack("<IIIIHH", 0, flags, 0, 0, form, 0)
        (d / "LE.esp").write_bytes(tes4(43, 0)); (d / "SE.esp").write_bytes(tes4(44, 0x200)); (d / "x.esl").write_bytes(b"")
        (d / "LE.bsa").write_bytes(b"BSA\0" + struct.pack("<I", 104)); (d / "SE.bsa").write_bytes(b"BSA\0" + struct.pack("<I", 105))
        (d / "textures/a.dds").write_bytes(dds_dx10(4096, 4096, 1, 98, b""))
        nif = lambda v: b"Gamebryo File Format, Version 20.2.0.7\n" + struct.pack("<IBIII", 0x14020007, 1, 12, 1, v)
        (d / "meshes/le.nif").write_bytes(nif(83)); (d / "meshes/se.nif").write_bytes(nif(100))
        pex = b"\xFA\x57\xC0\xDE\x03\x02\x00\x01" + b"\0" * 8 + (b"\0\0" * 3)
        strings = ["MyQuest", "SKI_ConfigBase", "StringUtil", "Split", "GetLength", "GetNumItems"]
        pex += struct.pack(">H", len(strings)) + b"".join(struct.pack(">H", len(s)) + s.encode() for s in strings)
        (d / "scripts/q.pex").write_bytes(pex)
        (d / "SKSE/Plugins/x.dll").write_bytes(b"MZ")
        out = self.drive("analyze", d)
        self.assertIn("plugins 2 se 1 esl 2 bsas 2 se_bsas 1 textures 1 dx10 1 bc7 1 4k 1 nomips 1 max 4096 meshes 2 se_meshes 1 scripts 1 skse 1 mcm 1 dlls 1", out)
        self.assertIn("missing [Split, GetNumItems]", out)
        # Classic save: header, 2x1 screenshot (RGB), form version, plugin list.
        header = struct.pack("<II", 9, 7) + wstr("Dova") + struct.pack("<I", 3) + wstr("Whiterun") + wstr("Day 1") + wstr("NordRace")
        header += struct.pack("<HffQII", 0, 0.0, 1.0, 0, 2, 1)
        plugins = [b"Skyrim.esm", b"Update.esm", b"CoolMod.esp"]
        info = bytes([len(plugins)]) + b"".join(struct.pack("<H", len(p)) + p for p in plugins)
        ess = b"TESV_SAVEGAME" + struct.pack("<I", len(header)) + header + b"\x01" * 6 + bytes([74]) + struct.pack("<I", len(info)) + info
        out = self.drive("save", self.write("s.ess", ess))
        self.assertIn("save 0 count 3", out)
        self.assertIn("plugin CoolMod.esp", out)


if __name__ == "__main__":
    unittest.main()
