"""Check a read-only MOD directory and an authored x86 plugin/helper pair on POSIX."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from test_pe_image import BASE, DATA, OPT, RD, SECTIONS, add_named_clock, directory, make_image, put, zero_string
from test_probe_local_file import supported_fixture

HERE = Path(__file__).resolve().parent


def primary(library=b"OwnedWorker.dll", symbol=b"OwnedCheck"):
    data = supported_fixture()
    add_named_clock(data)
    zero_string(data, DATA + 0x40, library)
    zero_string(data, DATA + 0xa2, symbol)
    # The Load result is returned by the actual imported helper function.
    data[0x200:0x207] = b"\xff\x15" + struct.pack("<I", BASE + 0x3060) + b"\xc3"
    return data


def helper(success=True):
    data = make_image()
    put(data, OPT + 28, "I", BASE + 0x100000)
    zero_string(data, RD + 0x40, b"OwnedWorker.dll")
    put(data, RD + 20, "II", 1, 1)
    put(data, RD + 0x60, "I", 0x1000)
    put(data, RD + 0x68, "I", 0x2080)
    put(data, RD + 0x70, "H", 0)
    zero_string(data, RD + 0x80, b"OwnedCheck")
    data[0x200:0x206] = b"\xb8" + struct.pack("<I", int(success)) + b"\xc3"
    return data


def add_second_import(data, name):
    directory(data, 1, 0x3000, 60)
    directory(data, 12, 0x3060, 16)
    put(data, DATA + 20, "IIIII", 0x3120, 0, 0, 0x3140, 0x3068)
    put(data, DATA + 0x120, "II", 0x30a0, 0)
    put(data, DATA + 0x68, "II", 0x30a0, 0)
    zero_string(data, DATA + 0x140, name)


def file_asset_fixture():
    data = supported_fixture()
    add_named_clock(data)
    directory(data, 12, 0x3060, 20)
    for table in (0x60, 0x80):
        put(data, DATA + table, "5I", 0x30a0, 0x30c0, 0x30e0, 0x3100, 0)
    for offset, name in ((0xa0, b"CreateFileA"), (0xc0, b"ReadFile"), (0xe0, b"CloseHandle"), (0x100, b"SetFilePointer")):
        zero_string(data, DATA + offset + 2, name)
    zero_string(data, RD + 0x120, b"data/Skse/Plugins/settings.INI")
    put(data, SECTIONS + 8, "I", 0x180)
    put(data, RD + 0x60, "I", 0x1080)
    code = bytearray(b"\x53")  # Preserve EBX across the Classic cdecl call.
    failure_jumps = []

    def push(value):
        code.extend(b"\x68" + struct.pack("<I", value))

    def call(slot):
        code.extend(b"\xff\x15" + struct.pack("<I", BASE + 0x3060 + slot * 4))

    def jump_to_failure(opcode):
        code.extend(bytes((opcode, 0)))
        failure_jumps.append(len(code) - 1)

    for value in (0, 0x80, 3, 0, 1, 0x80000000, BASE + 0x2120):
        push(value)
    call(0)
    code.extend(b"\x83\xf8\xff")  # Reject INVALID_HANDLE_VALUE.
    jump_to_failure(0x74)
    code.extend(b"\x89\xc3")  # EBX holds the descriptor.
    for value in (0, 0, 1):  # FILE_BEGIN, no high word, one byte forward.
        push(value)
    code.append(0x53)
    call(3)
    code.extend(b"\x83\xf8\x01")
    jump_to_failure(0x75)
    for value in (0, BASE + 0x3140, 1, BASE + 0x3144):
        push(value)
    code.append(0x53)
    call(1)
    code.extend(b"\x85\xc0")
    jump_to_failure(0x74)
    code.extend(b"\x80\x3d" + struct.pack("<I", BASE + 0x3144) + b"u")
    jump_to_failure(0x75)
    code.append(0x53)
    call(2)
    code.extend(b"\x5b\xc3")
    for displacement in failure_jumps:
        code[displacement] = len(code) - displacement - 1
    code.extend(b"\x31\xc0\x5b\xc3")
    data[0x280:0x280 + len(code)] = code
    return data


@unittest.skipIf(sys.platform == "win32", "Darwin/POSIX directory descriptors required")
class DirectoryProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            raise unittest.SkipTest("C compiler required")
        cls.tool = tempfile.TemporaryDirectory(prefix="skse-dir-tool-")
        cls.addClassCleanup(cls.tool.cleanup)
        cls.binary = Path(cls.tool.name) / "directory-probe"
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        sources = ["test_probe_directory.c", "probe_directory.c", "plugin_trial.c", "dependency_plan.c", "pe_image.c",
                   "x86_call.c", "x86_heap.c", "x86_winthread.c", "x86_wintext.c", "x86_winfile.c", "x86_winsync.c",
                   "x86_winenv.c", "papyrus_trial.c",
                   "x86_skse_records.c", "probe_asset_file.c"]
        subprocess.run([compiler, *flags, *(HERE / s for s in sources), "-o", cls.binary], check=True, timeout=90)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="skse-directory-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.selected = self.root / "selected"
        self.selected.mkdir()

    def scan(self, directory=None):
        data = subprocess.check_output([self.binary, directory if directory is not None else self.selected],
                                       text=True, timeout=8).splitlines()
        header = data[0].split("|")
        files = {fields[0]: fields[1:] for fields in (line.split("|") for line in data[1:])}
        self.assertEqual(int(header[1]), len(files))
        self.assertEqual(list(files), sorted(files, key=lambda path: path.encode()))
        return header, files

    def test_three_reachable_helpers_are_taken_only_from_selected_mod_folders(self):
        parent = self.selected / "Data/SKSE/Plugins"
        parent.mkdir(parents=True)
        root = primary(b"A.dll")
        add_second_import(root, b"B.dll")
        (self.selected / "Main.dll").write_bytes(root)
        for slot, name in enumerate(("A.dll", "B.dll", "Leaf.dll"), 1):
            data = helper()
            put(data, OPT + 28, "I", BASE + slot * 0x100000)
            if name != "Leaf.dll":
                add_named_clock(data)
                zero_string(data, DATA + 0x40, b"LEAF.dll")
                zero_string(data, DATA + 0xa2, b"OwnedCheck")
            (parent / name).write_bytes(data)
        header, records = self.scan()
        self.assertEqual(header, ["checked", "4", "1"])
        self.assertEqual(records["Main.dll"][:7], ["paired", "1", "1", "2", "A.dll", "B.dll", "yes"])
        self.assertEqual(records["Main.dll"][-1], "7")
        alternate = self.selected / "ignored"
        alternate.mkdir()
        (parent / "Leaf.dll").rename(alternate / "Leaf.dll")
        _, records = self.scan()
        self.assertEqual(records["Main.dll"][:6], ["missing", "1", "0", "2", "LEAF.dll", "A.dll"])

    def test_read_only_file_uses_the_selected_package(self):
        (self.selected / "AssetProbe.dll").write_bytes(file_asset_fixture())
        accessories = self.selected / "Data/SKSE/Plugins"
        accessories.mkdir(parents=True)
        asset = accessories / "Settings.ini"
        asset.write_bytes(b"Authored settings")
        self.assertEqual(self.scan()[1]["AssetProbe.dll"][6], "yes")
        self.assertEqual(asset.read_bytes(), b"Authored settings")
        outside = self.root / "Settings.ini"
        asset.rename(outside)
        asset.symlink_to(outside)
        self.assertEqual(self.scan()[1]["AssetProbe.dll"][6], "no")
        self.assertEqual(outside.read_bytes(), b"Authored settings")

    def test_pair_across_approved_paths_is_sorted_and_read_only(self):
        deep = self.selected / "dAtA/sKse/pLuGIns"
        deep.mkdir(parents=True)
        alternate = self.selected / "SKSE/Plugins"
        alternate.mkdir(parents=True)
        irrelevant = self.selected / "other/Nested"
        irrelevant.mkdir(parents=True)
        files = {self.selected / "日本語.DLL": supported_fixture(), deep / "Main.dll": primary(),
                 alternate / "OWNEDWORKER.Dll": helper(), self.selected / "bad.dll": b"truncated", irrelevant / "Other.dll": primary()}
        for path, content in files.items():
            path.write_bytes(content)
        mtimes = {path: path.stat().st_mtime_ns for path in files}
        header, records = self.scan()
        self.assertEqual(header, ["checked", "4", "2"])
        self.assertEqual(records["日本語.DLL"][:4], ["standalone", "1", "1", "0"])
        pair = records["dAtA/sKse/pLuGIns/Main.dll"]
        self.assertEqual(pair[:7], ["paired", "1", "1", "1", "OwnedWorker.dll", "", "yes"])
        self.assertEqual(pair[-1], "7")
        self.assertEqual(records["SKSE/Plugins/OWNEDWORKER.Dll"][0], "not-plugin")
        self.assertEqual(records["bad.dll"][0], "not-plugin")
        self.assertEqual((header, records), self.scan())
        self.assertEqual(mtimes, {path: path.stat().st_mtime_ns for path in files})
        for path, content in files.items():
            self.assertEqual(path.read_bytes(), content)
        # Changing this helper's return value prevents the plugin's Load from succeeding.
        (alternate / "OWNEDWORKER.Dll").write_bytes(helper(False))
        self.assertEqual(self.scan()[1]["dAtA/sKse/pLuGIns/Main.dll"][6], "no")

    def test_unresolved_multiple_and_duplicate_libraries(self):
        main = self.selected / "Main.dll"
        main.write_bytes(primary())
        self.assertEqual(self.scan()[1]["Main.dll"][:5], ["missing", "1", "0", "1", "OwnedWorker.dll"])
        (self.selected / "OwnedWorker.dll").write_bytes(helper())
        deep = self.selected / "SKSE/Plugins"
        deep.mkdir(parents=True)
        (deep / "ownedworker.DLL").write_bytes(helper())
        self.assertEqual(self.scan()[1]["Main.dll"][0], "ambiguous")
        (deep / "ownedworker.DLL").unlink()
        more = primary()
        add_second_import(more, b"Other.dll")
        main.write_bytes(more)
        self.assertEqual(self.scan()[1]["Main.dll"][:6], ["missing", "1", "0", "2", "Other.dll", ""])
        (self.selected / "Other.dll").write_bytes(helper())
        self.assertEqual(self.scan()[1]["Main.dll"][0], "dependency-limit")  # Two fixed helper images at one VA.
        # Windows case folding deduplicates distinct descriptors for the same library.
        same = primary()
        add_second_import(same, b"OWNEDworker.DLL")
        main.write_bytes(same)
        self.assertEqual(self.scan()[1]["Main.dll"][:4], ["paired", "1", "1", "1"])

    def test_declared_external_file_never_becomes_a_search_path(self):
        main = self.selected / "Main.dll"
        main.write_bytes(primary(b"../Outside.dll"))
        (self.root / "Outside.dll").write_bytes(helper())
        self.assertEqual(self.scan()[1]["Main.dll"][:5], ["invalid", "1", "0", "1", "../Outside.dll"])
        main.write_bytes(primary(b"main.DLL"))
        self.assertEqual(self.scan()[1]["Main.dll"][0], "self")
        main.write_bytes(primary())
        nested = helper()
        add_named_clock(nested)
        zero_string(nested, DATA + 0x40, b"Extra.dll")
        (self.selected / "OwnedWorker.dll").write_bytes(nested)
        self.assertEqual(self.scan()[1]["Main.dll"][:6], ["missing", "1", "0", "1", "Extra.dll", "OwnedWorker.dll"])

    def test_reserved_system_names_are_never_resolved_to_local_dlls(self):
        (self.selected / "main.dll").write_bytes(primary(b"Kernel32.dll", b"OwnedCheck"))
        (self.selected / "KERNEL32.dll").write_bytes(helper())
        row = self.scan()[1]["main.dll"]
        self.assertEqual(row[:7], ["standalone", "1", "1", "0", "", "", "no"])
        self.assertIn("import", row[7])

    def test_links_pipes_and_empty_or_oversized_files(self):
        outside = self.root / "outside.dll"
        outside.write_bytes(helper())
        (self.selected / "Main.dll").write_bytes(primary())
        (self.selected / "OwnedWorker.dll").symlink_to(outside)
        os.link(outside, self.selected / "hard.dll")
        os.mkfifo(self.selected / "pipe.dll")
        (self.selected / "empty.dll").touch()
        large = self.selected / "large.dll"
        with large.open("wb") as stream:
            stream.truncate(64 * 1024 * 1024 + 1)
        alias = self.root / "alias"
        alias.symlink_to(self.selected, target_is_directory=True)
        self.assertEqual(self.scan(alias)[0], ["unavailable", "0", "0"])
        (self.selected / "child").mkdir()
        for indirect in (str(alias) + "/", str(alias) + "/.", alias / "child", str(self.selected) + "/../selected"):
            self.assertEqual(self.scan(indirect)[0], ["unavailable", "0", "0"])
        self.assertEqual(self.scan(self.root / "absent")[0], ["unavailable", "0", "0"])
        self.assertEqual(self.scan(outside)[0], ["unavailable", "0", "0"])
        status, files = self.scan()
        self.assertEqual(status, ["checked", "6", "1"])
        self.assertEqual(files["Main.dll"][0], "unreadable-dependency")
        self.assertEqual([files[name][0] for name in ("OwnedWorker.dll", "hard.dll", "pipe.dll", "empty.dll", "large.dll")],
                         ["not-regular", "not-regular", "not-regular", "empty", "too-large"])

    def test_supported_layouts_and_nested_symlinks(self):
        deep = self.root / "outside/SKSE/Plugins"
        deep.mkdir(parents=True)
        (deep / "Main.dll").write_bytes(supported_fixture())
        (self.selected / "Data").symlink_to(self.root / "outside", target_is_directory=True)
        self.assertEqual(self.scan()[0], ["no-plugins", "0", "0"])
        (self.selected / "Data").unlink()
        (self.selected / "Data").mkdir()
        (self.selected / "Data/SKSE").symlink_to(deep.parent, target_is_directory=True)
        self.assertEqual(self.scan()[0], ["no-plugins", "0", "0"])
        (self.selected / "bad.dll").write_bytes(b"invalid")
        self.assertEqual(self.scan()[0], ["no-plugins", "1", "0"])
        if not (self.selected / "data").exists():
            (self.selected / "data").mkdir()
            self.assertEqual(self.scan()[0], ["ambiguous-layout", "0", "0"])

    def test_parent_needs_search_permission_only(self):
        boundary = self.root / "search-only"
        picked = boundary / "Authorised"
        picked.mkdir(parents=True)
        (picked / "Main.dll").write_bytes(supported_fixture())
        boundary.chmod(0o111)
        try:
            self.assertEqual(self.scan(picked)[0], ["checked", "1", "1"])
        finally:
            boundary.chmod(0o700)

    def test_repeated_scans_release_descriptors_on_success_and_limit(self):
        import resource
        def bound_open_files():
            resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))
        for success, count in ((True, 1), (False, 17)):
            place = self.root / ("ok" if success else "too-many")
            place.mkdir()
            for i in range(count):
                (place / f"{i}.dll").write_bytes(supported_fixture())
            data = subprocess.check_output([self.binary, place, "repeat"], text=True, timeout=10,
                                           preexec_fn=bound_open_files).splitlines()
            self.assertEqual(data[0], "checked|1|1" if success else "file-limit|0|0")

    def test_limits_stop_the_whole_batch_before_any_trial(self):
        for i in range(16):
            (self.selected / f"{i:02d}.dll").write_bytes(supported_fixture())
        self.assertEqual(self.scan()[0], ["checked", "16", "16"])
        (self.selected / "17.dll").write_bytes(supported_fixture())
        self.assertEqual(self.scan()[0], ["file-limit", "0", "0"])
        noisy = self.root / "noise"
        noisy.mkdir()
        for i in range(1025):
            (noisy / str(i)).touch()
        self.assertEqual(self.scan(noisy)[0], ["entry-limit", "0", "0"])
        large = self.root / "large"
        large.mkdir()
        for name in ("a.dll", "b.dll"):
            with (large / name).open("wb") as f:
                f.truncate(33 * 1024 * 1024)
        self.assertEqual(self.scan(large)[0], ["size-limit", "0", "0"])

    def test_real_independently_linked_x86_plugin_and_helper(self):
        compiler = shutil.which("clang") or shutil.which("clang-18")
        linker = shutil.which("lld-link") or shutil.which("lld-link-18")
        if not linker and sys.platform == "linux" and shutil.which("wslpath"):
            windows_llvm = Path("/mnt/c/Program Files/LLVM/bin/lld-link.exe")
            if windows_llvm.is_file():
                linker = str(windows_llvm)
        if not compiler or not linker:
            self.skipTest("LLVM Clang and COFF linker required for the real two-DLL fixture")
        build = self.root / "compiler"
        build.mkdir()
        common = [compiler, "--target=i686-pc-windows-msvc", "-ffreestanding", "-fno-stack-protector", "-O2"]
        def run(args):
            subprocess.run(args, cwd=build, check=True, capture_output=True, timeout=30)
        run([*common, "-c", HERE / "test_dependency_dll.c", "-o", "helper.obj"])
        run([*common, "-DFIXTURE_VARIANT=9", "-c", HERE / "test_fixture_dll.c", "-o", "plugin.obj"])
        run([linker, "/dll", "/machine:x86", "/nodefaultlib", "/base:0x10000000", "/entry:OwnedDependencyEntry@12",
             "/out:OwnedWorker.dll", "/implib:helper.lib", "helper.obj"])
        run([linker, "/dll", "/machine:x86", "/nodefaultlib", "/base:0x10000000", "/noentry", "/out:OwnedClassic.dll",
             "/implib:plugin.lib", "plugin.obj", "helper.lib"])
        deep = self.selected / "Data/SKSE/Plugins"
        deep.mkdir(parents=True)
        shutil.copyfile(build / "OwnedClassic.dll", deep / "OwnedClassic.dll")
        self.assertEqual(self.scan()[1]["Data/SKSE/Plugins/OwnedClassic.dll"][0], "missing")
        shutil.copyfile(build / "OwnedWorker.dll", deep / "OwnedWorker.dll")
        header, files = self.scan()
        self.assertEqual(header, ["checked", "2", "1"])
        self.assertEqual(files["Data/SKSE/Plugins/OwnedClassic.dll"][:7],
                         ["paired", "1", "1", "1", "OwnedWorker.dll", "", "yes"])
        self.assertEqual(files["Data/SKSE/Plugins/OwnedWorker.dll"][0], "not-plugin")


if __name__ == "__main__":
    unittest.main()
