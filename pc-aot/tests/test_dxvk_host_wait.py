"""Isolated host-trial contracts; stubs never establish Apple/iPhone behavior."""
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch as mock_patch

ROOT = Path(__file__).resolve().parents[1]
EXPERIMENT = ROOT / "experiments/dxvk-host-wait"
spec = importlib.util.spec_from_file_location("isolated_dxvk_host_wait", EXPERIMENT / "patch.py")
trial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trial)
SOURCE = trial.SOURCE


def minimum_stage(path):
    for name in (trial.LOCK, trial.CS, *trial.GUARDS):
        target = path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(trial.baseline(name), encoding="utf-8")


def snapshot(path):
    return {file.relative_to(path).as_posix(): file.read_bytes() for file in path.rglob("*") if file.is_file()}


class HostWaitStageTests(unittest.TestCase):
    def test_shared_spin_and_cs_execution_are_unchanged(self):
        old = trial.baseline(trial.LOCK)
        new = trial.candidate("unfair-lock")
        begin = old.index("  template<typename Fn>")
        end = old.index("  class Spinlock {")
        self.assertIn(old[begin:end], new)
        old_class = old[old.index("  class Spinlock {"):old.index("\n  };", old.index("  class Spinlock {")) + 5]
        self.assertIn("#else\n" + old_class + "\n#endif", new)
        body = (EXPERIMENT / "apple_spinlock.in").read_text(encoding="utf-8")
        lock = body[body.index("    void lock()"):body.index("    bool try_lock()")]
        self.assertEqual(lock.count("os_unfair_lock_trylock("), 1)
        self.assertEqual(lock.count("os_unfair_lock_lock("), 1)
        self.assertLess(lock.index("if (!os_unfair_lock_trylock"), lock.index("LockWaitSample"))
        self.assertNotIn("while", lock)
        self.assertNotIn("for (", lock)
        observer = (EXPERIMENT / "host_lock_wait.h").read_text(encoding="utf-8")
        self.assertNotIn("fprintf", observer)
        self.assertNotIn("ofstream", observer)
        old = trial.baseline(trial.CS)
        new = trial.candidate("cs-qos")
        restored = new.removeprefix('#include "host_cs_qos.h"\n').replace("\n    skyrim::host_wait::csQosStartup();", "")
        self.assertEqual(restored, old)
        self.assertEqual(new.count('env::setThreadName("dxvk-cs");\n    skyrim::host_wait::csQosStartup();'), 1)

    def test_modes_change_only_their_own_stage_files_and_are_exclusive(self):
        for mode, target, helper in (("unfair-lock", trial.LOCK, trial.WAIT), ("cs-qos", trial.CS, trial.QOS)):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="host-stage-") as temporary:
                stage = Path(temporary)
                minimum_stage(stage)
                before = snapshot(stage)
                trial.apply(stage, mode)
                after = snapshot(stage)
                self.assertEqual({key for key in before.keys() | after.keys() if before.get(key) != after.get(key)}, {target, helper})
                trial.apply(stage, mode)
                self.assertEqual(snapshot(stage), after)
                other = "cs-qos" if mode == "unfair-lock" else "unfair-lock"
                with self.assertRaisesRegex(ValueError, "exclusive"):
                    trial.apply(stage, other)
                self.assertEqual(snapshot(stage), after)

    def test_drift_and_helper_conflicts_fail_before_any_write(self):
        for mode, changed in (("unfair-lock", trial.LOCK), ("cs-qos", trial.CS),
                              ("unfair-lock", trial.GUARDS[0]), ("cs-qos", trial.WAIT)):
            with self.subTest(mode=mode, changed=changed), tempfile.TemporaryDirectory(prefix="host-drift-") as temporary:
                stage = Path(temporary)
                minimum_stage(stage)
                path = stage / changed
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text((path.read_text(encoding="utf-8") if path.exists() else "") + "\n// altered\n", encoding="utf-8")
                before = snapshot(stage)
                with self.assertRaises(ValueError):
                    trial.apply(stage, mode)
                self.assertEqual(snapshot(stage), before)
        with self.assertRaisesRegex(ValueError, "anchor"):
            trial.once("x x", "x", "y")

    def test_vendor_production_destinations_and_unknown_mode_are_refused(self):
        for destination in (ROOT, ROOT / "vendor", SOURCE, ROOT / "ios", EXPERIMENT):
            with self.subTest(destination=destination), self.assertRaises(ValueError):
                trial.apply(destination, "unfair-lock")
        with self.assertRaises(ValueError):
            trial.apply(ROOT, "both")
        production = (ROOT / "ios/dependencies/prepare_dxvk.py").read_text(encoding="utf-8")
        for marker in ("host_cs_qos", "host_lock_wait", "dxvk-host-wait"):
            self.assertNotIn(marker, production)

    @unittest.skipUnless(os.name == "posix", "Creating links requires POSIX test permissions")
    def test_linked_stage_target_is_refused(self):
        with tempfile.TemporaryDirectory(prefix="host-link-") as temporary:
            folder = Path(temporary); stage = folder / "stage"; minimum_stage(stage)
            path = stage / trial.LOCK; outside = folder / "outside.h"
            outside.write_bytes(path.read_bytes()); path.unlink(); path.symlink_to(outside)
            before = outside.read_bytes()
            with self.assertRaisesRegex(ValueError, "Linked"):
                trial.apply(stage, "unfair-lock")
            self.assertEqual(outside.read_bytes(), before)

    @unittest.skipUnless(os.name == "posix", "Creating links requires POSIX test permissions")
    def test_private_directory_link_cannot_allow_a_vendor_destination(self):
        with tempfile.TemporaryDirectory(prefix="host-private-link-") as temporary:
            fake_root = Path(temporary).resolve() / "checkout"
            vendor = fake_root / "vendor"
            minimum_stage(vendor)
            (fake_root / "local").symlink_to(vendor, target_is_directory=True)
            before = snapshot(vendor)
            with mock_patch.object(trial, "ROOT", fake_root), self.assertRaisesRegex(ValueError, "never production/vendor"):
                trial.apply(fake_root / "local", "unfair-lock")
            self.assertEqual(snapshot(vendor), before)


def sanitizer_flags():
    mode = os.environ.get("SKYRIM_TEST_SANITIZERS")
    if mode == "thread": return ["-fsanitize=thread", "-fno-omit-frame-pointer"]
    if mode == "1": return ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    return []


def includes(source=SOURCE):
    result = ["-I", str(EXPERIMENT), "-I", str(ROOT / "tests/native"),
              "-I", str(ROOT / "ios/runtime-port"), "-I", str(ROOT / "ios/dependencies")]
    for name in ("src", "src/util/sync", "include", "include/native", "include/native/windows", "include/native/directx"):
        result += ["-I", str(source / name)]
    return result


def native_environment():
    result = dict(os.environ)
    if platform.system() == "Darwin":
        result.pop("IPHONEOS_DEPLOYMENT_TARGET", None)
        result.pop("SDKROOT", None)
    return result


def macos_compile_flags():
    if platform.system() != "Darwin":
        return []
    result = subprocess.run(["xcrun", "--sdk", "macosx", "--show-sdk-path"],
                            env=native_environment(), check=True, capture_output=True, text=True, timeout=20)
    sdk = result.stdout.strip()
    if not sdk:
        raise RuntimeError("xcrun returned an empty macOS SDK path")
    return ["-isysroot", sdk, "-mmacosx-version-min=14.0"]


def compile_command():
    return [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-g", "-pthread", *sanitizer_flags(),
            "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-Wno-unused-parameter",
            "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00",
            *macos_compile_flags()]


def syntax_command(stage):
    return [os.environ.get("CXX", "c++"), "-std=c++17", "-fsyntax-only", "-include", "cstdint", "-include", "cstring",
            "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00", "-Wno-deprecated-declarations",
            *macos_compile_flags(), *includes(stage), str(stage / trial.CS)]


class HostWaitCompilerCommandTests(unittest.TestCase):
    def test_both_mac_commands_use_explicit_host_sdk_and_clean_environment(self):
        sdk = "/Applications/Xcode Test.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
        resolved = subprocess.CompletedProcess([], 0, stdout=sdk + "\n", stderr="")
        with mock_patch.object(platform, "system", return_value="Darwin"), \
                mock_patch.dict(os.environ, {"SDKROOT": "/wrong/iPhoneOS.sdk", "IPHONEOS_DEPLOYMENT_TARGET": "18.0"}), \
                mock_patch.object(subprocess, "run", return_value=resolved) as resolve:
            commands = (compile_command(), syntax_command(Path("stage")))
            for command in commands:
                self.assertEqual(command.count("-isysroot"), 1)
                self.assertEqual(command[command.index("-isysroot") + 1], sdk)
                self.assertIn("-mmacosx-version-min=14.0", command)
            self.assertEqual(resolve.call_count, 2)
            for call in resolve.call_args_list:
                self.assertEqual(call.args[0], ["xcrun", "--sdk", "macosx", "--show-sdk-path"])
                self.assertNotIn("SDKROOT", call.kwargs["env"])
                self.assertNotIn("IPHONEOS_DEPLOYMENT_TARGET", call.kwargs["env"])
                self.assertTrue(call.kwargs["check"])

    def test_non_mac_commands_do_not_query_an_apple_sdk(self):
        with mock_patch.object(platform, "system", return_value="Linux"), mock_patch.object(subprocess, "run") as resolve:
            for command in (compile_command(), syntax_command(Path("stage"))):
                self.assertNotIn("-isysroot", command)
                self.assertNotIn("-mmacosx-version-min=14.0", command)
            resolve.assert_not_called()

    def test_empty_sdk_path_is_an_explicit_failure(self):
        with mock_patch.object(platform, "system", return_value="Darwin"), \
                mock_patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stdout="\n", stderr="")), \
                self.assertRaisesRegex(RuntimeError, "empty macOS SDK"):
            compile_command()


def run_command(binary):
    result = [str(binary)]
    if os.environ.get("SKYRIM_TEST_SANITIZERS") == "thread" and os.environ.get("SKYRIM_TSAN_DISABLE_ASLR") == "1":
        setarch = shutil.which("setarch")
        if not setarch: raise RuntimeError("setarch required for requested ThreadSanitizer ASLR control")
        result = [setarch, platform.machine(), "-R", *result]
    return result


def apple_stubs(folder):
    for name in ("os/lock.h", "pthread/qos.h", "sys/qos.h"):
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "host_wait_apple_stubs.h"\n', encoding="utf-8")


@unittest.skipUnless(os.name == "posix" and shutil.which(os.environ.get("CXX", "c++")), "Native POSIX compiler required")
class HostWaitNativeTests(unittest.TestCase):
    def execute(self, command, timeout=90):
        result = subprocess.run(command, env=native_environment(), capture_output=True, text=True, timeout=timeout)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_actual_locks_publication_exception_fast_path_and_contended_samples(self):
        for enabled in (False, True):
            stub = enabled and platform.system() != "Darwin"
            with self.subTest(trial=enabled, stub=stub), tempfile.TemporaryDirectory(prefix="host-lock-") as temporary:
                folder = Path(temporary)
                header = folder / "util/sync/sync_spinlock.h"
                header.parent.mkdir(parents=True)
                header.write_text(trial.candidate("unfair-lock") if enabled else trial.baseline(trial.LOCK), encoding="utf-8")
                if stub: apple_stubs(folder)
                binary = folder / "lock"
                self.execute([*compile_command(), f"-DHOST_TRIAL={int(enabled)}", f"-DHOST_APPLE_STUB={int(stub)}",
                    "-I", str(folder), *includes(), str(ROOT / "tests/native/dxvk_host_wait_lock.cpp"), "-o", str(binary)])
                result = self.execute(run_command(binary), timeout=30)
                report = json.loads(result.stdout)
                self.assertEqual(report["trial"], int(enabled))
                self.assertEqual(report["authored_api_stub"], int(stub))
                self.assertEqual(report["apple_backend_verified"], int(enabled and not stub))
                self.assertEqual((report["semantics"], report["waits"], report["samples"], report["hot_clock_reads"]), (1, 3, 3, 0))
                self.assertEqual((report["iphone_verified"], report["performance_verified"]), (0, 0))
                self.assertGreater(report["wall_ns"], 0)
                self.assertGreaterEqual(report["cpu_ns"], 0)
                print("HOST_LOCK", result.stdout.strip(), flush=True)

    def test_cs_qos_exact_optin_errno_failure_reporting_and_concurrent_ordinals(self):
        stub = platform.system() != "Darwin"
        with tempfile.TemporaryDirectory(prefix="host-qos-") as temporary:
            folder = Path(temporary)
            if stub: apple_stubs(folder)
            code = (ROOT / "tests/native/dxvk_host_wait_qos.cpp").read_text(encoding="utf-8")
            cs = trial.candidate("cs-qos")
            begin = cs.index('    env::setThreadName("dxvk-cs");')
            end = cs.index("\n\n    DxvkCsChunkRef chunk;", begin)
            code = code.replace("// ACTUAL_CS_STARTUP", cs[begin:end])
            source = folder / "qos.cpp"; source.write_text(code, encoding="utf-8")
            binary = folder / "qos"
            self.execute([*compile_command(), f"-DHOST_APPLE_STUB={int(stub)}", "-I", str(folder),
                          *includes(), str(source), "-o", str(binary)])
            for mode in ("disabled", "enabled"):
                with self.subTest(mode=mode):
                    result = self.execute([*run_command(binary), mode], timeout=30)
                    report = json.loads(result.stdout)
                    self.assertEqual(report["semantics"], 1)
                    self.assertEqual(report["authored_api_stub"], int(stub))
                    self.assertEqual(report["apple_api_verified"], int(not stub and mode == "enabled"))
                    self.assertEqual((report["iphone_verified"], report["effective_qos_verified"], report["performance_verified"]), (0, 0, 0))
                    lines = result.stderr.splitlines()
                    if mode == "disabled":
                        self.assertEqual(lines, [], "Disabled startup must make no report")
                    else:
                        self.assertEqual(len(lines), 64, result.stderr)
                        records = []
                        for line in lines:
                            self.assertTrue(line.startswith("DXVK_HOST_CS_QOS "))
                            record = dict(item.split("=", 1) for item in line.split()[1:])
                            self.assertTrue(all(value.lstrip("-").isdigit() for value in record.values()))
                            self.assertEqual((record["enabled"], record["set_attempted"], record["set_result"]), ("1", "1", "0"))
                            self.assertEqual((record["effective_observed"], record["iphone_verified"]), ("0", "0"))
                            records.append(int(record["thread"]))
                        self.assertEqual(sorted(records), list(range(1, 65)))
                    print("HOST_QOS", result.stdout.strip(), flush=True)

    def test_complete_staged_cs_translation_unit(self):
        with tempfile.TemporaryDirectory(prefix="host-cs-syntax-") as temporary:
            stage = Path(temporary) / "dxvk"
            trial.adapter.stage(SOURCE, stage)
            trial.apply(stage, "cs-qos")
            self.execute(syntax_command(stage), timeout=90)


if __name__ == "__main__":
    unittest.main()
