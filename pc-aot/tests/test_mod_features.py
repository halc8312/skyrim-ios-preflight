"""Detect first-wave MOD features by marker names in ordered Data roots (authored files)."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


def versioned_pe(file_version, *, pe64=False, second_language=None):
    pe, raw, rva = 0x80, 0x200, 0x1000
    table = 112 if pe64 else 96
    optional_size = table + 16 * 8
    payload = bytearray(0x400)
    payload[:2] = b"MZ"
    struct.pack_into("<I", payload, 60, pe)
    struct.pack_into("<4sHHIIIHH", payload, pe, b"PE\0\0", 0x8664 if pe64 else 0x14C,
                     1, 0, 0, 0, optional_size, 0x2002)
    optional = pe + 24
    struct.pack_into("<H", payload, optional, 0x20B if pe64 else 0x10B)
    struct.pack_into("<I", payload, optional + table - 4, 16)
    struct.pack_into("<II", payload, optional + table + 16, rva, 0x200)
    struct.pack_into("<8sIIII", payload, optional + optional_size, b".rsrc", 0x200, rva, 0x200, raw)

    def node(offset, entries):
        struct.pack_into("<H", payload, raw + offset + 14, len(entries))
        for i, entry in enumerate(entries):
            struct.pack_into("<II", payload, raw + offset + 16 + i * 8, *entry)

    node(0, [(16, 0x80000020)])
    node(0x20, [(1, 0x80000040)])
    languages = [(1033, 0x70)]
    if second_language is not None:
        languages.append((1041, 0x80))
    node(0x40, languages)

    def write_version(entry, data, version):
        at = raw + data
        struct.pack_into("<II", payload, raw + entry, rva + data, 92)
        struct.pack_into("<HHH", payload, at, 92, 52, 0)
        payload[at + 6:at + 38] = "VS_VERSION_INFO\0".encode("utf-16le")
        major, minor, build, revision = version
        struct.pack_into("<6I", payload, at + 40, 0xFEEF04BD, 0x10000,
                         major << 16 | minor, build << 16 | revision, 0x98765432, 0)

    write_version(0x70, 0x100, file_version)
    if second_language is not None:
        write_version(0x80, 0x160, second_language)
    return payload


@unittest.skipUnless(os.name == "posix", "Requires POSIX paths")
class FeatureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / "features"
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        subprocess.run([cc, *flags, ROOT / "tests/native/test_mod_features.c", PORT / "mod_features.c",
                        PORT / "mod_plugins.c", PORT / "mod_deploy.c", PORT / "mod_archive.c",
                        "-lz", "-o", cls.binary], check=True, timeout=60)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = Path(tmp.name)

    def root(self, name, files):
        base = self.top / name
        base.mkdir()
        for path in files:
            target = base / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"x")
        return base

    def scan(self, *roots):
        out = subprocess.check_output([self.binary, *roots], text=True, timeout=10).split()
        return int(out[0]), {item.split(":")[0]: tuple(map(int, item.split(":")[1:])) for item in out[1:]}

    def test_detection_by_marker_names_ignores_case_and_spacing(self):
        a = self.root("a", ["SKSE/Plugins/Fuz Ro D-oh.DLL", "SKSE/Plugins/StorageUtil.dll",
                            "SKSE/Plugins/ShowRaceMenuPrecacheKiller.dll", "SKSE/Plugins/hdtPhysicsExtensions.dll",
                            "SKSE/Plugins/hdtHighHeels.dll", "SKSE/Plugins/SKSE_EnhancedCamera.dll",
                            "SKSE/Plugins/SafetyLoad.dll", "SKSE/Plugins/chargen.dll", "SKSE/Plugins/nioverride.dll",
                            "enhancedcharacteredit.ESP", "SKSE/Plugins/unrelated.dll"])
        unreadable, found = self.scan(a)
        self.assertEqual(unreadable, 0)
        self.assertEqual(found, {key: (1, 0) for key in (
            "fuz", "papyrusutil", "precache", "racemenu", "nioverride", "ece", "enhancedcamera",
            "safetyload", "hdtphysics", "hdthighheels")})

    def test_non_dll_files_and_wrong_folders_do_not_match(self):
        a = self.root("a", ["SKSE/Plugins/fuz.ini", "SKSE/Plugins/fuz.dll.txt", "Plugins/fuz.dll",
                            "SKSE/fuz.dll", "SKSE/Plugins/sub/fuz.dll", "ece.esp"])
        self.assertEqual(self.scan(a), (0, {}))
        self.assertEqual(self.scan(), (0, {}))
        self.assertEqual(self.scan(str(self.top / "missing")), (0, {}))

    def test_duplicate_providers_report_the_later_root(self):
        a = self.root("a", ["SKSE/Plugins/nioverride.dll"])
        b = self.root("b", ["SKSE/Plugins/fuz.dll"])
        c = self.root("c", ["SKSE/Plugins/NiOverride.dll"])
        self.assertEqual(self.scan(a, b, c), (0, {"nioverride": (2, 2), "fuz": (1, 1)}))

    def nioverride(self, *roots, first_is_original=False):
        option = "--nioverride-original" if first_is_original else "--nioverride"
        return subprocess.check_output([self.binary, option, *roots], text=True, timeout=10).split()

    def test_nioverride_protected_original_matches_the_real_overlay(self):
        original_name = "SKSE/Plugins/NiOverride.dll"
        mod_name = "skse/plugins/nioverride.dll"
        game = self.root("game", [f"Data/{original_name}"])
        original_data = game / "Data"
        mod_data = self.root("mod", [mod_name])
        original_dll = original_data / original_name
        mod_dll = mod_data / mod_name
        mod_bytes = versioned_pe((3, 4, 5, 2))
        mod_dll.write_bytes(mod_bytes)

        for i, (base_bytes, version) in enumerate(((versioned_pe((3, 4, 4, 0)), ["3.4.4.0"]),
                                                   (b"missing version metadata", []))):
            with self.subTest(version=version):
                original_dll.write_bytes(base_bytes)
                self.assertEqual(self.nioverride(original_data, mod_data, first_is_original=True),
                                 ["2", "0", *version])
                view = self.top / f"view-{i}"
                subprocess.run([self.binary, "--deploy", game, view, mod_data], check=True, timeout=10)
                self.assertEqual((view / "Data" / original_name).read_bytes(), base_bytes)
                self.assertEqual(original_dll.read_bytes(), base_bytes)
                self.assertEqual(mod_dll.read_bytes(), mod_bytes)

        original_dll.write_bytes(versioned_pe((3, 4, 4, 0)))
        original_dll.chmod(0)
        try:
            if not os.access(original_dll, os.R_OK):
                self.assertEqual(self.nioverride(original_data, mod_data, first_is_original=True), ["2", "0"])
                view = self.top / "view-unreadable"
                result = subprocess.run([self.binary, "--deploy", game, view, mod_data], capture_output=True,
                                        timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(view.exists())
        finally:
            original_dll.chmod(0o600)

        original_dll.unlink()
        self.assertEqual(self.nioverride(original_data, mod_data, first_is_original=True), ["1", "1", "3.4.5.2"])
        view = self.top / "view-unprotected"
        subprocess.run([self.binary, "--deploy", game, view, mod_data], check=True, timeout=10)
        deployed = view / "Data" / Path(original_name).parent / Path(mod_name).name
        self.assertEqual(deployed.read_bytes(), mod_bytes)
        self.assertFalse(original_dll.exists())

    def test_nioverride_uses_the_exact_name_and_the_winning_files_metadata(self):
        a = self.root("racemenu", ["SKSE/Plugins/NIOverride.dll"])
        b = self.root("standalone", ["skse/plugins/nioverride.dll"])
        c = self.root("other", ["SKSE/Plugins/archive_nioverride.dll"])
        first, second = a / "SKSE/Plugins/NIOverride.dll", b / "skse/plugins/nioverride.dll"
        first.write_bytes(versioned_pe((3, 4, 5, 2)))
        second.write_bytes(versioned_pe((3, 4, 4, 0), pe64=True, second_language=(3, 4, 4, 0)))
        self.assertEqual(self.nioverride(a, b, c), ["2", "1", "3.4.4.0"])
        self.assertEqual(self.nioverride(b, a, c), ["2", "1", "3.4.5.2"])
        self.assertEqual(self.nioverride(c), ["0", "-1"])

        second.write_bytes(b"No fixed metadata")
        self.assertEqual(self.nioverride(a, b), ["2", "1"])

    def test_nioverride_refuses_ambiguous_and_out_of_bounds_metadata(self):
        a = self.root("a", ["SKSE/Plugins/nioverride.dll"])
        dll = a / "SKSE/Plugins/nioverride.dll"
        ambiguous = versioned_pe((1, 2, 3, 4), second_language=(1, 2, 3, 5))
        forged = bytearray(versioned_pe((1, 2, 3, 4)))
        struct.pack_into("<I", forged, 0x270, 0xFFFFFF00)
        for data in (ambiguous, forged, versioned_pe((1, 2, 3, 4))[:-1]):
            with self.subTest(size=len(data)):
                dll.write_bytes(data)
                self.assertEqual(self.nioverride(a), ["1", "0"])

    def test_nioverride_skips_links_and_case_aliases(self):
        actual = self.root("actual", ["SKSE/Plugins/nioverride.dll"])
        (actual / "SKSE/Plugins/nioverride.dll").write_bytes(versioned_pe((1, 0, 0, 0)))
        linked = self.root("linked", ["SKSE/Plugins/fuz.dll"])
        (linked / "SKSE/Plugins/nioverride.dll").symlink_to(actual / "SKSE/Plugins/nioverride.dll")
        self.assertEqual(self.nioverride(linked), ["0", "-1"])
        self.assertEqual(self.scan(linked), (1, {"fuz": (1, 0)}))
        alias = actual / "SKSE/Plugins/NiOverride.dll"
        if not alias.exists():
            alias.write_bytes(b"other version")
            self.assertEqual(self.nioverride(actual), ["0", "-1"])

    def test_links_and_ambiguous_aliases_are_skipped(self):
        real = self.root("real", ["SKSE/Plugins/fuz.dll"])
        linked = self.root("linked", [])
        (linked / "SKSE").symlink_to(real / "SKSE")
        alias = self.root("alias", ["SKSE/Plugins/fuz.dll"])
        dll_link = self.root("dll_link", [])
        (dll_link / "SKSE/Plugins").mkdir(parents=True)
        (dll_link / "SKSE/Plugins/fuz.dll").symlink_to(real / "SKSE/Plugins/fuz.dll")
        self.assertEqual(self.scan(linked, dll_link), (1, {}))
        alternate = alias / "skse/plugins/fuz.dll"
        if alternate.exists():
            self.assertEqual(self.scan(alias), (0, {"fuz": (1, 0)}))
        else:
            alternate.parent.mkdir(parents=True)
            alternate.write_bytes(b"x")
            self.assertEqual(self.scan(alias), (1, {}))

    def test_verified_dll_features_require_the_original_file_name(self):
        for option, name in (("precache", "ShowRacemenu_PrecacheKiller.DLL"),
                             ("safety", "SaFeTyLoAd.DLL")):
            selected = f"skse/Plugins/{name}"
            unrelated = f"SKSE/Plugins/alternate-{name}"
            a = self.root(f"{option}-selected", [selected])
            b = self.root(f"{option}-generic", [unrelated])
            c = self.root(f"{option}-ambiguous", [selected])
            alternate = c / Path(selected).with_name(name.lower())
            if alternate.exists():
                c_result = ["1", str(c / selected)]
            else:
                alternate.write_bytes(b"x")
                c_result = ["-1"]
            d = self.root(f"{option}-link", [unrelated])
            (d / "SKSE/Plugins" / name).symlink_to(a / selected)
            for root, expected in ((a, ["1", str(a / selected)]), (b, ["0"]), (c, c_result), (d, ["-1"])):
                with self.subTest(option=option, root=root.name):
                    self.assertEqual(subprocess.check_output([self.binary, f"--{option}-path", root], text=True,
                                                             timeout=10).splitlines(), expected)


if __name__ == "__main__":
    unittest.main()
