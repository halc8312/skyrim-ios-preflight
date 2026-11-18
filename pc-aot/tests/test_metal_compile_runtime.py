"""Portable control/validation tests only: no Apple compilation or GPU evidence."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_compile_runtime", ROOT / "experiments/metal-compile/runtime.py")
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


def synthetic_report(repeats=2, offline=True, async_library=False, legacy_only=False):
    """Authored JSON for parser tests; deliberately not returned by a Metal process."""
    async_apis = (["legacy_async_source"] if legacy_only else
                  ["legacy_async_source", "metal4_async_source"]) if async_library else []
    result = {
        "schema": 1, "status": "passed", "scope": runtime.SCOPE,
        "comparison_mode": "legacy-only" if legacy_only else "metal4-comparison",
        "legacy_only_requested": legacy_only,
        "metal4_library_verified": not legacy_only, "metal4_compiler_created": not legacy_only,
        "library_factory_probe_requested": False,
        "source_options": copy.deepcopy(runtime.SOURCE_OPTIONS),
        "gpu_execution_verified": True, "repeat": repeats, "offline_library_requested": offline,
        "os_version": [26, 0, 0],
        "device": {"available": True, "name": "SYNTHETIC NON-GPU", "supports_metal4": not legacy_only},
        "compiler_setup_ns": 0 if legacy_only else 100, "samples": [], "rejections": [],
        "async_library_requested": async_library, "async_adapter_verified": async_library,
        "metal4_async_adapter_verified": async_library and not legacy_only,
        "async_apis": async_apis,
        "async_retention_checks": [],
        "async_timing_scope": ("submission through callback publication; controlled retention checks excluded" if legacy_only
                               else "submission through callback publication and task join; controlled retention checks excluded"),
        "specialization_scope": "vertex lookup plus fragment and compute specialization",
        **{key: False for key in runtime.FALSE_EVIDENCE},
    }
    # Explicit bit patterns are independent of the runner's float-to-bits calculation.
    color = [0x3e400000, 0x3ee00000, 0x3f000000, 0x3f800000]
    compute = [3, 7, 10, 0x12345678, 4, 14, 10, 0x12345678,
               5, 21, 10, 0x12345678, 6, 28, 10, 0x12345678]
    source_apis = ["legacy_source"] if legacy_only else ["legacy_source", "metal4_source"]
    source_apis += async_apis

    def async_fields(api):
        if "_async_" not in api:
            return {}
        fields = {"async_callback_count": 1, "metal4_task_joined": api == "metal4_async_source"}
        if fields["metal4_task_joined"]:
            fields["metal4_task_status"] = "finished"
        return fields

    for iteration in range(1, repeats + 1):
        offset = (iteration - 1) % len(source_apis)
        modes = source_apis[offset:] + source_apis[:offset]
        if offline:
            modes.append("offline_metallib")
        for api in modes:
            result["samples"].append({
                "api": api, "iteration": iteration,
                "library_operation": "load" if api == "offline_metallib" else "async_compile" if "_async_" in api else "compile",
                "library_ns": 101, "specialization_ns": 102, "render_pso_ns": 103, "compute_pso_ns": 104,
                "render_bits": color * 16, "compute_words": list(compute), "command_status": "completed",
                **async_fields(api),
            })
    for api in source_apis:
        result["rejections"].append({
            "api": api, "library_ns": 105, "returned_nil": True, "error_present": True,
            "error": {"domain": "SyntheticCompilerError", "code": 3},
            **async_fields(api),
        })
    if async_library:
        for api in async_apis:
            for case in ("library_success", "compiler_error"):
                result["async_retention_checks"].append({
                    "api": api, "case": case, "input_snapshot_verified": True, "wait_timed_out": True,
                    "cancellation_requested": False,
                    "timeout_scope": "controlled publication gate after the real compiler callback; not compilation latency",
                    "returned_nil": case == "compiler_error", "error_present": case == "compiler_error",
                    "error": {"domain": "SyntheticCompilerError", "code": 3} if case == "compiler_error" else {},
                    "render_bits": color * 16, "compute_words": list(compute), "command_status": "completed",
                    **async_fields(api),
                })
    return result


def synthetic_factory_probe(created=True):
    return {"scope": runtime.FACTORY_PROBE_SCOPE,
            "status": "library_created" if created else "no_device",
            "factory_attempted": created, "factory_created": created,
            "library_attempted": created, "library_created": created,
            **{key: False for key in runtime.FACTORY_PROBE_FALSE_EVIDENCE}}


class MetalRuntimeReportTests(unittest.TestCase):
    def validate(self, report):
        runtime.validate_native(report, 2, True)

    def test_valid_control_reports_with_and_without_offline(self):
        self.validate(synthetic_report())
        runtime.validate_native(synthetic_report(1, False), 1, False)

    def test_legacy_only_accepts_exact_outputs_without_metal4_but_requires_explicit_mode(self):
        report = synthetic_report(3, True, legacy_only=True)
        runtime.validate_native(report, 3, True, legacy_only=True)
        self.assertEqual([sample["api"] for sample in report["samples"]],
                         ["legacy_source", "offline_metallib"] * 3)
        with self.assertRaisesRegex(ValueError, "comparison mode"):
            runtime.validate_native(report, 3, True)
        for asynchronous in (False, True):
            with self.subTest(asynchronous=asynchronous), \
                    self.assertRaisesRegex(ValueError, "invalid legacy-only"):
                runtime.validate_native(report, 3, False, asynchronous, legacy_only=True)

    def test_legacy_only_cannot_promote_metal4_async_or_lifetime_evidence(self):
        mutations = (
            lambda r: r.__setitem__("metal4_library_verified", True),
            lambda r: r.__setitem__("metal4_compiler_created", True),
            lambda r: r.__setitem__("full_metal4_verified", True),
            lambda r: r.__setitem__("compiler_setup_ns", 1),
            lambda r: r.__setitem__("async_library_requested", True),
            lambda r: r.__setitem__("async_adapter_verified", True),
            lambda r: r.__setitem__("metal4_async_adapter_verified", True),
            lambda r: r.__setitem__("production_async_lifetime_verified", True),
            lambda r: r["async_retention_checks"].append({"synthetic": True}),
            lambda r: r.__setitem__("library_factory_probe_requested", True),
            lambda r: r.__setitem__("library_factory_probe", synthetic_factory_probe()),
            lambda r: r.__setitem__("comparison_mode", "metal4-comparison"),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                report = synthetic_report(legacy_only=True)
                mutation(report)
                with self.assertRaises(ValueError):
                    runtime.validate_native(report, 2, True, legacy_only=True)

    def test_legacy_async_requires_its_own_routes_and_retained_success_and_error(self):
        report = synthetic_report(3, True, True, legacy_only=True)
        runtime.validate_native(report, 3, True, True, legacy_only=True)
        self.assertEqual([sample["api"] for sample in report["samples"]], [
            "legacy_source", "legacy_async_source", "offline_metallib",
            "legacy_async_source", "legacy_source", "offline_metallib",
            "legacy_source", "legacy_async_source", "offline_metallib",
        ])
        self.assertEqual([(check["api"], check["case"]) for check in report["async_retention_checks"]],
                         [("legacy_async_source", "library_success"), ("legacy_async_source", "compiler_error")])
        with self.assertRaisesRegex(ValueError, "async route"):
            runtime.validate_native(report, 3, True, legacy_only=True)
        with self.assertRaisesRegex(ValueError, "comparison mode"):
            runtime.validate_native(report, 3, True, True)

    def test_legacy_async_protocol_failures_and_metal4_claims_cannot_be_hidden(self):
        mutations = (
            lambda r: r.__setitem__("metal4_async_adapter_verified", True),
            lambda r: r.__setitem__("async_apis", ["legacy_async_source", "metal4_async_source"]),
            lambda r: r.__setitem__("async_timing_scope", "submission through callback publication and task join; controlled retention checks excluded"),
            lambda r: r["samples"][1].__setitem__("api", "metal4_async_source"),
            lambda r: r["samples"][1].__setitem__("async_callback_count", 2),
            lambda r: r["samples"][1].__setitem__("metal4_task_joined", True),
            lambda r: r["samples"][1].__setitem__("metal4_task_status", "finished"),
            lambda r: r["samples"][1]["render_bits"].__setitem__(63, 0),
            lambda r: r["rejections"][1].__setitem__("returned_nil", False),
            lambda r: r["rejections"][1].__setitem__("error", {}),
            lambda r: r["async_retention_checks"].pop(),
            lambda r: r["async_retention_checks"][0].__setitem__("input_snapshot_verified", False),
            lambda r: r["async_retention_checks"][0].__setitem__("wait_timed_out", False),
            lambda r: r["async_retention_checks"][0].__setitem__("cancellation_requested", True),
            lambda r: r["async_retention_checks"][0].__setitem__("timeout_scope", "actual compiler timed out"),
            lambda r: r["async_retention_checks"][0]["compute_words"].__setitem__(15, 0),
            lambda r: r["async_retention_checks"][1].__setitem__("error_present", False),
            lambda r: r["async_retention_checks"][1].__setitem__("returned_nil", False),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                report = synthetic_report(1, True, True, legacy_only=True)
                mutation(report)
                with self.assertRaises(ValueError):
                    runtime.validate_native(report, 1, True, True, legacy_only=True)

    def test_legacy_only_requires_both_exact_readbacks_and_real_source_rejection(self):
        for index in range(4):
            for key in ("render_bits", "compute_words"):
                with self.subTest(sample=index, readback=key):
                    report = synthetic_report(legacy_only=True)
                    report["samples"][index][key][-1] ^= 1
                    with self.assertRaisesRegex(ValueError, "readback mismatch"):
                        runtime.validate_native(report, 2, True, legacy_only=True)
        mutations = (
            lambda r: r["samples"].pop(),
            lambda r: r["samples"][1].__setitem__("api", "metal4_source"),
            lambda r: r["samples"][1].__setitem__("library_operation", "compile"),
            lambda r: r["rejections"].clear(),
            lambda r: r["rejections"][0].__setitem__("returned_nil", False),
            lambda r: r["rejections"][0].__setitem__("error", {}),
            lambda r: r["source_options"]["function_constant"].__setitem__("value", 8),
            lambda r: r["device"].__setitem__("available", False),
            lambda r: r.__setitem__("os_version", [15, 0, 0]),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                report = synthetic_report(legacy_only=True)
                mutation(report)
                with self.assertRaises(ValueError):
                    runtime.validate_native(report, 2, True, legacy_only=True)

    def test_async_is_explicit_and_requires_every_retained_result(self):
        report = synthetic_report(3, True, True)
        runtime.validate_native(report, 3, True, True)
        with self.assertRaisesRegex(ValueError, "async route"):
            runtime.validate_native(report, 3, True, False)
        report["async_retention_checks"].pop()
        with self.assertRaisesRegex(ValueError, "retention checks"):
            runtime.validate_native(report, 3, True, True)

    def test_async_protocol_failures_cannot_be_hidden_by_successful_readbacks(self):
        mutations = (
            lambda r: r["samples"][2].__setitem__("async_callback_count", 2),
            lambda r: r["samples"][3].__setitem__("metal4_task_joined", False),
            lambda r: r["rejections"][3].__setitem__("metal4_task_status", "scheduled"),
            lambda r: r["async_retention_checks"][0].__setitem__("input_snapshot_verified", False),
            lambda r: r["async_retention_checks"][0].__setitem__("wait_timed_out", False),
            lambda r: r["async_retention_checks"][0].__setitem__("cancellation_requested", True),
            lambda r: r["async_retention_checks"][1].__setitem__("error_present", False),
            lambda r: r["async_retention_checks"][2]["compute_words"].__setitem__(15, 0),
            lambda r: r["async_retention_checks"][3].__setitem__("returned_nil", False),
        )
        for mutation in mutations:
            report = synthetic_report(2, False, True)
            mutation(report)
            with self.assertRaises(ValueError):
                runtime.validate_native(report, 2, False, True)

    def test_all_pixels_and_compute_threads_are_checked_even_if_native_claims_pass(self):
        for key, index in (("render_bits", 63), ("compute_words", 15)):
            with self.subTest(key=key):
                report = synthetic_report()
                report["samples"][-1][key][index] ^= 1
                with self.assertRaisesRegex(ValueError, "readback mismatch"):
                    self.validate(report)

    def test_missing_duplicate_reordered_or_mislabeled_routes_fail(self):
        mutations = (
            lambda r: r["samples"].pop(),
            lambda r: r["samples"].__setitem__(1, copy.deepcopy(r["samples"][0])),
            lambda r: r["samples"].reverse(),
            lambda r: r["samples"][2].__setitem__("library_operation", "compile"),
        )
        for mutation in mutations:
            report = synthetic_report()
            mutation(report)
            with self.assertRaises(ValueError):
                self.validate(report)

    def test_both_real_errors_require_nil_and_diagnostic(self):
        mutations = (
            lambda r: r["rejections"].pop(),
            lambda r: r["rejections"][1].__setitem__("returned_nil", False),
            lambda r: r["rejections"][0].__setitem__("error_present", False),
            lambda r: r["rejections"][1].__setitem__("error", {}),
        )
        for mutation in mutations:
            report = synthetic_report()
            mutation(report)
            with self.assertRaises(ValueError):
                self.validate(report)

    def test_options_are_exact_and_false_evidence_cannot_be_promoted(self):
        for key, value in (("fast_math", True), ("language", "4.0"), ("fast_math", 0)):
            report = synthetic_report()
            report["source_options"][key] = value
            with self.assertRaisesRegex(ValueError, "options differ"):
                self.validate(report)
        for key in runtime.FALSE_EVIDENCE:
            report = synthetic_report()
            report[key] = True
            with self.assertRaisesRegex(ValueError, "evidence claim"):
                self.validate(report)

    def test_unsupported_or_unexecuted_cannot_be_a_pass(self):
        mutations = (
            lambda r: r.__setitem__("status", "unsupported"),
            lambda r: r.__setitem__("gpu_execution_verified", False),
            lambda r: r.__setitem__("os_version", [15, 0, 0]),
            lambda r: r["device"].__setitem__("supports_metal4", False),
            lambda r: r["samples"][0].__setitem__("command_status", "error"),
        )
        for mutation in mutations:
            report = synthetic_report()
            mutation(report)
            with self.assertRaises(ValueError):
                self.validate(report)

    def test_invalid_durations_and_non_integer_words_fail(self):
        for value in (-1, True, float("nan"), "100"):
            report = synthetic_report()
            report["samples"][0]["library_ns"] = value
            with self.assertRaises(ValueError):
                self.validate(report)
        report = synthetic_report()
        report["samples"][0]["compute_words"][0] = 3.0
        with self.assertRaises(ValueError):
            self.validate(report)

    def test_factory_probe_cannot_claim_gpu_or_full_metal4_or_an_uncreated_library(self):
        runtime.validate_factory_probe(synthetic_factory_probe())
        runtime.validate_factory_probe(synthetic_factory_probe(False))
        for key in runtime.FACTORY_PROBE_FALSE_EVIDENCE:
            with self.subTest(key=key):
                probe = synthetic_factory_probe()
                probe[key] = True
                with self.assertRaisesRegex(ValueError, "evidence claim"):
                    runtime.validate_factory_probe(probe)
        for key in ("factory_attempted", "factory_created", "library_attempted", "library_created"):
            with self.subTest(key=key):
                probe = synthetic_factory_probe()
                probe[key] = False
                with self.assertRaises(ValueError):
                    runtime.validate_factory_probe(probe)
        with self.assertRaisesRegex(ValueError, "missing"):
            runtime.validate_factory_probe(None)


class MetalRuntimeControlTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="metal-runtime-control-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.output = self.directory / "fresh evidence with spaces"

    def arguments(self, *extra):
        return runtime.parse_args(["--output", str(self.output), "--repeat", "1", *extra])

    def test_legacy_only_requires_library_and_rejects_other_metal4_modes_before_work(self):
        self.assertFalse(self.arguments().legacy_only)
        args = self.arguments("--legacy-only", "--library", "authored-macos.metallib")
        self.assertTrue(args.legacy_only)
        asynchronous = self.arguments("--legacy-only", "--library", "authored-macos.metallib", "--async-library")
        self.assertTrue(asynchronous.legacy_only and asynchronous.async_library)
        invalid = (("--legacy-only",),
                   ("--legacy-only", "--async-library"),
                   ("--legacy-only", "--library", "authored-macos.metallib", "--async-library", "--probe-library-factory"),
                   ("--legacy-only", "--library", "authored-macos.metallib", "--probe-library-factory"))
        for extra in invalid:
            with self.subTest(arguments=extra), contextlib.redirect_stderr(io.StringIO()), \
                    patch.object(runtime, "run_experiment") as run, self.assertRaises(SystemExit):
                runtime.main(["--output", str(self.output), *extra])
            run.assert_not_called()
            self.assertFalse(self.output.exists())

    def native_comparison_control(self, native, *, legacy_only, async_library=False, returncode=0):
        """No native tools run: test forwarding and independent report validation."""
        library = self.directory / "synthetic library.metallib"
        library.write_bytes(b"MTLB synthetic; not compiled")
        calls = []

        def command(argv, label, output, report, env, timeout, deadline=None):
            calls.append(argv)
            if label == "native-compile":
                (output / "runtime").write_bytes(b"synthetic binary")
                return {"returncode": 0}
            self.assertEqual(label, "native-run")
            (output / "native.json").write_text(json.dumps(native))
            return {"returncode": returncode}

        extra = ["--library", str(library)]
        if legacy_only:
            extra.append("--legacy-only")
        if async_library:
            extra.append("--async-library")
        with patch.object(runtime.platform, "system", return_value="Darwin"), \
                patch.object(runtime.platform, "mac_ver", return_value=("26.5.1", (), "")), \
                patch.object(runtime, "probe", side_effect=["26.5", "synthetic SDK", "synthetic Xcode", "synthetic clang"]), \
                patch.object(runtime, "run_command", side_effect=command):
            report = runtime.run_experiment(self.arguments(*extra))
        self.assertEqual(report, json.loads((self.output / "result.json").read_text()))
        self.assertEqual(report["native"], native)
        return report, calls[-1]

    def test_legacy_only_command_retains_and_validates_both_routes_without_metal4_claims(self):
        report, command = self.native_comparison_control(synthetic_report(1, True, legacy_only=True),
                                                         legacy_only=True)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["comparison_mode"], "legacy-only")
        self.assertTrue(report["legacy_only_requested"])
        self.assertTrue(report["gpu_execution_verified"])
        self.assertIn("--legacy-only", command)
        self.assertEqual(command[command.index("--library") + 1], str(self.output / "offline.metallib"))
        self.assertNotIn("--async-library", command)
        self.assertNotIn("--probe-library-factory", command)
        for key in ("metal4_library_verified", "metal4_async_adapter_verified", "async_adapter_verified", *runtime.FALSE_EVIDENCE):
            self.assertIs(report[key], False)

    def test_legacy_async_command_verifies_only_requested_legacy_adapter_scope(self):
        report, command = self.native_comparison_control(synthetic_report(1, True, True, legacy_only=True),
                                                         legacy_only=True, async_library=True)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["comparison_mode"], "legacy-only")
        self.assertEqual(report["async_apis"], ["legacy_async_source"])
        self.assertTrue(report["async_adapter_verified"])
        self.assertTrue(report["gpu_execution_verified"])
        self.assertIn("--legacy-only", command)
        self.assertIn("--async-library", command)
        self.assertNotIn("--probe-library-factory", command)
        for key in ("metal4_library_verified", "metal4_async_adapter_verified", *runtime.FALSE_EVIDENCE):
            self.assertIs(report[key], False)

    def test_legacy_only_failure_retains_native_evidence_without_gpu_promotion(self):
        native = synthetic_report(1, True, legacy_only=True)
        native["samples"][-1]["compute_words"][-1] ^= 1
        report, _ = self.native_comparison_control(native, legacy_only=True)
        self.assertEqual(report["status"], "failed")
        self.assertIn("readback mismatch", report["failure"])
        self.assertFalse(report["gpu_execution_verified"])
        self.assertFalse(report["metal4_library_verified"])

    def test_default_mode_stays_unsupported_for_non_metal4_device(self):
        native = synthetic_report(1, True)
        native.update(status="unsupported", failure="a Metal 4 capable device is required",
                      unsupported_reason="metal4_family_unavailable", gpu_execution_verified=False,
                      metal4_library_verified=False, metal4_compiler_created=False)
        native["device"]["supports_metal4"] = False
        native["samples"].clear()
        native["rejections"].clear()
        report, command = self.native_comparison_control(native, legacy_only=False, returncode=78)
        self.assertEqual(report["status"], "unsupported")
        self.assertEqual(report["comparison_mode"], "metal4-comparison")
        self.assertFalse(report["legacy_only_requested"])
        self.assertFalse(report["gpu_execution_verified"])
        self.assertFalse(report["metal4_library_verified"])
        self.assertNotIn("--legacy-only", command)

    def test_non_mac_records_explicit_unsupported_with_source_hashes_and_no_process(self):
        with patch.object(runtime.platform, "system", return_value="Windows"), \
                patch.object(runtime.subprocess, "Popen") as spawn:
            report = runtime.run_experiment(self.arguments())
        spawn.assert_not_called()
        self.assertEqual(report["status"], "unsupported")
        self.assertIs(report["native_compilation_verified"], False)
        self.assertIs(report["gpu_execution_verified"], False)
        for name in ("fixture.metal", "runtime.mm"):
            self.assertEqual(report["inputs"][name]["sha256"], hashlib.sha256((self.output / name).read_bytes()).hexdigest())
        self.assertEqual(report, json.loads((self.output / "result.json").read_text(encoding="utf-8")))
        before = (self.output / "result.json").read_bytes()
        with self.assertRaises(FileExistsError):
            runtime.run_experiment(self.arguments())
        self.assertEqual(before, (self.output / "result.json").read_bytes())

    def test_existing_and_dangling_output_symlinks_are_rejected_before_work(self):
        for existing in (True, False):
            with self.subTest(target_exists=existing):
                target = self.directory / ("existing-link-target" if existing else "absent-link-target")
                if existing:
                    target.mkdir()
                    (target / "keep").write_bytes(b"retained evidence")
                self.output = self.directory / ("existing-output-link" if existing else "dangling-output-link")
                try:
                    self.output.symlink_to(target, target_is_directory=True)
                except (OSError, NotImplementedError) as error:
                    self.skipTest("directory symlink creation unavailable: " + str(error))
                with patch.object(runtime.platform, "system", return_value="Windows"), \
                        patch.object(runtime, "run_command") as launch, self.assertRaises(FileExistsError):
                    runtime.run_experiment(self.arguments())
                launch.assert_not_called()
                self.assertTrue(self.output.is_symlink())
                if existing:
                    self.assertEqual(list(target.iterdir()), [target / "keep"])
                    self.assertEqual((target / "keep").read_bytes(), b"retained evidence")
                else:
                    self.assertFalse(target.exists())

    def test_old_mac_os_is_unsupported_before_compilation(self):
        with patch.object(runtime.platform, "system", return_value="Darwin"), \
                patch.object(runtime.platform, "mac_ver", return_value=("15.7", (), "")), \
                patch.object(runtime.subprocess, "Popen") as spawn:
            report = runtime.run_experiment(self.arguments())
        self.assertEqual(report["status"], "unsupported")
        spawn.assert_not_called()

    def test_empty_library_fails_and_does_not_claim_native_execution(self):
        library = self.directory / "empty.metallib"
        library.write_bytes(b"")
        report = runtime.run_experiment(self.arguments("--library", str(library)))
        self.assertEqual(report["status"], "failed")
        self.assertIn("empty", report["failure"])
        self.assertFalse(report["native_compilation_verified"])

    def test_interruption_retains_report_then_propagates(self):
        with patch.object(runtime.platform, "system", return_value="Darwin"), \
                patch.object(runtime.platform, "mac_ver", return_value=("26.0", (), "")), \
                patch.object(runtime, "probe", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                runtime.run_experiment(self.arguments())
        report = json.loads((self.output / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "interrupted")
        self.assertFalse(report["gpu_execution_verified"])

    def test_sdk_failure_retains_failure_instead_of_native_pass(self):
        with patch.object(runtime.platform, "system", return_value="Darwin"), \
                patch.object(runtime.platform, "mac_ver", return_value=("26.0", (), "")), \
                patch.object(runtime, "probe", side_effect=RuntimeError("SDK discovery failed")):
            report = runtime.run_experiment(self.arguments())
        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["native_compilation_verified"])
        self.assertIn("SDK discovery", report["failure"])

    def test_unsupported_device_diagnostics_and_narrow_probe_are_retained_without_promotion(self):
        for available in (False, True):
            with self.subTest(default_device_available=available):
                self.output = self.directory / ("available" if available else "missing")
                device = {"available": available, "name": "SYNTHETIC VIRTUAL GPU" if available else "",
                          "supports_metal4": False, "metal4_family_queried": available,
                          "supports_metal3": available, "supports_mac2": available,
                          "supports_apple7": False, "supports_apple8": False, "supports_apple9": False,
                          "new_compiler_selector_available": available}
                native = {"schema": 1, "status": "unsupported", "scope": runtime.SCOPE,
                          "failure": "synthetic unsupported device", "device": device,
                          "default_device_available": available, "device_enumeration": "all_devices",
                          "available_device_count": int(available), "available_devices": [device] if available else [],
                          "gpu_execution_verified": False, "library_factory_probe_requested": True,
                          "library_factory_probe": synthetic_factory_probe(available),
                          **{key: False for key in runtime.FALSE_EVIDENCE}}
                calls = []

                def command(argv, label, output, report, env, timeout, deadline=None):
                    calls.append(argv)
                    if label == "native-compile":
                        (output / "runtime").write_bytes(b"synthetic binary")
                        return {"returncode": 0}
                    self.assertEqual(label, "native-run")
                    (output / "native.json").write_text(json.dumps(native))
                    return {"returncode": 78}

                with patch.object(runtime.platform, "system", return_value="Darwin"), \
                        patch.object(runtime.platform, "mac_ver", return_value=("26.5.1", (), "")), \
                        patch.object(runtime, "probe", side_effect=["26.5", "synthetic SDK", "synthetic Xcode", "synthetic clang"]), \
                        patch.object(runtime, "run_command", side_effect=command):
                    report = runtime.run_experiment(self.arguments("--probe-library-factory"))
                self.assertEqual(report["status"], "unsupported")
                self.assertTrue(report["native_compilation_verified"])
                self.assertEqual(report["native"], native)
                self.assertIn("--probe-library-factory", calls[-1])
                for key in ("gpu_execution_verified", *runtime.FALSE_EVIDENCE):
                    self.assertIs(report[key], False)
                self.assertEqual(report, json.loads((self.output / "result.json").read_text()))

    def test_budget_is_optional_positive_finite_and_bounded(self):
        self.assertIsNone(self.arguments().budget)
        self.assertEqual(self.arguments("--budget", "3600").budget, 3600)
        for value in ("0", "-1", "nan", "inf", "1e309", "3601"):
            with self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.arguments("--budget", value)

    def budget_control(self, budget, compile_seconds, native_timeout=False):
        """Fake clocks/processes exercise admission without any Apple compiler."""
        clock, waits = [0.0], []

        def child(command, **kwargs):
            native = Path(command[0]).name == "runtime"
            compile_step = "-o" in command
            message = "26.2" if "--show-sdk-version" in command else "SYNTHETIC CONTROL; NOT APPLE"
            kwargs["stdout"].write((message + "\n").encode())
            if compile_step:
                Path(command[command.index("-o") + 1]).write_bytes(b"synthetic executable placeholder")
            if native:
                (self.output / "native.json").write_text('{"status":"running","synthetic":true}\n')
            process = unittest.mock.Mock(pid=12345, returncode=-9)

            def wait(timeout):
                waits.append(timeout)
                clock[0] += timeout if native else compile_seconds if compile_step else 1
                if native and native_timeout:
                    raise subprocess.TimeoutExpired(command, timeout)
                return 0

            process.wait.side_effect = wait
            return process

        with patch.object(runtime.platform, "system", return_value="Darwin"), \
                patch.object(runtime.platform, "mac_ver", return_value=("26.0", (), "")), \
                patch.object(runtime.time, "monotonic", side_effect=lambda: clock[0]), \
                patch.object(runtime.subprocess, "Popen", side_effect=child) as spawn, \
                patch.object(runtime, "stop_process") as cleanup:
            report = runtime.run_experiment(self.arguments("--budget", str(budget)))
        self.assertEqual(report, json.loads((self.output / "result.json").read_text()))
        return report, waits, spawn.call_count, cleanup.call_count

    def test_budget_stops_native_launch_after_probes_and_compile_and_keeps_evidence(self):
        report, waits, launches, cleanups = self.budget_control(7, 3)
        self.assertEqual((launches, cleanups), (5, 0))
        self.assertEqual(waits, [7, 6, 5, 4, 3])
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertTrue(report["native_compilation_verified"])
        self.assertFalse(report["gpu_execution_verified"])
        self.assertEqual(report["commands"][-1]["label"], "native-run")
        self.assertFalse(report["commands"][-1]["process_started"])
        self.assertTrue((self.output / "runtime").is_file())
        self.assertIn("SYNTHETIC", (self.output / "native-compile.stdout.log").read_text())

    def test_native_wait_uses_remaining_budget_and_preserves_partial_failure(self):
        report, waits, launches, cleanups = self.budget_control(8, 2, native_timeout=True)
        self.assertEqual((launches, cleanups), (6, 1))
        self.assertEqual(waits, [8, 7, 6, 5, 4, 2])
        self.assertEqual(report["status"], "budget_exhausted")
        event = report["commands"][-1]
        self.assertEqual(event["timeout_cap_seconds"], 180)
        self.assertEqual(event["timeout_seconds"], 2)
        self.assertEqual(event["status"], "timed_out")
        self.assertTrue(event["budget_exhausted"])
        self.assertFalse(report["gpu_execution_verified"])
        self.assertTrue(json.loads((self.output / "native.json").read_text())["synthetic"])

    def test_budget_exhausted_during_inputs_never_starts_first_probe(self):
        clock = [0.0]
        original = runtime.sha256

        def inspected(path):
            clock[0] = 2.0
            return original(path)

        with patch.object(runtime.platform, "system", return_value="Darwin"), \
                patch.object(runtime.platform, "mac_ver", return_value=("26.0", (), "")), \
                patch.object(runtime.time, "monotonic", side_effect=lambda: clock[0]), \
                patch.object(runtime, "sha256", side_effect=inspected), \
                patch.object(runtime.subprocess, "Popen") as spawn:
            report = runtime.run_experiment(self.arguments("--budget", "1"))
        spawn.assert_not_called()
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertEqual(report["commands"][0]["label"], "sdk-version")
        self.assertFalse(report["commands"][0]["process_started"])
        self.assertFalse(report["native_compilation_verified"])
        self.assertEqual(len(report["inputs"]), 3)

    def test_budget_is_rechecked_after_process_startup(self):
        self.output.mkdir()
        report = {"commands": []}
        process = unittest.mock.Mock(pid=12345)
        process.wait.return_value = 17
        with patch.object(runtime.time, "monotonic", side_effect=[2.0, 4.0]), \
                patch.object(runtime.subprocess, "Popen", return_value=process):
            event = runtime.run_command(["synthetic"], "startup", self.output, report, {}, 30, deadline=5)
        process.wait.assert_called_once_with(timeout=1)
        self.assertEqual(event["timeout_seconds"], 3)
        self.assertEqual(event["returncode"], 17)

    def test_inherited_test_deadline_never_extends_explicit_deadline(self):
        self.output.mkdir()
        for explicit, expected in ((None, 2), (12, 2), (9, 1)):
            with self.subTest(explicit=explicit):
                process = unittest.mock.Mock(pid=12345)
                process.wait.return_value = 0
                report = {"commands": []}
                with patch.object(runtime.time, "monotonic", return_value=8), \
                        patch.object(runtime.subprocess, "Popen", return_value=process):
                    event = runtime.run_command(["synthetic"], "inherited", self.output, report,
                        {runtime.TEST_DEADLINE_ENV: "10"}, 30, deadline=explicit)
                process.wait.assert_called_once_with(timeout=expected)
                self.assertTrue(event["test_deadline_inherited"])
        process = unittest.mock.Mock(pid=12345)
        process.wait.return_value = 0
        with patch.object(runtime.time, "monotonic", side_effect=AssertionError("unexpected deadline clock")), \
                patch.object(runtime.subprocess, "Popen", return_value=process):
            runtime.run_command(["synthetic"], "legacy", self.output, {"commands": []}, {}, 30)
        process.wait.assert_called_once_with(timeout=30)

    def test_invalid_or_expired_inherited_deadline_keeps_logs_without_launch(self):
        self.output.mkdir()
        for value in ("", "nan", "inf", "-inf", "0", "-1", "invalid", "1e999", "7"):
            with self.subTest(value=value):
                report = {"commands": []}
                with patch.object(runtime.time, "monotonic", return_value=8), \
                        patch.object(runtime.subprocess, "Popen") as spawn, self.assertRaises((ValueError, runtime.BudgetExhausted)):
                    runtime.run_command(["synthetic"], "invalid", self.output, report,
                        {runtime.TEST_DEADLINE_ENV: value}, 30)
                spawn.assert_not_called()
                event = report["commands"][0]
                self.assertFalse(event["process_started"])
                self.assertTrue((self.output / event["stderr"]).is_file())
                self.assertEqual(event["status"], "budget_exhausted" if value == "7" else "invalid_deadline")

    def run_child(self, script, timeout=10):
        self.output.mkdir()
        report = {"commands": []}
        command = [sys.executable, "-u", "-c", script]
        event = runtime.run_command(command, "synthetic-child", self.output, report, os.environ.copy(), timeout)
        self.assertEqual(report["commands"], [event])
        return event

    def test_real_synthetic_child_retains_both_streams_and_nonzero_exit(self):
        event = self.run_child("import sys; print('SYNTHETIC stdout'); print('SYNTHETIC stderr', file=sys.stderr); sys.exit(23)")
        self.assertEqual(event["returncode"], 23)
        self.assertEqual(event["status"], "completed")
        self.assertIn("SYNTHETIC stdout", (self.output / event["stdout"]).read_text())
        self.assertIn("SYNTHETIC stderr", (self.output / event["stderr"]).read_text())

    def test_real_synthetic_child_timeout_retains_partial_output_and_is_reaped(self):
        self.output.mkdir()
        report = {"commands": []}
        with self.assertRaisesRegex(RuntimeError, "deadline"):
            runtime.run_command([sys.executable, "-u", "-c", "import time; print('before timeout'); time.sleep(20)"],
                                "timeout", self.output, report, os.environ.copy(), 1)
        event = report["commands"][0]
        self.assertEqual(event["status"], "timed_out")
        self.assertIsNotNone(event["returncode"])
        self.assertIn("before timeout", (self.output / event["stdout"]).read_text())
        self.assertLess(event["duration_ns"], 8_000_000_000)

    def test_command_interrupt_stops_child_before_propagating(self):
        self.output.mkdir()
        report = {"commands": []}
        process = unittest.mock.Mock()
        process.pid = 12345
        process.wait.side_effect = [KeyboardInterrupt, 0]
        process.returncode = -9
        with patch.object(runtime.subprocess, "Popen", return_value=process), \
                patch.object(runtime.os, "name", "nt"):
            with self.assertRaises(KeyboardInterrupt):
                runtime.run_command(["synthetic"], "interrupted", self.output, report, {}, 10)
        process.kill.assert_called_once()
        self.assertEqual(process.wait.call_count, 2)
        self.assertEqual(report["commands"][0]["status"], "interrupted")

    def test_interrupt_immediately_after_popen_return_still_cleans_owned_child(self):
        self.output.mkdir()
        report = {"commands": []}
        process = unittest.mock.Mock(pid=12345, returncode=-9)
        interrupted = []

        def trace(frame, event, argument):
            if (frame.f_code is runtime.run_command.__code__ and event == "line"
                    and frame.f_locals.get("process") is process and not interrupted):
                interrupted.append(True)
                raise KeyboardInterrupt()
            return trace

        previous = sys.gettrace()
        with patch.object(runtime.subprocess, "Popen", return_value=process), \
                patch.object(runtime, "stop_process") as cleanup:
            try:
                sys.settrace(trace)
                with self.assertRaises(KeyboardInterrupt):
                    runtime.run_command(["synthetic"], "startup-interrupt", self.output, report, {}, 10)
            finally:
                sys.settrace(previous)
        self.assertTrue(interrupted)
        process.wait.assert_not_called()
        cleanup.assert_called_once_with(process, report["commands"][0])
        self.assertEqual(report["commands"][0]["status"], "interrupted")

    def test_cli_does_not_return_success_for_unsupported(self):
        with patch.object(runtime.platform, "system", return_value="Windows"), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(runtime.main(["--output", str(self.output)]), 78)


if __name__ == "__main__":
    unittest.main()
