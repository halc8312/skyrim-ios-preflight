"""Portable request-lifetime model; no actual Metal callback timing is asserted."""
import contextlib
import hashlib
import importlib.util
import os
from pathlib import Path
import platform
import shutil
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_request_runtime_helpers", ROOT / "experiments/metal-compile/runtime.py")
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)
CXX = shutil.which(os.environ.get("CXX", "c++"))
EXPECTED = "METAL_REQUEST_LIFETIME barrier_cases=9 owner_hold_cases=9 launch_unwind_cases=5 concurrent_iterations_each=300 metal_api_verified=0"
EXPECTED_LAUNCH = "METAL_REQUEST_LAUNCH_UNWIND cases=5 metal_api_verified=0 objc_runtime_verified=0"
TOTAL_BUDGET_SECONDS = 180


def budgeted_command(command, label, output, report, env, deadline, cap=60):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise RuntimeError("request lifetime aggregate budget exhausted before " + label)
    # The runtime helper rechecks the same deadline immediately before Popen.
    return runtime.run_command(command, label, output, report, env, min(cap, remaining), deadline=deadline)


class MetalRequestBudgetTests(unittest.TestCase):
    def test_shared_deadline_caps_each_command_and_refuses_launch_at_zero(self):
        with patch.object(time, "monotonic", side_effect=[100, 176, 180]), \
                patch.object(runtime, "run_command", return_value={}) as child:
            budgeted_command(["synthetic"], "first", None, {}, {}, 180, 60)
            budgeted_command(["synthetic"], "second", None, {}, {}, 180, 60)
            with self.assertRaisesRegex(RuntimeError, "aggregate budget exhausted"):
                budgeted_command(["synthetic"], "third", None, {}, {}, 180, 60)
        self.assertEqual(child.call_count, 2)
        self.assertEqual([call.args[-1] for call in child.call_args_list], [60, 4])
        self.assertEqual([call.kwargs["deadline"] for call in child.call_args_list], [180, 180])

    def test_fractional_remaining_time_is_preserved_and_expired_budget_never_starts_child(self):
        with patch.object(time, "monotonic", side_effect=[179.875, 181]), \
                patch.object(runtime, "run_command", return_value={}) as child:
            budgeted_command(["synthetic"], "fractional", None, {}, {}, 180)
            with self.assertRaisesRegex(RuntimeError, "aggregate budget exhausted"):
                budgeted_command(["synthetic"], "expired", None, {}, {}, 180)
        self.assertEqual(child.call_count, 1)
        self.assertEqual(child.call_args.args[-1], 0.125)


@unittest.skipUnless(os.name == "posix" and CXX, "portable C++17 model requires a POSIX C++ compiler; not native Metal evidence")
class MetalRequestLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.deadline = time.monotonic() + TOTAL_BUDGET_SECONDS
        base = os.environ.get("SKYRIM_METAL_REQUEST_OUTPUT")
        cls.output = Path(base).resolve() if base else ROOT / "local/tests" / f"metal-request-{time.time_ns()}-{os.getpid()}"
        cls.output.mkdir(parents=True, exist_ok=False)
        cls.report = {"scope": "authored portable request-lifetime model", "status": "running", "commands": [],
                      "metal_api_verified": False, "objc_runtime_verified": False,
                      "gpu_execution_verified": False, "inputs": {}, "tests": {},
                      "budget_seconds": TOTAL_BUDGET_SECONDS, "tsan_model_verified": False,
                      "tsan_scope": "Darwin-only safe authored models; not Metal callback or driver race validation"}
        cls.env = os.environ.copy()
        for name in ("SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "IPHONEOS_DEPLOYMENT_TARGET"):
            cls.env.pop(name, None)
        cls.flags = []
        for source in (ROOT / "experiments/metal-compile/request_lifetime.h",
                       ROOT / "experiments/metal-compile/owner_retirement.h",
                       ROOT / "tests/native/test_metal_request_lifetime.cpp",
                       Path(__file__).resolve(), ROOT / "experiments/metal-compile/runtime.py"):
            target = cls.output / source.name
            target.write_bytes(source.read_bytes())
            cls.report["inputs"][source.name] = hashlib.sha256(target.read_bytes()).hexdigest()
        version = cls.run_logged([CXX, "--version"], "compiler-version", 30)
        if version["returncode"] != 0:
            cls.report["status"] = "failed"
            runtime.write_report(cls.output, cls.report)
            raise RuntimeError("compiler version probe failed; retained model logs: " + str(cls.output))
        cls.report["compiler_version"] = (cls.output / version["stdout"]).read_text(encoding="utf-8", errors="replace")
        if platform.system() == "Darwin":
            event = cls.run_logged(["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"], "sdk-path", 30)
            if event["returncode"] != 0:
                cls.report["status"] = "failed"
                runtime.write_report(cls.output, cls.report)
                raise RuntimeError("macOS SDK discovery failed; retained model logs: " + str(cls.output))
            sdk = (cls.output / event["stdout"]).read_text(encoding="utf-8").strip()
            cls.flags = ["-isysroot", sdk, "-mmacosx-version-min=14.0"]
        runtime.write_report(cls.output, cls.report)

    @classmethod
    def run_logged(cls, command, label, timeout=60, env=None):
        try:
            return budgeted_command(command, label, cls.output, cls.report, env or cls.env, cls.deadline, timeout)
        except BaseException as error:
            cls.report["status"] = "interrupted" if isinstance(error, (KeyboardInterrupt, SystemExit)) else "failed"
            cls.report["failure"] = type(error).__name__ + ": " + str(error)
            raise
        finally:
            runtime.write_report(cls.output, cls.report)

    def build(self, suffix, sanitizer=None):
        binary = self.output / ("request-model-" + suffix)
        command = [CXX, "-std=c++17", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror", *self.flags,
                   "-I", str(self.output), str(self.output / "test_metal_request_lifetime.cpp"), "-o", str(binary)]
        if sanitizer:
            command.extend(("-fsanitize=" + sanitizer, "-fno-omit-frame-pointer"))
        event = self.run_logged(command, "compile-" + suffix)
        self.assertEqual(event["returncode"], 0, (self.output / event["stderr"]).read_text(errors="replace"))
        return binary

    def verify_positive(self, binary, suffix, env=None, arguments=(), expected=EXPECTED):
        event = self.run_logged([str(binary), *arguments], "run-" + suffix, env=env)
        self.assertEqual(event["returncode"], 0, (self.output / event["stderr"]).read_text(errors="replace"))
        self.assertEqual((self.output / event["stderr"]).read_text(errors="replace"), "", "safe model emitted a runtime diagnostic")
        self.assertEqual((self.output / event["stdout"]).read_text().strip(), expected)

    @contextlib.contextmanager
    def case(self, name):
        observation = {"status": "running"}
        self.report["tests"][name] = observation
        try:
            yield
            observation["status"] = "passed"
        except BaseException as error:
            observation["status"] = "interrupted" if isinstance(error, (KeyboardInterrupt, SystemExit)) else "failed"
            observation["failure"] = type(error).__name__ + ": " + str(error)
            raise
        finally:
            states = [test["status"] for test in self.report["tests"].values()]
            self.report["status"] = ("interrupted" if "interrupted" in states else "failed" if "failed" in states
                                     else "passed" if all(s == "passed" for s in states) else "running")
            runtime.write_report(self.output, self.report)

    def test_normal_state_transitions(self):
        with self.case("normal"):
            self.verify_positive(self.build("normal"), "normal")
            self.report["normal_model_verified"] = True

    def test_launch_unwind_host_protocol_normal_and_sanitized(self):
        with self.case("launch_unwind_host_protocol"):
            arguments = ("--launch-unwind-only",)
            self.verify_positive(self.build("launch-normal"), "launch-normal",
                                 arguments=arguments, expected=EXPECTED_LAUNCH)
            env = self.env.copy()
            env["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=0:detect_leaks=" + (
                "0" if platform.system() == "Darwin" else "1")
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            self.verify_positive(self.build("launch-asan", "address,undefined"), "launch-asan", env,
                                 arguments=arguments, expected=EXPECTED_LAUNCH)
            self.report["launch_unwind_host_protocol_verified"] = True
            self.report["launch_unwind_cases"] = 5
            self.report["launch_unwind_scope"] = (
                "Authored C++ provider null/throw before or after notification and concurrent notification; "
                "not Objective-C unwinding, Metal task release, cancellation, or production integration.")

    def test_asan_model_and_explicit_unsafe_negative_control(self):
        with self.case("asan_ubsan_and_authored_negative"):
            binary = self.build("asan", "address,undefined")
            env = self.env.copy()
            env["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=0:detect_leaks=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            # Apple ASan does not support leak detection; this is still an ownership
            # counter + address/UB check, without claiming a platform leak scanner.
            if platform.system() == "Darwin":
                env["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=0:detect_leaks=0"
            self.verify_positive(binary, "asan-safe", env)
            event = self.run_logged([str(binary), "--unsafe-model"], "asan-intentional-negative", env=env)
            self.assertNotEqual(event["returncode"], 0, "unsafe authored model unexpectedly exited successfully")
            failure = (self.output / event["stderr"]).read_text(errors="replace")
            self.assertIn("EXPECTED NEGATIVE", failure)
            self.assertIn("AddressSanitizer: heap-use-after-free", failure)
            self.report["sanitized_model_verified"] = True
            self.report["authored_unsafe_negative_detected"] = True

    @unittest.skipUnless(platform.system() == "Darwin", "ThreadSanitizer run requires Darwin; no race pass on other hosts")
    def test_tsan_safe_models_on_darwin(self):
        with self.case("tsan_safe_models"):
            binary = self.build("tsan", "thread")
            env = self.env.copy()
            env["TSAN_OPTIONS"] = "halt_on_error=1"
            # Both models and all 300 concurrent iterations each use the same
            # exact output/stderr checks. The authored unsafe control is ASan-only.
            self.verify_positive(binary, "tsan-safe", env)
            self.report["tsan_model_verified"] = True


if __name__ == "__main__":
    unittest.main()
