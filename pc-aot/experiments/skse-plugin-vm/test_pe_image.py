"""Exercise the C reader. The only dynamically loaded library is the reader itself.

The PE fixtures are authored here, and none of their instructions are executed.
Run with: python3 -m unittest discover -s experiments/skse-plugin-vm -p test_pe_image.py
"""

import ctypes as c
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
NT = 0x80
OPT = NT + 24
SECTIONS = OPT + 224
RD = 0x400
DATA = 0x600
BASE = 0x10000000
MAX_IMPORT_SYMBOLS = 256
R, W, X = 1, 2, 4


class Section(c.Structure):
    _fields_ = [("rva", c.c_uint32), ("length", c.c_uint32), ("permissions", c.c_uint)]


class Image(c.Structure):
    _fields_ = [(n, c.c_uint32) for n in ("base", "size", "query_va", "load_va", "entry_va")] + [
        ("bytes", c.c_void_p), ("headers_size", c.c_uint32),
        ("section_count", c.c_uint16), ("sections", Section * 96),
        ("data_page_protection", c.c_void_p)]


class Symbol(c.Structure):
    _fields_ = [("library_index", c.c_uint16), ("ordinal", c.c_uint16),
                ("by_ordinal", c.c_uint8), ("name", c.c_char * 128)]


class Binding(c.Structure):
    _fields_ = [("library", c.c_char_p), ("symbol", c.c_char_p), ("service_token", c.c_uint32)]

class RequiredExport(c.Structure):
    _fields_ = [("name", c.c_char_p), ("address", c.c_uint32)]

class TlsInfo(c.Structure):
    _fields_ = [(n, c.c_uint32) for n in ("present", "template_rva", "template_bytes", "zero_fill",
                                          "alignment", "index_rva", "callback_count")] + [
        ("callback_rvas", c.c_uint32 * 64)]

CLOCK_BINDING = (b"KERNEL32.dll", b"GetTickCount", 0x60000110)


class Report(c.Structure):
    _fields_ = [("directory_mask", c.c_uint32), ("entry_rva", c.c_uint32),
                ("has_writable_code", c.c_uint), ("library_count", c.c_uint16),
                ("symbol_count", c.c_uint16), ("libraries", (c.c_char * 128) * 32),
                ("symbols", Symbol * MAX_IMPORT_SYMBOLS), ("preferred_base", c.c_uint32), ("image_size", c.c_uint32)]


def put(data, offset, fmt, *fields):
    struct.pack_into("<" + fmt, data, offset, *fields)


def zero_string(data, offset, value):
    data[offset:offset + len(value) + 1] = value + b"\0"


def directory(data, which, rva, length):
    put(data, OPT + 96 + which * 8, "II", rva, length)


def make_image():
    """PE32 with RX code, R export table and RW data including a zero-filled tail."""
    d = bytearray(0x800)
    d[:2] = b"MZ"
    put(d, 60, "I", NT)
    d[NT:NT + 4] = b"PE\0\0"
    put(d, NT + 4, "HHIIIHH", 0x14c, 3, 0, 0, 0, 224, 0x2102)
    put(d, OPT, "H", 0x10b)
    put(d, OPT + 28, "III", BASE, 0x1000, 0x200)
    put(d, OPT + 56, "II", 0x4000, 0x200)
    put(d, OPT + 92, "I", 16)
    directory(d, 0, 0x2000, 0xc0)
    sections = [(b".text", 0x40, 0x1000, 0x200, 0x60000020),
                (b".rdata", 0x200, 0x2000, RD, 0x40000040),
                (b".data", 0x240, 0x3000, DATA, 0xc0000040)]
    for i, (name, size, rva, raw, flags) in enumerate(sections):
        at = SECTIONS + i * 40
        d[at:at + len(name)] = name
        put(d, at + 8, "IIII", size, rva, 0x200, raw)
        put(d, at + 36, "I", flags)
    put(d, RD + 12, "IIIIIII", 0x2040, 1, 2, 2, 0x2060, 0x2068, 0x2070)
    zero_string(d, RD + 0x40, b"OwnedFixture.dll")
    put(d, RD + 0x60, "IIIIHH", 0x1000, 0x1010, 0x2080, 0x20a0, 1, 0)
    zero_string(d, RD + 0x80, b"SKSEPlugin_Load")
    zero_string(d, RD + 0xa0, b"SKSEPlugin_Query")
    d[0x200] = d[0x210] = 0xc3
    d[DATA] = 42
    return d


def add_imports(d):
    d[DATA:DATA + 0x100] = bytes(0x100)
    directory(d, 1, 0x3000, 40)
    directory(d, 12, 0x3060, 12)
    put(d, DATA, "IIIII", 0x3080, 0, 0, 0x3040, 0x3060)
    zero_string(d, DATA + 0x40, b"OwnedDependency.dll")
    for i in (0x60, 0x80):
        put(d, DATA + i, "III", 0x30a0, 0x80000007, 0)
    put(d, DATA + 0xa0, "H", 3)
    zero_string(d, DATA + 0xa2, b"Example")


def add_named_clock(d):
    add_imports(d)
    directory(d, 12, 0x3060, 8)
    put(d, DATA + 0x64, "I", 0)
    put(d, DATA + 0x84, "I", 0)
    zero_string(d, DATA + 0x40, b"kernel32.DLL")
    zero_string(d, DATA + 0xa2, b"GetTickCount")


def add_code_relocation(d, pointed_rva=0x2040):
    directory(d, 5, 0x3100, 12)
    put(d, DATA + 0x100, "IIHH", 0x1000, 12, 0x3008, 0)
    put(d, 0x208, "I", BASE + pointed_rva)


def add_tls(d):
    directory(d, 9, 0x20c0, 24)
    put(d, RD + 0xc0, "6I", BASE + 0x3120, BASE + 0x3128, BASE + 0x3210,
        BASE + 0x20e0, 12, 0x300000)
    put(d, RD + 0xe0, "4I", BASE + 0x1010, BASE + 0x1000, BASE + 0x1010, 0)
    put(d, DATA + 0x120, "2I", 0x13579bdf, 0)
    # All four TLS directory pointers and the three callbacks are HIGHLOW.
    directory(d, 5, 0x3180, 24)
    put(d, DATA + 0x180, "II8H", 0x2000, 24,
        0x30c0, 0x30c4, 0x30c8, 0x30cc, 0x30e0, 0x30e4, 0x30e8, 0)


def add_legacy_load_config(d):
    directory(d, 10, 0x20c0, 64)
    put(d, RD + 0xc0, "I", 72)
    put(d, RD + 0xfc, "III", BASE + 0x3130, BASE + 0x2190, 2)
    put(d, RD + 0x190, "II", 0x1000, 0x1010)
    put(d, DATA + 0x130, "I", 0x13579bdf)
    directory(d, 5, 0x3180, 12)
    put(d, DATA + 0x180, "IIHH", 0x2000, 12, 0x30fc, 0x3100)


def find_tool(*names):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    if sys.platform == "win32":
        llvm = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM" / "bin"
        for name in names:
            path = llvm / (name + ".exe")
            if path.is_file():
                return str(path)
    return None


class PeReaderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        cls.cc = find_tool("clang", "clang-18", "cc")
        if not cls.cc:
            raise unittest.SkipTest("a host C compiler is required")
        options = ["-std=c11", "-O1", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
        if sys.platform == "win32":
            path = cls.work / "owned_reader.dll"
            definitions = cls.work / "reader.def"
            symbols = ("skse_pe_map", "skse_pe_map_with_imports", "skse_pe_map_at_base", "skse_pe_map_library",
                       "skse_pe_map_tls", "skse_pe_inspect", "skse_pe_classic_exports",
                       "skse_pe_allows", "skse_pe_free", "skse_pe_status_text")
            definitions.write_text("EXPORTS\n" + "\n".join(symbols) + "\n", encoding="ascii")
            options.extend(["-shared", "-Xlinker", "/def:" + str(definitions)])
        else:
            path = cls.work / "owned_reader.so"
            options.extend(["-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC"])
        subprocess.run([cls.cc, *options, HERE / "pe_image.c", "-o", path], cwd=cls.work, check=True)
        cls.lib = c.CDLL(str(path))
        if sys.platform == "win32":
            import _ctypes
            cls.addClassCleanup(_ctypes.FreeLibrary, cls.lib._handle)
        cls.lib.skse_pe_map.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(Image)]
        cls.lib.skse_pe_map_with_imports.argtypes = [
            c.c_void_p, c.c_size_t, c.POINTER(Binding), c.c_size_t, c.c_uint, c.POINTER(Image)]
        cls.lib.skse_pe_map_at_base.argtypes = [
            c.c_void_p, c.c_size_t, c.c_uint32, c.POINTER(Binding), c.c_size_t, c.c_uint, c.POINTER(Image)]
        cls.lib.skse_pe_map_library.argtypes = [
            c.c_void_p, c.c_size_t, c.c_uint32, c.POINTER(Binding), c.c_size_t, c.c_uint,
            c.POINTER(RequiredExport), c.c_size_t, c.POINTER(Image)]
        cls.lib.skse_pe_map_tls.argtypes = [*cls.lib.skse_pe_map_library.argtypes, c.POINTER(TlsInfo)]
        cls.lib.skse_pe_inspect.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(Report)]
        cls.lib.skse_pe_classic_exports.argtypes = [c.c_void_p, c.c_size_t]
        cls.lib.skse_pe_allows.argtypes = [c.POINTER(Image), c.c_uint32, c.c_size_t, c.c_uint]
        cls.lib.skse_pe_status_text.argtypes = [c.c_int]
        cls.lib.skse_pe_status_text.restype = c.c_char_p
        cls.lib.skse_pe_free.argtypes = [c.POINTER(Image)]
        cls.lib.skse_pe_free.restype = None

    def map(self, data):
        buf = c.create_string_buffer(bytes(data))
        image = Image()
        status = self.lib.skse_pe_map(buf, len(data), c.byref(image))
        self.addCleanup(self.lib.skse_pe_free, c.byref(image))
        return self.lib.skse_pe_status_text(status).decode("ascii"), image

    def expect(self, data, reason):
        status, image = self.map(data)
        self.assertEqual(reason, status)
        self.assertFalse(image.bytes)
        self.assertEqual(0, image.section_count)
        report = Report()
        buf = c.create_string_buffer(bytes(data))
        status = self.lib.skse_pe_inspect(buf, len(data), c.byref(report))
        self.assertEqual(reason, self.lib.skse_pe_status_text(status).decode("ascii"))
        return report

    def bind(self, data, bindings=(CLOCK_BINDING,), flags=0, base=None):
        buf = c.create_string_buffer(bytes(data))
        services = (Binding * len(bindings))(*(Binding(*item) for item in bindings))
        image = Image()
        if base is None:
            status = self.lib.skse_pe_map_with_imports(buf, len(data), services, len(services), flags, c.byref(image))
        else:
            status = self.lib.skse_pe_map_at_base(buf, len(data), base, services, len(services), flags, c.byref(image))
        self.addCleanup(self.lib.skse_pe_free, c.byref(image))
        self.assertEqual(bytes(data), buf.raw[:len(data)])
        if status:
            self.assertFalse(image.bytes)
            self.assertEqual((0, 0, 0), (image.query_va, image.load_va, image.section_count))
        return self.lib.skse_pe_status_text(status).decode("ascii"), image

    def test_owned_image_permissions_header_gaps_padding_and_zero_fill(self):
        d = make_image()
        status, im = self.map(d)
        self.assertEqual("ok", status)
        self.assertEqual((BASE, 0x4000, BASE + 0x1000, BASE + 0x1010, 0),
                         (im.base, im.size, im.query_va, im.load_va, im.entry_va))
        self.assertEqual(b"MZ", c.string_at(im.bytes, 2))
        self.assertEqual(b"\xc3", c.string_at(im.bytes + 0x1000, 1))
        self.assertEqual(b"*", c.string_at(im.bytes + 0x3000, 1))
        self.assertEqual(bytes(0x40), c.string_at(im.bytes + 0x3200, 0x40))
        for rva, size, flags, yes in [
                (0, 512, R, True), (0, 1, X, False), (0, 1, W, False),
                (511, 2, R, False), (0x200, 1, R, False),
                (0x1000, 64, R | X, True), (0x1000, 1, W, False),
                (0x103f, 2, X, False), (0x1040, 1, R, False),
                (0x2000, 1, R, True), (0x2000, 1, X, False),
                (0x3200, 64, R | W, True), (0x3200, 64, X, False),
                (0x323f, 2, W, False), (0, 0, R, False), (0, 1, 0, False),
                (0x3000, 1, 8, False), (0x1000, c.c_size_t(-1).value, X, False)]:
            with self.subTest(rva=rva, size=size, flags=flags):
                self.assertEqual(yes, bool(self.lib.skse_pe_allows(c.byref(im), BASE + rva, size, flags)))
        self.assertFalse(self.lib.skse_pe_allows(c.byref(im), BASE - 1, 1, R))
        self.lib.skse_pe_free(c.byref(im))
        self.assertFalse(im.bytes)
        self.assertFalse(self.lib.skse_pe_allows(c.byref(im), BASE, 1, R))

    def map_tls(self, data, base=0, bindings=(), names=(), flags=0):
        source = c.create_string_buffer(bytes(data))
        services = (Binding * len(bindings))(*(Binding(*b) for b in bindings))
        required = (RequiredExport * len(names))(*(RequiredExport(n, 0xbaded) for n in names)) if names else None
        image, tls = Image(), TlsInfo()
        c.memset(c.byref(tls), 0xa5, c.sizeof(tls))
        status = self.lib.skse_pe_map_tls(source, len(data), base, services, len(bindings), flags,
                                          required, len(names), c.byref(image), c.byref(tls))
        self.addCleanup(self.lib.skse_pe_free, c.byref(image))
        self.assertEqual(bytes(data), source.raw[:len(data)])
        if status:
            self.assertFalse(any(bytes(image)))
            self.assertFalse(any(bytes(tls)))
            self.assertTrue(all(e.address == 0 for e in (required or ())))
        return self.lib.skse_pe_status_text(status).decode(), image, tls

    def test_tls_opt_in_template_index_order_relocation_default_rejection(self):
        d = make_image()
        self.assertEqual(0, self.map_tls(d)[2].present)
        add_tls(d)
        self.expect(d, "TLS unsupported")
        self.assertEqual(0, self.lib.skse_pe_classic_exports(bytes(d), len(d)))
        for base in (BASE, 0x23000000):
            status, im, tls = self.map_tls(d, base)
            self.assertEqual("ok", status)
            self.assertEqual((1, 0x3120, 8, 12, 4, 0x3210, 3), (
                tls.present, tls.template_rva, tls.template_bytes, tls.zero_fill, tls.alignment,
                tls.index_rva, tls.callback_count))
            self.assertEqual([0x1010, 0x1000, 0x1010], list(tls.callback_rvas[:tls.callback_count]))
            self.assertEqual(struct.pack("<2I", 0x13579bdf, 0), c.string_at(im.bytes + tls.template_rva, 8))
            self.assertEqual(bytes(4), c.string_at(im.bytes + tls.index_rva, 4))
            self.assertEqual((base+0x3120, base+0x3128, base+0x3210, base+0x20e0, 12, 0x300000),
                             struct.unpack("<6I", c.string_at(im.bytes + 0x20c0, 24)))
            self.assertEqual(tuple(base+r for r in [0x1010, 0x1000, 0x1010]) + (0,),
                             struct.unpack("<4I", c.string_at(im.bytes + 0x20e0, 16)))

    def test_tls_empty_absent_callbacks_alignment_and_callback_limit(self):
        d = make_image()
        add_tls(d)
        put(d, RD+0xc0, "II", 0, 0)
        put(d, RD+0xcc, "III", 0, 16, 0)
        status, _, tls = self.map_tls(d)
        self.assertEqual(("ok", 0, 16, 0, 0), (status, tls.template_bytes, tls.zero_fill,
                                               tls.callback_count, tls.alignment))
        put(d, RD+0xc0, "II", BASE + 0x3120, BASE + 0x3120)
        put(d, RD+0xcc, "I", BASE + 0x3000)
        put(d, DATA, "65I", *([BASE + 0x1010] * 64), 0)
        status, _, tls = self.map_tls(d)
        self.assertEqual(("ok", 64), (status, tls.callback_count))
        put(d, DATA+64*4, "I", BASE + 0x1010)
        self.assertEqual("parser limit exceeded", self.map_tls(d)[0])

    def test_malformed_tls_metadata_does_not_publish(self):
        cases = [
            (0, 0), (0, BASE-1), (0, BASE+0x3129), (0, BASE+0x2fff), (1, BASE+0x3210),
            (2, 0), (2, BASE+0x1000), (2, BASE+0x3124), (2, BASE+0x3211), (2, BASE+0x323e),
            (3, BASE-1), (3, BASE+0x400), (3, BASE+0x20e1), (3, BASE+0x3200),
            (5, 1), (5, 0xf00000), (5, 0x1300000),
        ]
        for field, value in cases:
            with self.subTest(field=field, value=hex(value)):
                d = make_image()
                add_tls(d)
                put(d, RD+0xc0+field*4, "I", value)
                self.assertEqual("invalid static TLS metadata", self.map_tls(d)[0])
        d = make_image()
        add_tls(d)
        directory(d, 9, 0x20c0, 20)
        self.assertEqual("invalid static TLS metadata", self.map_tls(d)[0])
        directory(d, 9, 0x20c0, 24)
        put(d, RD+0xd0, "I", 0xffffffff)
        self.assertEqual("parser limit exceeded", self.map_tls(d)[0])
        put(d, RD+0xd0, "I", 0)
        put(d, RD+0xcc, "I", BASE+0x31fc)
        put(d, DATA+0x1fc, "I", BASE+0x1000)  # Its null terminator would lie in BSS.
        self.assertEqual("invalid static TLS metadata", self.map_tls(d)[0])
        for target in (BASE-1, BASE+0x40, BASE+0x1040, BASE+0x2000, BASE+0x3210):
            with self.subTest(target=hex(target)):
                put(d, RD+0xcc, "I", BASE+0x20e0)
                put(d, RD+0xe0, "I", target)
                self.assertEqual("invalid static TLS metadata", self.map_tls(d)[0])

    def test_tls_missing_fixups_at_alternate_base_and_import_alias(self):
        for missing in range(7):
            d = make_image()
            add_tls(d)
            put(d, DATA+0x188+missing*2, "H", 0)
            self.assertEqual("ok", self.map_tls(d)[0])
            self.assertEqual("invalid image relocation", self.map_tls(d, 0x23000000)[0])
        d = make_image()
        add_named_clock(d)
        add_tls(d)
        for field, value in ((0, BASE+0x3060), (2, BASE+0x3060), (3, BASE+0x3060)):
            sample = bytearray(d)
            put(sample, RD+0xc0+field*4, "I", value)
            if field == 0:
                put(sample, RD+0xc4, "I", value+4)
            self.assertEqual("invalid static TLS metadata", self.map_tls(sample, bindings=(CLOCK_BINDING,))[0])

    def test_tls_rejects_relocated_index_and_keeps_one_past_template_end(self):
        d = make_image()
        add_tls(d)
        put(d, RD+0xc8, "I", BASE+0x3140)
        put(d, DATA+0x140, "I", BASE+0x2004)
        directory(d, 5, 0x3180, 36)
        put(d, DATA+0x198, "IIHH", 0x3000, 12, 0x3140, 0)
        self.assertEqual("invalid image relocation", self.map_tls(d, 0x23000000)[0])
        d = make_image()
        add_tls(d)
        # A real initialized section may end at the final image boundary.
        # The half-open end is allowed unless rebasing would wrap it to zero.
        d.extend(bytes(DATA+0xd000-len(d)))
        put(d, OPT+56, "I", 0x10000)
        put(d, SECTIONS+80+8, "III", 0xd000, 0x3000, 0xd000)
        put(d, RD+0xc0, "II", BASE+0xfffc, BASE+0x10000)
        status, im, tls = self.map_tls(d, 0x23000000)
        self.assertEqual(("ok", 0xfffc, 4), (status, tls.template_rva, tls.template_bytes))
        self.assertEqual(0x23010000, struct.unpack("<I", c.string_at(im.bytes+0x20c4, 4))[0])
        self.assertEqual("invalid image relocation", self.map_tls(d, 0xffff0000)[0])

    @unittest.skipUnless(sys.platform == "win32", "the Windows loader verifies the true native reference")
    def test_real_windows_tls_matches_both_mapped_layouts(self):
        llvm = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin"
        sdk = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / (
            "Windows Kits/10/Lib/10.0.22621.0/um/x86/kernel32.lib")
        if not sdk.is_file() or not all((llvm / name).is_file() for name in (
                "clang.exe", "lld-link.exe", "llvm-readobj.exe", "llvm-objdump.exe")):
            self.skipTest("the native reference requires the pinned Windows SDK and LLVM")
        clang_version = subprocess.check_output([llvm / "clang.exe", "--version"], text=True).splitlines()[0]
        if not re.match(r"clang version 19\.1\.1(?:\s|$)", clang_version):
            self.skipTest("the reproducible native fixture uses LLVM 19.1.1")
        comparison = HERE.parent / "classic-dll-tls" / "compare.py"
        where = self.work / "tls-windows"
        subprocess.run([sys.executable, comparison, "windows", where, "--llvm", llvm, "--kernel32-lib", sdk], check=True)
        dll = (where / "OwnedTls.dll").read_bytes()
        self.expect(dll, "TLS unsupported")
        status = self.lib.skse_pe_classic_exports(dll, len(dll))
        self.assertEqual("missing SKSE exports", self.lib.skse_pe_status_text(status).decode())
        binding = [(b"KERNEL32.dll", b"MulDiv", 0x60000110)]
        self.assertEqual("missing dependency export", self.map_tls(dll, 0, binding, [b"Missing"], 1)[0])
        for base in (0x100000, 0x200000):
            status, im, tls = self.map_tls(dll, base, binding, [b"ReadValue"], 1)
            self.assertEqual("ok", status)
            self.assertEqual((0x4000, 12, 12, 4, 0x3000, [0x11b0, 0x11d0]), (
                tls.template_rva, tls.template_bytes, tls.zero_fill, tls.alignment, tls.index_rva,
                list(tls.callback_rvas[:tls.callback_count])))
            self.assertEqual(struct.pack("<3I", 0, 0x13579bdf, 0), c.string_at(im.bytes+tls.template_rva, 12))

    def test_every_input_truncation(self):
        data = make_image()
        buf = c.create_string_buffer(bytes(data))
        for length in range(len(data)):
            image = Image()
            self.assertNotEqual(0, self.lib.skse_pe_map(buf, length, c.byref(image)), msg=length)
            self.assertFalse(image.bytes)
        self.assertEqual("ok", self.map(data)[0])

    def test_malformed_structure_and_machine(self):
        for offset, fmt, value, reason in [
                (0, "H", 0, "malformed PE"), (60, "I", 0xffffffff, "truncated headers"),
                (NT + 4, "H", 0x8664, "requires PE32 i386"), (OPT, "H", 0x20b, "requires PE32 i386"),
                (NT + 6, "H", 97, "parser limit exceeded"), (NT + 20, "H", 32, "malformed PE"),
                (NT + 22, "H", 2, "malformed PE"), (OPT + 28, "I", 0x1234, "unsupported PE layout"),
                (OPT + 32, "I", 1, "unsupported PE layout"), (OPT + 56, "I", 0x4000000, "parser limit exceeded"),
                (OPT + 60, "I", 0x100, "malformed PE"), (OPT + 92, "I", 17, "unsupported PE layout"),
                (SECTIONS + 40 + 20, "I", 0x200, "malformed PE"),
                (SECTIONS + 40 + 12, "I", 0x1000, "malformed PE"),
                (SECTIONS + 40 + 12, "I", 0xfffff000, "malformed PE"),
                (SECTIONS + 8, "I", 0x300, "unsupported PE layout"),
                (RD + 36, "I", 0x3400, "malformed PE"), (RD + 24, "I", 5000, "parser limit exceeded")]:
            with self.subTest(offset=offset, value=value):
                d = make_image()
                put(d, offset, fmt, value)
                self.expect(d, reason)
        d = make_image()
        put(d, OPT + 28, "I", 0xffff0000)
        put(d, OPT + 56, "I", 0x20000)
        self.expect(d, "malformed PE")

    def test_export_targets_and_names(self):
        for offset, fmt, value, reason in [
                (RD + 0x60, "I", 0x2055, "forwarded SKSE export"),
                (RD + 0x60, "I", 0x103f, None), (RD + 0x60, "I", 0x1040, "malformed PE"),
                (RD + 0x60, "I", 0x3010, "malformed PE"), (RD + 0x60, "I", 0, "malformed PE"),
                (RD + 0x70, "H", 2, "malformed PE"), (RD + 0x68, "I", 0x20a0, "malformed PE"),
                (RD + 0x6c, "I", 0x3201, "malformed PE")]:
            with self.subTest(offset=offset, value=value):
                d = make_image()
                put(d, offset, fmt, value)
                if reason:
                    self.expect(d, reason)
                else:
                    self.assertEqual("ok", self.map(d)[0])
        d = make_image()
        zero_string(d, RD + 0xa0, b"Other")
        self.expect(d, "missing SKSE exports")
        zero_string(d, RD + 0xa0, b"_SKSEPlugin_Query")
        self.assertEqual("ok", self.map(d)[0])
        zero_string(d, RD + 0xa0, b"_SKSEPlugin_Query@8")
        self.expect(d, "unsupported SKSE calling convention")
        d = make_image()
        directory(d, 0, 0, 0)
        self.expect(d, "missing SKSE exports")

    def test_unsupported_features_and_nonzero_entry(self):
        for number, reason in [(9, "TLS unsupported"), (12, "imports unsupported"),
                               (13, "delay imports unsupported"), (14, "CLR unsupported"),
                               (10, "unsupported data directory")]:
            with self.subTest(number=number):
                d = make_image()
                directory(d, number, 0x3100, 16)
                report = self.expect(d, reason)
                self.assertTrue(report.directory_mask & (1 << number))
        d = make_image()
        put(d, OPT + 16, "I", 0x1000)
        self.assertEqual(0x1000, self.expect(d, "DLL entry point unsupported").entry_rva)
        d = make_image()
        put(d, SECTIONS + 36, "I", 0xe0000020)
        self.assertTrue(self.expect(d, "writable executable section").has_writable_code)
        d = make_image()
        directory(d, 4, len(d) - 8, 8)
        self.expect(d, "unsupported data directory")
        directory(d, 4, len(d) - 8, 16)
        self.expect(d, "malformed PE")

    def test_import_report_and_lookup_fallback_never_resolve(self):
        for original_first_thunk in [0, 0x3080]:
            with self.subTest(original_first_thunk=original_first_thunk):
                d = make_image()
                add_imports(d)
                put(d, DATA, "I", original_first_thunk)
                info = self.expect(d, "imports unsupported")
                self.assertEqual((1, 2), (info.library_count, info.symbol_count))
                self.assertEqual(b"OwnedDependency.dll", info.libraries[0].value)
                a, b = info.symbols[0], info.symbols[1]
                self.assertEqual((0, 0, b"Example"), (a.library_index, a.by_ordinal, a.name))
                self.assertEqual((0, 1, 7), (b.library_index, b.by_ordinal, b.ordinal))

    def test_dependency_exports_are_exact_and_published_together(self):
        data = make_image()
        zero_string(data, RD + 0x80, b"OwnedWorkerFunction")
        zero_string(data, RD + 0xa0, b"SecondOwnedFunction")
        self.expect(data, "missing SKSE exports")
        file = c.create_string_buffer(bytes(data))
        image = Image()
        self.addCleanup(self.lib.skse_pe_free, c.byref(image))
        exports = (RequiredExport * 2)(RequiredExport(b"OwnedWorkerFunction"), RequiredExport(b"SecondOwnedFunction"))
        status = self.lib.skse_pe_map_library(file, len(data), 0, None, 0, 0, exports, 2, c.byref(image))
        self.assertEqual(0, status)
        self.assertEqual([BASE + 0x1010, BASE + 0x1000], [e.address for e in exports])
        self.lib.skse_pe_free(c.byref(image))
        exports[1].name = b"secondOwnedFunction"
        status = self.lib.skse_pe_map_library(file, len(data), 0, None, 0, 0, exports, 2, c.byref(image))
        self.assertEqual(b"missing dependency export", self.lib.skse_pe_status_text(status))
        self.assertEqual([0, 0], [e.address for e in exports])
        self.assertFalse(image.bytes)
        put(data, RD + 0x64, "I", 0x2050)
        file = c.create_string_buffer(bytes(data))
        status = self.lib.skse_pe_map_library(file, len(data), 0, None, 0, 0, exports, 2, c.byref(image))
        self.assertEqual(b"forwarded dependency export", self.lib.skse_pe_status_text(status))
        self.assertFalse(image.bytes)

    def test_import_bounds_count_and_terminators(self):
        for at, value in [(DATA, 0xfffffff0), (DATA + 16, 0x3210),
                          (DATA + 0x80, 0x31ff), (DATA + 0x84, 0x80010002),
                          (DATA + 12, 0x3201), (DATA + 0x88, 0x3401)]:
            with self.subTest(at=at, value=value):
                d = make_image()
                add_imports(d)
                put(d, at, "I", value)
                self.expect(d, "malformed PE")
        d = make_image()
        add_imports(d)
        directory(d, 1, 0x3000, 20)
        self.expect(d, "malformed PE")
        d = make_image()
        add_imports(d)
        d[DATA + 0xa2:DATA + 0xa2 + 128] = b"x" * 128
        self.expect(d, "malformed PE")

    def test_import_manifest_capacity(self):
        for many_libraries in (True, False):
            with self.subTest(many_libraries=many_libraries):
                d = make_image()
                d[DATA:] = bytes(0xa00)
                put(d, SECTIONS + 80 + 8, "I", 0xa00)
                put(d, SECTIONS + 80 + 16, "I", 0xa00)
                count = 33 if many_libraries else 1
                directory(d, 1, 0x3000, (count + 1) * 20)
                for i in range(count):
                    put(d, DATA + i * 20, "IIIII", 0x3400, 0, 0, 0x3300, 0x3400)
                zero_string(d, DATA + 0x300, b"OwnedDependency.dll")
                if not many_libraries:
                    for i in range(MAX_IMPORT_SYMBOLS + 1):
                        put(d, DATA + 0x400 + i * 4, "I", 0x80000003)
                info = self.expect(d, "parser limit exceeded")
                self.assertEqual(32 if many_libraries else 1, info.library_count)
                self.assertEqual(0 if many_libraries else MAX_IMPORT_SYMBOLS, info.symbol_count)
                if not many_libraries:
                    put(d, DATA + 0x400 + MAX_IMPORT_SYMBOLS * 4, "I", 0)
                    complete = self.expect(d, "imports unsupported")
                    self.assertEqual(MAX_IMPORT_SYMBOLS, complete.symbol_count)

    def test_preferred_only_relocations(self):
        d = make_image()
        directory(d, 5, 0x3100, 12)
        put(d, DATA + 0x100, "IIHH", 0x1000, 12, 0x3008, 0)
        put(d, 0x208, "I", BASE + 0x2020)
        status, im = self.map(d)
        self.assertEqual("ok", status)
        self.assertEqual(BASE + 0x2020, struct.unpack("<I", c.string_at(im.bytes + 0x1008, 4))[0])
        put(d, DATA + 0x104, "I", 20)
        self.expect(d, "malformed PE")
        put(d, DATA + 0x104, "I", 12)
        put(d, DATA + 0x108, "H", 0x3050)
        self.expect(d, "malformed PE")

    def test_legacy_load_config_at_default_and_alternate_bases(self):
        d = make_image()
        add_legacy_load_config(d)
        status, image = self.map(d)
        self.assertEqual("ok", status)
        self.assertEqual(bytes(d[RD + 0xc0:RD + 0x108]), c.string_at(image.bytes + 0x20c0, 72))
        base = BASE + 0x200000
        status, image = self.bind(d, base=base)
        self.assertEqual("ok", status)
        self.assertEqual((base + 0x3130, base + 0x2190, 2),
                         struct.unpack("<III", c.string_at(image.bytes + 0x20fc, 12)))
        self.assertEqual((0x1000, 0x1010), struct.unpack("<II", c.string_at(image.bytes + 0x2190, 8)))
        self.assertEqual(b"\xdf\x9b\x57\x13", c.string_at(image.bytes + 0x3130, 4))

    def test_legacy_load_config_refuses_unknown_or_invalid_metadata(self):
        d = make_image()
        add_legacy_load_config(d)
        for offset, value, reason in (
                (RD + 0xc0, 96, "unsupported data directory"),
                (RD + 0xcc, 1, "unsupported data directory"),
                (RD + 0xfc, BASE + 0x1000, "malformed PE"),
                (RD + 0x100, BASE + 0x3190, "malformed PE"),
                (RD + 0x104, 4097, "parser limit exceeded"),
                (RD + 0x194, 0x1000, "malformed PE"),
                (RD + 0x194, 0x2000, "malformed PE")):
            with self.subTest(offset=offset, value=value):
                bad = bytearray(d)
                put(bad, offset, "I", value)
                self.expect(bad, reason)
        for flag, reason in ((0x4000, "unsupported data directory"), (0x0400, "malformed PE")):
            bad = bytearray(d)
            put(bad, OPT + 70, "H", flag)
            self.expect(bad, reason)
        bad = bytearray(d)
        directory(bad, 10, 0x21c0, 64)
        put(bad, RD + 0x1c0, "I", 72)
        self.expect(bad, "malformed PE")
        for displacement in (0x188, 0x18a):
            bad = bytearray(d)
            put(bad, DATA + displacement, "H", 0)
            self.assertEqual("invalid image relocation", self.bind(bad, base=BASE + 0x200000)[0])

    def test_only_named_approved_iat_words_change(self):
        for lookup in (0x3080, 0):  # Unbound IAT is a lookup source when OriginalFirstThunk is absent.
            with self.subTest(lookup=lookup):
                d = make_image()
                add_named_clock(d)
                put(d, DATA, "I", lookup)
                put(d, SECTIONS + 80 + 36, "I", 0x40000040)  # Short-lived construction of a read-only IAT.
                status, im = self.bind(d)
                self.assertEqual("ok", status)
                expected = bytearray(d[DATA:DATA + 0x200])
                put(expected, 0x60, "I", CLOCK_BINDING[2])
                self.assertEqual(bytes(expected), c.string_at(im.bytes + 0x3000, 0x200))
                self.assertFalse(self.lib.skse_pe_allows(c.byref(im), im.base + 0x3060, 4, W))
                self.assertTrue(self.lib.skse_pe_allows(c.byref(im), im.base + 0x3060, 4, R))
                self.expect(d, "imports unsupported")

    def test_mixed_unknown_or_ordinal_gets_no_partial_image(self):
        d = make_image()
        add_named_clock(d)
        directory(d, 12, 0x3060, 12)
        zero_string(d, DATA + 0xc2, b"OtherClock")
        for second, result in [(0x30c0, "import absent from the service list"),
                               (0x8000002a, "ordinal imports unsupported")]:
            with self.subTest(second=second):
                for offset in (0x64, 0x84):
                    put(d, DATA + offset, "I", second)
                self.assertEqual(result, self.bind(d)[0])
        add_named_clock(d)
        self.assertEqual("import absent from the service list", self.bind(d, bindings=())[0])
        for lib, symbol in [(b"KernelBase.dll", b"GetTickCount"), (b"KERNEL32.dll", b"gettickcount")]:
            self.assertEqual("import absent from the service list", self.bind(d, [(lib, symbol, 0x70000000)])[0])
        for table in [[CLOCK_BINDING, (b"kernel32.DLL", CLOCK_BINDING[1], 0x70000000)],
                      [(b"KERNEL32.DLL", b"GetTickCount", BASE + 0x3200)],
                      [(b"KERNEL32.DLL", b"GetTickCount", 0)]]:
            self.assertEqual("bad argument", self.bind(d, table)[0])

    def test_entire_iat_extent_and_bound_import_rejected(self):
        for address, size, reason in [(0, 0, "imports unsupported"), (0x3060, 4, "malformed PE"),
                                      (0x3060, 12, "malformed PE"), (0x3062, 8, "malformed PE"),
                                      (0x100, 8, "malformed PE"), (0x1020, 8, "malformed PE"),
                                      (0x2060, 8, "malformed PE"), (0x3200, 8, "malformed PE")]:
            with self.subTest(address=address, size=size):
                d = make_image()
                add_named_clock(d)
                directory(d, 12, address, size)
                self.assertEqual(reason, self.bind(d)[0])
        d = make_image()
        add_named_clock(d)
        put(d, DATA + 4, "I", 123)
        self.assertEqual("bound imports unsupported", self.bind(d)[0])
        put(d, DATA + 4, "I", 0)
        for offset, changed in [(0x60, 0x77123456), (0x64, 0x30f0)]:
            with self.subTest(offset=offset):
                b = bytearray(d)
                put(b, DATA + offset, "I", changed)
                self.assertEqual("bound imports unsupported", self.bind(b)[0])
        directory(d, 1, 0, 0)
        self.assertEqual("malformed PE", self.bind(d)[0])

    def test_two_reviewed_modules_and_duplicate_iat_ownership(self):
        d = make_image()
        add_named_clock(d)
        directory(d, 1, 0x3000, 60)
        directory(d, 12, 0x3060, 16)
        put(d, DATA + 20, "IIIII", 0x3090, 0, 0, 0x30d0, 0x3068)
        for offset in [0x68, 0x90]:
            put(d, DATA + offset, "II", 0x30c0, 0)
        zero_string(d, DATA + 0xc2, b"OtherClock")
        zero_string(d, DATA + 0xd0, b"OwnedOther.dll")
        self.assertEqual("import absent from the service list", self.bind(d)[0])
        table = [CLOCK_BINDING, (b"ownedother.DLL", b"OtherClock", 0x60000118)]
        status, im = self.bind(d, table)
        self.assertEqual("ok", status)
        self.assertEqual((CLOCK_BINDING[2], 0, 0x60000118, 0), struct.unpack("<4I", c.string_at(im.bytes + 0x3060, 16)))
        put(d, DATA + 20 + 16, "I", 0x3060)
        self.assertEqual("malformed PE", self.bind(d, table)[0])

    def test_entry_only_by_explicit_option_tls_delay_clr_stay_refused(self):
        d = make_image()
        put(d, OPT + 16, "I", 0x1018)
        self.assertEqual("DLL entry point unsupported", self.bind(d)[0])
        self.assertEqual("bad argument", self.bind(d, flags=2)[0])
        status, im = self.bind(d, flags=1)
        self.assertEqual("ok", status)
        self.assertEqual(BASE + 0x1018, im.entry_va)
        self.expect(d, "DLL entry point unsupported")
        for which, text in [(9, "TLS unsupported"), (13, "delay imports unsupported"), (14, "CLR unsupported")]:
            b = bytearray(d)
            directory(b, which, 0x3100, 16)
            self.assertEqual(text, self.bind(b, flags=1)[0])

    def test_alternate_guest_base_adjusts_only_owned_words(self):
        d = make_image()
        add_code_relocation(d)
        directory(d, 5, 0x3100, 24)
        put(d, DATA + 0x10c, "IIHH", 0x3000, 12, 0x3004, 0)
        put(d, DATA + 4, "I", BASE + 0x3200)
        put(d, OPT + 16, "I", 0x1014)
        ok, original = self.bind(d, flags=1, base=BASE)
        self.assertEqual("ok", ok)
        original_bytes = c.string_at(original.bytes, original.size)
        for requested in [0x01000000, 0x34000000, 0xffff0000]:
            with self.subTest(requested=requested):
                status, im = self.bind(d, flags=1, base=requested)
                self.assertEqual("ok", status)
                self.assertEqual((requested, requested + 0x1000, requested + 0x1010, requested + 0x1014),
                                 (im.base, im.query_va, im.load_va, im.entry_va))
                expected = bytearray(original_bytes)
                put(expected, 0x1008, "I", requested + 0x2040)
                put(expected, 0x3004, "I", requested + 0x3200)
                self.assertEqual(bytes(expected), c.string_at(im.bytes, im.size))
                self.assertTrue(self.lib.skse_pe_allows(c.byref(im), requested + 0x3200, 4, W))
                self.assertFalse(self.lib.skse_pe_allows(c.byref(im), BASE + 0x3200, 4, W))
                self.assertNotEqual(original.bytes, im.bytes)
                c.c_ubyte.from_address(im.bytes + 0x3200).value = 42
                self.assertEqual(b"\0", c.string_at(original.bytes + 0x3200, 1))
        self.assertEqual(original_bytes, c.string_at(original.bytes, original.size))

    def test_alternate_base_rejects_missing_unsupported_overlapping_and_cross_section(self):
        d = make_image()
        dest = 0x20000000
        self.assertEqual("ok", self.bind(d, base=BASE)[0])
        self.assertEqual("relocations required at alternate base", self.bind(d, base=dest)[0])
        for bas in [0, 1, dest + 1]:
            self.assertEqual("bad argument", self.bind(d, base=bas)[0])
        b = bytearray(d)
        put(b, OPT + 56, "I", 0x20000)
        self.assertEqual("bad argument", self.bind(b, base=0xffff0000)[0])
        add_code_relocation(d)
        b = bytearray(d)
        put(b, NT + 22, "H", 0x2103)
        self.assertEqual("relocations required at alternate base", self.bind(b, base=dest)[0])
        b = bytearray(d)
        put(b, DATA + 0x108, "H", 0)
        self.assertEqual("relocations required at alternate base", self.bind(b, base=dest)[0])
        for item, reason in [(0xa008, "unsupported relocation type"), (0x303e, "invalid image relocation")]:
            b = bytearray(d)
            put(b, DATA + 0x108, "H", item)
            self.assertEqual(reason, self.bind(b, base=dest)[0])
        b = bytearray(d)
        put(b, DATA + 0x10a, "H", 0x3008)
        self.assertEqual("overlapping relocation words", self.bind(b, base=dest)[0])
        self.assertEqual("ok", self.map(b)[0])
        b = bytearray(d)
        put(b, OPT + 28, "I", 0x01010000)
        b[0x208:0x20d] = b"\x01" * 5  # Both overlapping words point to 0x01010101.
        put(b, DATA + 0x108, "HH", 0x3009, 0x3008)
        self.assertEqual("overlapping relocation words", self.bind(b, base=dest)[0])
        b = bytearray(d)
        put(b, SECTIONS + 40 + 8, "I", 0x1000)
        put(b, DATA + 0x100, "I", 0x2000)
        put(b, DATA + 0x108, "H", 0x3ffe)  # Last two bytes of R directly precede RW.
        self.assertEqual("invalid image relocation", self.bind(b, base=dest)[0])
        b = bytearray(d)
        put(b, 0x208, "I", BASE - 1)
        self.assertEqual("invalid image relocation", self.bind(b, base=dest)[0])
        b = bytearray(d)
        put(b, DATA + 0x100, "I", 0x3000)
        put(b, DATA + 0x108, "H", 0x3200)  # BSS has no initialized word to relocate.
        self.assertEqual("invalid image relocation", self.bind(b, base=dest)[0])

    def test_alternate_base_iat_address_and_token_have_distinct_owners(self):
        d = make_image()
        add_named_clock(d)
        add_code_relocation(d, 0x3060)
        dest = 0x28000000
        status, im = self.bind(d, base=dest)
        self.assertEqual("ok", status)
        self.assertEqual((dest + 0x3060,), struct.unpack("<I", c.string_at(im.bytes + 0x1008, 4)))
        self.assertEqual((CLOCK_BINDING[2], 0), struct.unpack("<II", c.string_at(im.bytes + 0x3060, 8)))
        self.assertEqual("bad argument", self.bind(d, [(CLOCK_BINDING[0], CLOCK_BINDING[1], dest + 0x80)], base=dest)[0])
        put(d, DATA + 0x100, "I", 0x3000)
        for item in [0x3060, 0x305e, 0x3064]:
            put(d, DATA + 0x108, "H", item)
            self.assertEqual("relocation overlaps import address table", self.bind(d, base=dest)[0])

    def test_real_llvm_authored_i386_dll_and_export_addresses(self):
        clang = find_tool("clang", "clang-18")
        lld = find_tool("lld-link", "lld-link-18")
        readobj = find_tool("llvm-readobj", "llvm-readobj-18")
        if not clang or not lld or not readobj:
            self.skipTest("LLVM clang, lld-link, and llvm-readobj are required for the linker reference")
        src = self.work / "owned_probe.c"
        src.write_text(
            "typedef struct { unsigned skse, runtime; } Info;\n"
            "__declspec(dllexport) int __cdecl SKSEPlugin_Query(const Info *i, void *o) "
            "{ return i && o && i->runtime == 0x09200000; }\n"
            "__declspec(dllexport) int __cdecl SKSEPlugin_Load(const Info *i) { return i != 0; }\n",
            encoding="ascii")
        subprocess.run([clang, "--target=i686-pc-windows-msvc", "-std=c11", "-O1",
                        "-fno-stack-protector", "-c", src, "-o", "probe.obj"], cwd=self.work, check=True)
        subprocess.run([lld, "/dll", "/noentry", "/nodefaultlib", "/machine:x86", "/timestamp:0",
                        "/base:0x10000000", "/out:probe.dll", "probe.obj"], cwd=self.work, check=True)
        dll = self.work / "probe.dll"
        reference = subprocess.check_output([readobj, "--coff-exports", dll], text=True)
        status, image = self.map(dll.read_bytes())
        self.assertEqual("ok", status)
        self.assertEqual(BASE, image.base)
        for name, va in [("SKSEPlugin_Query", image.query_va), ("SKSEPlugin_Load", image.load_va)]:
            self.assertRegex(reference, rf"Name: {name}\s+RVA: 0x{va - image.base:X}\b")
            self.assertTrue(self.lib.skse_pe_allows(c.byref(image), va, 1, X))
            self.assertFalse(self.lib.skse_pe_allows(c.byref(image), va, 1, W))

    def test_real_llvm_import_directory_without_loading_either_fixture(self):
        clang = find_tool("clang", "clang-18")
        lld = find_tool("lld-link", "lld-link-18")
        readobj = find_tool("llvm-readobj", "llvm-readobj-18")
        if not clang or not lld or not readobj:
            self.skipTest("LLVM clang, lld-link, and llvm-readobj are required for the import reference")
        src = self.work / "dep.c"
        src.write_text("__declspec(dllexport) int OwnedExample(void) { return 123; }", encoding="ascii")
        main = self.work / "consumer.c"
        main.write_text(
            "__declspec(dllimport) int OwnedExample(void);"
            "__declspec(dllexport) int SKSEPlugin_Query(void *x, void *y) { return x && y; }"
            "__declspec(dllexport) int SKSEPlugin_Load(void *x) { return x && OwnedExample(); }",
            encoding="ascii")
        compiler = [clang, "--target=i686-pc-windows-msvc", "-O1", "-fno-stack-protector", "-c"]
        linker = [lld, "/dll", "/noentry", "/nodefaultlib", "/machine:x86", "/timestamp:0"]
        subprocess.run([*compiler, src, "-o", "dep.obj"], cwd=self.work, check=True)
        subprocess.run([*linker, "/out:OwnedDependency.dll", "/implib:dep.lib", "dep.obj"],
                       cwd=self.work, check=True)
        subprocess.run([*compiler, main, "-o", "consumer.obj"], cwd=self.work, check=True)
        subprocess.run([*linker, "/out:consumer.dll", "consumer.obj", "dep.lib"], cwd=self.work, check=True)
        dll = self.work / "consumer.dll"
        reference = subprocess.check_output([readobj, "--coff-imports", dll], text=True)
        report = self.expect(dll.read_bytes(), "imports unsupported")
        self.assertEqual((1, 1), (report.library_count, report.symbol_count))
        self.assertEqual(b"OwnedDependency.dll", report.libraries[0].value)
        self.assertEqual(b"OwnedExample", report.symbols[0].name)
        self.assertIn("Name: OwnedDependency.dll", reference)
        self.assertIn("Symbol: OwnedExample", reference)

    def test_llvm_authored_clock_binding_matches_linker_iat(self):
        clang = find_tool("clang", "clang-18")
        lld = find_tool("lld-link", "lld-link-18")
        dlltool = find_tool("llvm-dlltool", "llvm-dlltool-18")
        if not clang or not lld or not dlltool:
            self.skipTest("LLVM clang, lld-link and llvm-dlltool are required for the clock reference")
        # Only the ABI declaration goes into this private import library.
        # No Windows library or system DLL is used to create or inspect the image.
        (self.work / "clock.def").write_text("LIBRARY KERNEL32.dll\nEXPORTS\nGetTickCount@0\n", encoding="ascii")
        (self.work / "clock.c").write_text(
            "__declspec(dllimport) unsigned __stdcall GetTickCount(void);"
            "__declspec(dllexport) int __cdecl SKSEPlugin_Query(void *x, void *y) { return x && y; }"
            "__declspec(dllexport) int __cdecl SKSEPlugin_Load(void *x) { return x && GetTickCount(); }",
            encoding="ascii")
        subprocess.run([dlltool, "-m", "i386", "-k", "-d", "clock.def", "-l", "clock.lib"], cwd=self.work, check=True)
        subprocess.run([clang, "--target=i686-pc-windows-msvc", "-O1", "-fno-stack-protector", "-c",
                        "clock.c", "-o", "clock.obj"], cwd=self.work, check=True)
        subprocess.run([lld, "/dll", "/noentry", "/nodefaultlib", "/machine:x86", "/timestamp:0",
                        "/out:clock_sample.dll", "clock.obj", "clock.lib"], cwd=self.work, check=True)
        source = (self.work / "clock_sample.dll").read_bytes()
        report = self.expect(source, "imports unsupported")
        self.assertEqual((1, 1), (report.library_count, report.symbol_count))
        self.assertEqual((b"KERNEL32.dll", b"GetTickCount"), (report.libraries[0].value, report.symbols[0].name))
        status, im = self.bind(source)
        self.assertEqual("ok", status)
        nt = struct.unpack_from("<I", source, 60)[0]
        iat_rva, size = struct.unpack_from("<II", source, nt + 24 + 96 + 12 * 8)
        self.assertEqual(8, size)
        self.assertEqual((CLOCK_BINDING[2], 0), struct.unpack("<II", c.string_at(im.bytes + iat_rva, size)))
        self.assertFalse(self.lib.skse_pe_allows(c.byref(im), im.base + iat_rva, size, X))
        new_base = 0x33000000
        status, moved = self.bind(source, base=new_base)
        self.assertEqual("ok", status)
        self.assertEqual((CLOCK_BINDING[2], 0), struct.unpack("<II", c.string_at(moved.bytes + iat_rva, 8)))
        reloc_rva, reloc_size = struct.unpack_from("<II", source, nt + 24 + 96 + 5 * 8)
        records = c.string_at(im.bytes + reloc_rva, reloc_size)
        offset = 0
        calls_to_iat = 0
        while offset < len(records):
            page, block = struct.unpack_from("<II", records, offset)
            for (entry,) in struct.iter_unpack("<H", records[offset + 8:offset + block]):
                if entry >> 12 != 3:
                    continue
                operand = page + (entry & 0xfff)
                old_word, = struct.unpack("<I", c.string_at(im.bytes + operand, 4))
                if old_word == im.base + iat_rva:
                    self.assertTrue(self.lib.skse_pe_allows(c.byref(im), im.base + operand, 4, X))
                    new_word, = struct.unpack("<I", c.string_at(moved.bytes + operand, 4))
                    self.assertEqual(new_base + iat_rva, new_word)
                    calls_to_iat += 1
            offset += block
        self.assertGreaterEqual(calls_to_iat, 1)

    def test_llvm_i386_real_data_and_rdata_pointers_at_two_guest_bases(self):
        clang = find_tool("clang", "clang-18")
        lld = find_tool("lld-link", "lld-link-18")
        readobj = find_tool("llvm-readobj", "llvm-readobj-18")
        if not clang or not lld or not readobj:
            self.skipTest("LLVM clang, lld-link and llvm-readobj are required for the relocation reference")
        src = self.work / "positions.c"
        src.write_text(
            "__declspec(dllexport) unsigned OwnedValue = 0x1234abcdu;\n"
            "__declspec(dllexport) const char OwnedLabel[] = \"Portable\";\n"
            "__declspec(dllexport) const char *const OwnedROPointer = OwnedLabel;\n"
            "__declspec(dllexport) unsigned *OwnedRWPointer = &OwnedValue;\n"
            "__declspec(dllexport) int __cdecl SKSEPlugin_Query(void *x, void *y) { return x && y; }\n"
            "__declspec(dllexport) int __cdecl SKSEPlugin_Load(void *x) { return x && *OwnedRWPointer == 0x1234abcdu; }\n",
            encoding="ascii")
        subprocess.run([clang, "--target=i686-pc-windows-msvc", "-O1", "-fno-stack-protector", "-c",
                        src, "-o", "positions.obj"], cwd=self.work, check=True)
        subprocess.run([lld, "/dll", "/noentry", "/nodefaultlib", "/machine:x86", "/timestamp:0",
                        "/base:0x10000000", "/dynamicbase", "/fixed:no", "/out:positions.dll", "positions.obj"],
                       cwd=self.work, check=True)
        dll = self.work / "positions.dll"
        exports = subprocess.check_output([readobj, "--coff-exports", dll], text=True)
        rvas = {name: int(value, 16) for name, value in re.findall(r"Name: (\w+)\s+RVA: 0x([0-9A-Fa-f]+)", exports)}
        relocs = subprocess.check_output([readobj, "--coff-basereloc", dll], text=True)
        self.assertIn("HIGHLOW", relocs)
        source = dll.read_bytes()
        for chosen in [BASE, 0x23000000]:
            with self.subTest(chosen=chosen):
                status, im = self.bind(source, bindings=(), base=chosen)
                self.assertEqual("ok", status)
                self.assertEqual(chosen + rvas["SKSEPlugin_Query"], im.query_va)
                for word, target, writable in [("OwnedROPointer", "OwnedLabel", False),
                                               ("OwnedRWPointer", "OwnedValue", True)]:
                    stored, = struct.unpack("<I", c.string_at(im.bytes + rvas[word], 4))
                    self.assertEqual(chosen + rvas[target], stored)
                    self.assertEqual(writable, bool(self.lib.skse_pe_allows(c.byref(im), chosen + rvas[word], 4, W)))
                    self.assertFalse(self.lib.skse_pe_allows(c.byref(im), chosen + rvas[word], 4, X))
                self.assertEqual(b"Portable\0", c.string_at(im.bytes + rvas["OwnedLabel"], 9))
                self.assertEqual(0x1234abcd, struct.unpack("<I", c.string_at(im.bytes + rvas["OwnedValue"], 4))[0])


if __name__ == "__main__":
    unittest.main()
