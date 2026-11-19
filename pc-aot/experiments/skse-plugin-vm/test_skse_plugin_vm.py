"""Compare imported, authored i386 PE code with Windows's native loader."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
LLVM = Path(os.environ.get("SKYRIM_LLVM_BIN", "C:/Program Files/LLVM/bin"))


def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, timeout=60, **kwargs)


def compile_interpreter(compiler, output, test_source, flag_list):
    run(compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", *flag_list,
        HERE / "pe_image.c", HERE / "x86_call.c", HERE / "x86_heap.c", HERE / "x86_winthread.c", HERE / "x86_winfile.c",
        HERE / "x86_winsync.c", HERE / "x86_skse_records.c", HERE / "dependency_plan.c", HERE / "plugin_trial.c",
        HERE / test_source, "-o", output)


def compare_with_windows(compiler, linker, emulator, directory, variant, opt,
                         *, imports=(), attach=False, mode=None, with_dependency=False):
    directory.mkdir()
    obj, dll, lib = (directory / f"fixture.{ext}" for ext in ("obj", "dll", "lib"))
    run(compiler, "--target=i686-pc-windows-msvc", "-ffreestanding", "-fno-stack-protector",
        opt, f"-DFIXTURE_VARIANT={variant}", "-c", HERE / "test_fixture_dll.c", "-o", obj)
    companion = directory / "OwnedWorker.dll"
    if with_dependency:
        dependency_obj, dependency_lib = directory / "worker.obj", directory / "worker.lib"
        helper_flags = ("-DHELPER_STATIC_TLS",) if variant == 16 else ()
        run(compiler, "--target=i686-pc-windows-msvc", "-ffreestanding", "-fno-stack-protector", opt,
            *helper_flags, "-c", HERE / "test_dependency_dll.c", "-o", dependency_obj)
        run(linker, "/dll", "/nodefaultlib", "/machine:x86", "/base:0x10000000",
            "/entry:OwnedDependencyEntry@12", f"/out:{companion}", f"/implib:{dependency_lib}", dependency_obj,
            *(imports if variant == 16 else ()))
        imports = (*imports, dependency_lib)
    entry = "/entry:TestModuleEntry@12" if attach else "/noentry"
    run(linker, "/dll", "/nodefaultlib", "/machine:x86", "/base:0x10000000",
        f"/out:{dll}", f"/implib:{lib}", obj, entry, *imports)
    reference = directory / "reference.exe"
    run(compiler, "--target=i686-pc-windows-msvc", "-fuse-ld=lld", "-std=c11",
        HERE / "test_native_windows.c", HERE / "x86_skse_records.c", "-o", reference)
    if mode == "file-assets":
        asset_root = directory / "Data/SKSE/Plugins"
        asset_root.mkdir(parents=True)
        (asset_root / "Settings.ini").write_bytes(b"ABCDEFG")
        (asset_root / "設定.ini").write_bytes(b"jp\r\n")
    try:
        expected = run(reference, *([mode] if mode else []), cwd=directory, capture_output=True, text=True).stdout
    except subprocess.CalledProcessError as error:
        raise AssertionError(error.stderr or f"Windows reference exited with {error.returncode}") from error
    command = [emulator, dll]
    if mode: command.append(mode)
    if with_dependency: command.append(companion)
    try:
        actual = run(*command, capture_output=True, text=True).stdout
    except subprocess.CalledProcessError as error:
        raise AssertionError(error.stderr or error.stdout or f"Interpreter exited with {error.returncode}") from error
    if mode and (not with_dependency or variant == 16):
        moved = run(*command, "move", capture_output=True, text=True).stdout
        if moved != actual:
            raise AssertionError(f"Moving the DLL changed the interpreted output:\n{moved}\n{actual}")
    return expected, actual


class PluginInterpreterTests(unittest.TestCase):
    def test_memory_permissions_abi_and_budget(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if sys.platform == "win32" and not compiler and (LLVM / "clang.exe").is_file():
            compiler = LLVM / "clang.exe"
        if not compiler:
            self.skipTest("C compiler required")
        flags = []
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        if sys.platform == "win32":
            flags += ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"]
        with tempfile.TemporaryDirectory(prefix="plugin-vm-faults-") as tmp:
            test = Path(tmp) / ("faults.exe" if sys.platform == "win32" else "faults")
            compile_interpreter(compiler, test, "test_x86_faults.c", flags)
            run(test)

    @unittest.skipUnless(sys.platform == "win32", "Windows runs the native i386 reference")
    def test_independently_linked_plugins_match_two_native_windows_builds(self):
        cc, linker = LLVM / "clang.exe", LLVM / "lld-link.exe"
        if not cc.is_file() or not linker.is_file():
            self.skipTest("Windows LLVM clang and lld-link required")
        with tempfile.TemporaryDirectory(prefix="skse-import-") as tmp:
            root = Path(tmp)
            emulator = root / "trial.exe"
            compile_interpreter(cc, emulator, "test_interpreted.c",
                                ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
            for variant, opt in ((1, "-O0"), (1, "-O2"), (2, "-O0"), (2, "-O2"),
                                 (5, "-O0"), (5, "-O2"), (12, "-O0"), (12, "-O2"),
                                 (13, "-O0"), (13, "-O2")):
                with self.subTest(variant=variant, optimization=opt):
                    expected, actual = compare_with_windows(cc, linker, emulator, root / f"{variant}-{opt}",
                                                            variant, opt)
                    self.assertEqual(actual, expected)
                    self.assertEqual(len(expected.strip().splitlines()), 8)
                    self.assertIn(",1,1,", expected)

    @unittest.skipUnless(sys.platform == "win32", "Windows SDK required for the imported clock reference")
    def test_stdcall_dll_entry_and_tick_count_import(self):
        cc, linker = LLVM / "clang.exe", LLVM / "lld-link.exe"
        kits = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Windows Kits/10/Lib"
        kernel32 = sorted(kits.glob("*/um/x86/kernel32.lib"))
        if not cc.is_file() or not linker.is_file() or not kernel32:
            self.skipTest("Windows LLVM and x86 Windows SDK required")
        with tempfile.TemporaryDirectory(prefix="skse-import-clock-") as tmp:
            root = Path(tmp)
            emulator = root / "trial.exe"
            compile_interpreter(cc, emulator, "test_interpreted.c",
                                ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
            for opt in ("-O0", "-O2"):
                with self.subTest(optimization=opt):
                    expected, actual = compare_with_windows(cc, linker, emulator, root / opt,
                                                            3, opt, imports=(kernel32[-1],),
                                                            attach=True, mode="clock")
                    self.assertEqual(actual, expected)

    @unittest.skipUnless(sys.platform == "win32", "Windows runs the native CRT reference")
    def test_bulk_crt_imports_preserve_data_and_match_windows(self):
        cc, linker, dlltool = (LLVM / (name + ".exe") for name in ("clang", "lld-link", "llvm-dlltool"))
        if not all(path.is_file() for path in (cc, linker, dlltool)):
            self.skipTest("LLVM toolchain with COFF import tooling required")
        with tempfile.TemporaryDirectory(prefix="skse-import-crt-") as tmp:
            root = Path(tmp)
            emulator, module, definition = root / "trial.exe", root / "msvcrt.lib", root / "msvcrt.def"
            compile_interpreter(cc, emulator, "test_interpreted.c",
                                ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
            definition.write_text("LIBRARY msvcrt.dll\nEXPORTS\nmemcpy\nmemmove\nmemset\n", encoding="ascii")
            run(dlltool, "-m", "i386", "-d", definition, "-l", module)
            for opt in ("-O0", "-O2"):
                with self.subTest(optimization=opt):
                    expected, actual = compare_with_windows(cc, linker, emulator, root / opt,
                                                            4, opt, imports=(module,), mode="memory")
                    self.assertEqual(actual, expected)
                    self.assertIn(",1,1,", actual)

    @unittest.skipUnless(sys.platform == "win32", "Windows runs the native Classic callback reference")
    def test_core_messaging_matches_windows_and_reports_missing_interfaces(self):
        cc, linker = LLVM / "clang.exe", LLVM / "lld-link.exe"
        if not cc.is_file() or not linker.is_file():
            self.skipTest("Windows LLVM clang and lld-link required")
        with tempfile.TemporaryDirectory(prefix="skse-import-messages-") as tmp:
            root = Path(tmp)
            emulator = root / "trial.exe"
            compile_interpreter(cc, emulator, "test_interpreted.c",
                                ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
            for variant, witness in ((6, "messages,2,0,0"), (7, "messages,2,0,2"), (8, "messages,0,1,0")):
                for opt in ("-O0", "-O2"):
                    with self.subTest(variant=variant, optimization=opt):
                        expected, actual = compare_with_windows(cc, linker, emulator, root / f"{variant}-{opt}",
                                                                variant, opt, mode="messages")
                        self.assertEqual(actual, expected)
                        self.assertIn(witness, actual)

    @unittest.skipUnless(sys.platform == "win32", "Windows runs the native dependency reference")
    def test_named_dependency_is_bound_and_relocated_without_host_code_loading(self):
        cc, linker = LLVM / "clang.exe", LLVM / "lld-link.exe"
        if not cc.is_file() or not linker.is_file():
            self.skipTest("Windows LLVM clang and lld-link required")
        with tempfile.TemporaryDirectory(prefix="skse-import-dependency-") as tmp:
            root = Path(tmp)
            emulator = root / "trial.exe"
            compile_interpreter(cc, emulator, "test_interpreted.c",
                                ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
            for opt in ("-O0", "-O2"):
                with self.subTest(optimization=opt):
                    expected, actual = compare_with_windows(cc, linker, emulator, root / opt, 9, opt,
                                                            mode="dependency", with_dependency=True)
                    self.assertIn(",1,1,", expected)
                    self.assertEqual(actual, expected)

    @unittest.skipUnless(sys.platform == "win32", "Windows runs the Classic callback calling-convention reference")
    def test_serialization_callbacks_write_revert_and_read_without_the_game(self):
        cc, linker = LLVM / "clang.exe", LLVM / "lld-link.exe"
        if not cc.is_file() or not linker.is_file():
            self.skipTest("Windows LLVM clang and lld-link required")
        with tempfile.TemporaryDirectory(prefix="skse-serialization-") as tmp:
            root = Path(tmp)
            emulator = root / "trial.exe"
            compile_interpreter(cc, emulator, "test_interpreted.c", ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
            for opt in ("-O0", "-O2"):
                with self.subTest(optimization=opt):
                    expected, actual = compare_with_windows(cc, linker, emulator, root / opt, 22, opt,
                                                            mode="serialization")
                    self.assertIn("serialization,3,2", expected)
                    self.assertEqual(actual, expected)

    @unittest.skipUnless(sys.platform == "win32", "Windows runs the heap service reference")
    def test_heap_allocations_match_windows_for_cdecl_and_stdcall(self):
        cc, linker, dlltool = (LLVM / (name + ".exe") for name in ("clang", "lld-link", "llvm-dlltool"))
        kits = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Windows Kits/10/Lib"
        kernel32 = sorted(kits.glob("*/um/x86/kernel32.lib"))
        if not all(path.is_file() for path in (cc, linker, dlltool)) or not kernel32:
            self.skipTest("LLVM and the x86 Windows SDK required")
        with tempfile.TemporaryDirectory(prefix="skse-import-heap-") as tmp:
            root = Path(tmp)
            emulator, module, definition = root / "trial.exe", root / "msvcrt.lib", root / "msvcrt.def"
            compile_interpreter(cc, emulator, "test_interpreted.c",
                                ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
            definition.write_text("LIBRARY msvcrt.dll\nEXPORTS\nmalloc\ncalloc\nrealloc\nfree\n", encoding="ascii")
            run(dlltool, "-m", "i386", "-d", definition, "-l", module)
            for variant, library in ((10, module), (11, kernel32[-1])):
                for opt in ("-O0", "-O2"):
                    with self.subTest(variant=variant, optimization=opt):
                        expected, actual = compare_with_windows(cc, linker, emulator, root / f"{variant}-{opt}",
                                                                variant, opt, mode="allocation", imports=(library,))
                        self.assertIn(",1,1,", expected)
                        self.assertEqual(actual, expected)

    @unittest.skipUnless(sys.platform == "win32", "Windows runs the TLS service reference")
    def test_win32_tls_synchronization_and_named_services_match_native_calls(self):
        cc, linker = LLVM / "clang.exe", LLVM / "lld-link.exe"
        kits = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Windows Kits/10/Lib"
        kernel32 = sorted(kits.glob("*/um/x86/kernel32.lib"))
        if not cc.is_file() or not linker.is_file() or not kernel32:
            self.skipTest("Windows LLVM and x86 Windows SDK required")
        with tempfile.TemporaryDirectory(prefix="skse-import-tls-") as tmp:
            root = Path(tmp)
            emulator = root / "trial.exe"
            compile_interpreter(cc, emulator, "test_interpreted.c",
                                ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
            for variant, mode in ((14, "tls"), (15, "static-tls"), (16, "static-tls-pair"), (17, "interlocked"),
                                  (18, "module-lookup"), (19, "locked-instructions"), (20, "file-assets"), (21, "events")):
                for opt in ("-O0", "-O2"):
                    with self.subTest(variant=variant, optimization=opt):
                        expected, actual = compare_with_windows(cc, linker, emulator, root / f"{variant}-{opt}",
                                                                variant, opt, mode=mode,
                                                                imports=(kernel32[-1],), attach=variant in (15, 16),
                                                                with_dependency=variant == 16)
                        self.assertIn(",1,1,", expected)
                        self.assertEqual(actual, expected)

if __name__ == "__main__":
    unittest.main()
