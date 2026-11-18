"""Portable authored report/orchestration controls; no Apple API/GPU evidence."""
import argparse
import contextlib
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal4_archive_test", ROOT / "experiments/metal-compile/archive_metal4.py")
fixture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixture)


def error():
    return {"present": True, "domain": "SyntheticTestError", "code": 7, "description": "control, not Apple evidence"}


def native(case="probe-device", nil_rejected=False):
    probe = case == "probe-device"
    supplied = case.endswith("-supplied")
    recorded = case.startswith("recorded-")
    created = not probe and recorded and not (nil_rejected and not supplied)
    result = {
        "schema": 1, "scope": fixture.NATIVE_SCOPE, "case": case,
        "process_id": 100 if probe else 101 + fixture.CASES.index(case), "os_version": [26, 0, 0],
        "kernel": "" if probe else "archive_recorded" if recorded else "archive_unrecorded",
        "library_mode": "none" if probe else "supplied" if supplied else "nil",
        "status": "device_supported" if probe else "recorded_readback_matched" if created else
            "boundary_rejected" if recorded else "unrecorded_rejected",
        "source_library_compiles": 0, "compiler_factory_calls": 0,
        "device": {"available": True, "name": "SYNTHETIC NO GPU", "supports_metal4": True,
                   "archive_selector_available": True, "architecture_query_status": "observed", "architecture_name": "applegpu_test"},
        "counters": {"library_load_attempts": int(supplied), "archive_load_attempts": int(not probe),
                     "direct_archive_lookup_attempts": int(not probe)},
        "library": {"attempted": supplied, "created": supplied, "function_names": ["archive_recorded", "archive_unrecorded"] if supplied else [], "error": {"present": False}},
        "archive": {"attempted": not probe, "created": not probe, "error": {"present": False}},
        "lookup": {"attempted": not probe, "pipeline_created": created, "error": {"present": False} if probe or created else error()},
        "gpu": {"attempted": created, "command_status": "completed" if created else "not_submitted",
                "command_status_value": 4 if created else None, "error": {"present": False},
                "words_u32": fixture.WORDS.copy() if created else [], "expected_u32": fixture.WORDS.copy() if recorded else [],
                "readback_matches": created},
        **{key: False for key in fixture.FALSE_EVIDENCE},
    }
    return result


def unsupported():
    result = native()
    result.update(status="unsupported", unsupported_reason="metal4_family_unavailable")
    result["device"]["supports_metal4"] = False
    return result


class Metal4ArchiveReportControls(unittest.TestCase):
    def test_four_independent_cases_and_explicit_nil_rejection(self):
        for rejected in (False, True):
            reports = [native(), *(native(case, rejected) for case in fixture.CASES)]
            for row in reports:
                code = 3 if row["status"] == "boundary_rejected" else 0
                fixture.validate_native(row, row["case"], code)
            summary = fixture.aggregate_native(reports)
            self.assertEqual(summary["status"], "boundary_rejected" if rejected else "passed")
            self.assertIs(summary["library_free_lookup_verified"], not rejected)

    def test_nil_library_cannot_hide_a_load_source_compile_or_fallback(self):
        changes = (
            lambda r: r["counters"].__setitem__("library_load_attempts", 1),
            lambda r: r["library"].__setitem__("attempted", True),
            lambda r: r["library"].__setitem__("created", True),
            lambda r: r["library"].__setitem__("function_names", ["archive_recorded"]),
            lambda r: r.__setitem__("library_mode", "supplied"),
            lambda r: r.__setitem__("source_library_compiles", 1),
            lambda r: r.__setitem__("compiler_factory_calls", 1),
            lambda r: r["counters"].__setitem__("direct_archive_lookup_attempts", 2),
        )
        for change in changes:
            row = native("recorded-nil")
            change(row)
            with self.assertRaises(ValueError):
                fixture.validate_native(row, "recorded-nil", 0)

    def test_every_output_word_is_checked_without_bool_integer_aliases(self):
        for index in range(16):
            row = native("recorded-supplied")
            row["gpu"]["words_u32"][index] ^= 1
            with self.subTest(index=index), self.assertRaises(ValueError):
                fixture.validate_native(row, row["case"], 0)
        row = native("recorded-supplied")
        row["schema"] = True
        with self.assertRaises(ValueError):
            fixture.validate_native(row, row["case"], 0)
        for key in ("source_library_compiles", "compiler_factory_calls"):
            row = native("recorded-supplied")
            row[key] = False
            with self.assertRaises(ValueError):
                fixture.validate_native(row, row["case"], 0)

    def test_unrecorded_kernel_must_exist_and_fail_direct_lookup(self):
        for field in ("function_names",):
            row = native("unrecorded-supplied")
            row["library"][field] = ["archive_recorded"]
            with self.assertRaises(ValueError):
                fixture.validate_native(row, row["case"], 0)
        for case in ("unrecorded-supplied", "unrecorded-nil"):
            for key, value in (("pipeline_created", True), ("error", {"present": False})):
                row = native(case)
                row["lookup"][key] = value
                with self.subTest(case=case, key=key), self.assertRaises(ValueError):
                    fixture.validate_native(row, case, 0)

    def test_nonnil_warning_is_retained_but_gpu_error_fails(self):
        row = native("recorded-supplied")
        for key in ("library", "archive", "lookup"):
            row[key]["error"] = error()
        fixture.validate_native(row, row["case"], 0)
        row["gpu"]["error"] = error()
        with self.assertRaises(ValueError):
            fixture.validate_native(row, row["case"], 0)

    def test_recorded_nil_rejection_never_becomes_passed(self):
        row = native("recorded-nil", True)
        fixture.validate_native(row, row["case"], 3)
        with self.assertRaises(ValueError):
            fixture.validate_native(row, row["case"], 0)
        row["status"] = "recorded_readback_matched"
        with self.assertRaises(ValueError):
            fixture.validate_native(row, row["case"], 0)
        row = native("recorded-supplied")
        row.update(status="boundary_rejected")
        row["lookup"].update(pipeline_created=False, error=error())
        row["gpu"].update(attempted=False, words_u32=[], readback_matches=False)
        with self.assertRaises(ValueError):
            fixture.validate_native(row, row["case"], 3)

    def test_unsupported_requires_no_work_and_matching_capability_evidence(self):
        row = unsupported()
        fixture.validate_native(row, "probe-device", 78)
        with self.assertRaises(ValueError):
            fixture.validate_native(row, "probe-device", 0)
        row["device"]["supports_metal4"] = True
        with self.assertRaises(ValueError):
            fixture.validate_native(row, "probe-device", 78)
        row["device"]["architecture_query_status"] = "exception"
        row["unsupported_reason"] = "device_architecture_unavailable"
        fixture.validate_native(row, "probe-device", 78)
        row["counters"]["archive_load_attempts"] = 1
        with self.assertRaises(ValueError):
            fixture.validate_native(row, "probe-device", 78)

    def test_process_os_device_and_case_identity_cannot_be_shared_or_changed(self):
        base = [native(), *(native(case) for case in fixture.CASES)]
        for change in (
            lambda rows: rows[2].__setitem__("process_id", rows[1]["process_id"]),
            lambda rows: rows[3].__setitem__("device", {"name": "different"}),
            lambda rows: rows[4].__setitem__("os_version", [26, 1, 0]),
            lambda rows: rows.reverse(), lambda rows: rows.pop(),
        ):
            rows = copy.deepcopy(base)
            change(rows)
            with self.assertRaises(ValueError):
                fixture.aggregate_native(rows)

    def test_production_or_zero_internal_compilation_claims_are_rejected(self):
        for key in fixture.FALSE_EVIDENCE:
            row = native("recorded-supplied")
            row[key] = True
            with self.subTest(field=key), self.assertRaises(ValueError):
                fixture.validate_native(row, row["case"], 0)

    def test_script_records_only_one_control_from_two_kernel_ir(self):
        self.assertEqual(fixture.pipeline_script(), {
            "libraries": [{"label": "authored", "path": "authored.metallib"}],
            "pipeline_descriptors": {"compute_pipeline_descriptors": [
                {"compute_function_descriptor": "lib:authored#archive_recorded"}]},
        })


class Metal4ArchiveRunnerControls(unittest.TestCase):
    def test_compile_only_has_explicit_separate_scope_and_bounds(self):
        args = fixture.parse_args(["--compile-only", "--output", "unused"])
        self.assertTrue(args.compile_only)
        self.assertEqual(args.budget, 240)
        for value in (0, -1, float("inf"), float("nan"), 241):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                fixture.budget_seconds(value)

    def test_non_apple_host_preserves_sources_without_running_tools(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "fresh"
            args = fixture.parse_args(["--output", str(output)])
            with patch.object(fixture.platform, "system", return_value="Windows"), \
                    patch.object(fixture.control, "run_command") as command:
                result, code = fixture.run(args)
            command.assert_not_called()
            self.assertEqual(code, 78)
            self.assertEqual(result["status"], "unsupported")
            self.assertFalse(result["gpu_execution_verified"])
            self.assertEqual(result["native_reports"], [])
            self.assertEqual(set(result["inputs"]), set(fixture.SNAPSHOTS))
            self.assertEqual(json.loads((output / "result.json").read_text())["status"], "unsupported")

    def test_existing_evidence_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            evidence = output / "result.json"
            evidence.write_text("original")
            with self.assertRaises(FileExistsError):
                fixture.run(fixture.parse_args(["--output", str(output)]))
            self.assertEqual(evidence.read_text(), "original")


class Metal4ArchiveOrchestrationControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk"
        self.sdk.mkdir()
        names = ("xcrun", "clang++", "metal", "metallib", "metal-nt", "metal-tt", "metal-config", "metal-lipo")
        self.tools = {name: self.root / name for name in names}
        for path in self.tools.values():
            path.write_bytes(b"synthetic non-executable tool")
        self.output = self.root / "result"
        self.calls = []
        self.nil_rejected = False
        self.unsupported = False
        self.fail_label = None
        self.mutate_after = None
        self.omit_report_on_failure = False
        self.budget_failure = False

    def command(self, argv, label, output, report, env, timeout, deadline=None):
        argv = list(map(str, argv))
        self.calls.append((label, argv, timeout, deadline))
        event = {"label": label, "argv": argv, "returncode": 0, "timed_out": False, "budget_exhausted": False,
                 "stdout": label + ".stdout.txt", "stderr": label + ".stderr.txt"}
        text = "synthetic version"
        if label.startswith("resolve-"):
            name = label.removeprefix("resolve-")
            text = str(self.sdk if name.endswith("-sdk") else self.tools["clang++" if name in ("clangxx", "ios-clangxx") else name])
        elif label == "macos-sdk-version":
            text = "26.5"
        elif label in ("query-supported-architectures", "inspect-archive-architecture"):
            text = "applegpu_test"
        elif label.startswith("compile-") or label.startswith("link-") or label == "generate-recorded-archive":
            path = Path(argv[argv.index("-o") + 1])
            path.write_bytes(b"MTLBsynthetic" if label == "link-authored-ir" else b"synthetic artifact")
        elif label.startswith("native-"):
            case = label.removeprefix("native-")
            value = unsupported() if case == "probe-device" and self.unsupported else native(case, self.nil_rejected)
            event["returncode"] = 78 if value["status"] == "unsupported" else 3 if value["status"] == "boundary_rejected" else 0
            Path(argv[argv.index("--report") + 1]).write_text(json.dumps(value), encoding="utf-8")
            if case == self.mutate_after:
                (output / "recorded.mtl4archive").write_bytes(b"changed archive")
        if label == self.fail_label:
            event.update(returncode=None, timed_out=True, budget_exhausted=self.budget_failure)
            if self.omit_report_on_failure and label.startswith("native-"):
                Path(argv[argv.index("--report") + 1]).unlink()
        (output / event["stdout"]).write_text(text, encoding="utf-8")
        (output / event["stderr"]).write_text("synthetic timeout" if event["timed_out"] else "", encoding="utf-8")
        report["commands"].append(event)
        return event, text, ""

    def run_fixture(self, compile_only=False):
        args = fixture.parse_args(["--output", str(self.output), *( ["--compile-only"] if compile_only else [])])
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(fixture.platform, "system", return_value="Darwin"))
            stack.enter_context(patch.object(fixture.platform, "mac_ver", return_value=("26.0", ("", "", ""), "arm64")))
            stack.enter_context(patch.object(fixture.shutil, "which", return_value=str(self.tools["xcrun"])))
            stack.enter_context(patch.object(fixture.control, "run_command", side_effect=self.command))
            return fixture.run(args)

    def test_compile_only_never_probes_device_or_compiles_a_shader(self):
        report, code = self.run_fixture(compile_only=True)
        self.assertEqual((report["status"], code), ("sdk_compilation_passed", 0))
        self.assertTrue(report["mac_native_compile_verified"])
        self.assertTrue(report["ios_object_compile_verified"])
        self.assertFalse(report["offline_archive_generated"])
        self.assertFalse(report["gpu_execution_verified"])
        self.assertEqual(report["native_reports"], [])
        self.assertFalse(any(label.startswith("native-") or label == "compile-authored-ir" for label, *_ in self.calls))

    def test_runtime_cases_are_process_separated_and_nil_omits_library_argument(self):
        report, code = self.run_fixture()
        self.assertEqual((report["status"], code), ("passed", 0))
        self.assertTrue(report["library_free_lookup_verified"])
        calls = [argv for label, argv, *_ in self.calls if label.startswith("native-")]
        self.assertEqual(len(calls), 5)
        for argv in calls:
            case = argv[argv.index("--case") + 1]
            self.assertEqual("--library" in argv, case.endswith("-supplied"))
            self.assertEqual("--archive" in argv, case != "probe-device")
        self.assertEqual(len({deadline for _, _, _, deadline in self.calls}), 1)
        self.assertTrue(all(timeout == 65 for _, _, timeout, _ in self.calls))
        link = next(argv for label, argv, *_ in self.calls if label == "link-authored-ir")
        self.assertEqual(link[0], str(self.tools["metallib"]))

    def test_no_supported_device_stops_before_offline_generation(self):
        self.unsupported = True
        report, code = self.run_fixture()
        self.assertEqual((report["status"], code), ("unsupported", 78))
        self.assertEqual(len(report["native_reports"]), 1)
        self.assertFalse(report["offline_archive_generated"])
        self.assertFalse(report["direct_archive_control_verified"])
        self.assertNotIn("compile-authored-ir", [row[0] for row in self.calls])

    def test_nil_boundary_rejection_keeps_remaining_negative_controls_and_nonzero_exit(self):
        self.nil_rejected = True
        report, code = self.run_fixture()
        self.assertEqual((report["status"], code), ("boundary_rejected", 3))
        self.assertEqual(len(report["native_reports"]), 5)
        self.assertTrue(report["direct_archive_control_verified"])
        self.assertFalse(report["library_free_lookup_verified"])

    def test_timeout_preserves_diagnostic_and_never_sets_success(self):
        self.fail_label = "generate-recorded-archive"
        report, code = self.run_fixture()
        self.assertEqual((report["status"], code), ("failed", 1))
        self.assertFalse(report["offline_archive_generated"])
        self.assertFalse(report["gpu_execution_verified"])
        self.assertEqual((self.output / (self.fail_label + ".stderr.txt")).read_text(), "synthetic timeout")
        self.assertEqual(json.loads((self.output / "result.json").read_text())["status"], "failed")

    def test_lookup_cannot_mutate_the_input_archive(self):
        self.mutate_after = "recorded-nil"
        report, code = self.run_fixture()
        self.assertEqual((report["status"], code), ("failed", 1))
        self.assertIn("changed original archive", report["error"])
        self.assertFalse(report["library_free_lookup_verified"])

    def test_missing_native_report_keeps_the_original_budget_failure(self):
        self.fail_label = "native-recorded-nil"
        self.omit_report_on_failure = True
        self.budget_failure = True
        report, code = self.run_fixture()
        self.assertEqual((report["status"], code), ("budget_exhausted", 1))
        self.assertIn("BudgetExhausted", report["error"])
        self.assertEqual(report["native_report_errors"][0]["case"], "recorded-nil")
        self.assertFalse(report["library_free_lookup_verified"])

    def test_dangling_output_symlink_does_not_claim_its_target(self):
        target = self.root / "uncreated-target"
        try:
            self.output.symlink_to(target, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("host does not permit symlinks")
        with self.assertRaises(FileExistsError):
            self.run_fixture()
        self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
