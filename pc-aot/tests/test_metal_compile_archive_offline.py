"""Authored fake-tool controls; these tests do not provide Apple/GPU evidence."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("archive_offline_tested", ROOT / "experiments/metal-compile/archive_offline.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)
MISSING_ERROR = "air-nt.exe: error: no constant value specified for required function constant variable with index 0 and name 'archive_value' in 'synthetic.airc'\n"
MISSING_WARNING = MISSING_ERROR.replace(": error:", ": warning:")


class ArchiveOfflineControls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        for name in probe.TOOL_NAMES:
            (self.tools / (name + ".exe")).write_bytes(("fake " + name).encode())
        self.output = self.root / "result"
        self.calls = []
        self.deadlines = []

    def args(self, **overrides):
        args = probe.parse_args(["--tools", str(self.tools), "--output", str(self.output)])
        for name, value in overrides.items():
            setattr(args, name, value)
        return args

    def invoke(self, *, fail=None, timeout=None, budget=None, interrupt=None, invalid_library=False,
               missing_archive=False, query=None, inspected=None, truncated=None, system="Windows",
               specialized=False, negative_exit=1, negative_diagnostic=MISSING_ERROR,
               negative_product=False, negative_event=None, identical_specializations=False):
        def command(argv, label, output, report, env, limit, *, deadline):
            argv = [str(value) for value in argv]
            self.calls.append((label, argv))
            self.deadlines.append(deadline)
            self.assertEqual(limit, 65)
            self.assertFalse(any(name in env for name in probe.control.ENV_OPTIONS))
            self.assertEqual(output, self.output.resolve())
            stdout = "authored fake version\n"
            negative = label == "generate-missing-required-constant-mtl4-archive"
            stderr = negative_diagnostic if negative else "authored failure" if label == fail else ""
            if label == "query-ios26-apple9-architectures":
                stdout = query if query is not None else "applegpu_g15g applegpu_g16p applegpu_g18p\n"
            elif label.startswith("inspect-"):
                stdout = inspected if inspected is not None else probe.ARCH + "\n"
            if label == "compile-authored-ios26":
                source = Path(argv[argv.index("-o") - 1])
                self.assertTrue(source.is_absolute())
                self.assertEqual(source.parent, output)
                source_name = "archive_specialized.metal" if specialized else "archive_offline_fixture.metal"
                self.assertEqual(source.read_bytes(), (probe.HERE / source_name).read_bytes())
                (output / "authored.air").write_bytes(b"fake authored AIR")
            if label == "link-authored-ir-library":
                (output / "authored.metallib").write_bytes(b"invalid" if invalid_library else b"MTLBfake authored library")
            if label.startswith("generate-"):
                script = next(Path(value) for value in argv if value.endswith(".mtl4-json"))
                self.assertFalse(any(value.endswith(".mtlp-json") for value in argv))
                expected = probe.specialized_descriptor(dict(probe.SPECIALIZED_CASES)[script.stem]) if specialized else probe.descriptor(script.stem)
                self.assertEqual(json.loads(script.read_text()), expected)
                self.assertEqual(argv[argv.index("-arch") + 1], "applegpu_g16p")
                self.assertEqual(argv[argv.index("-gpu-family") + 1], "apple9")
                self.assertEqual(argv[argv.index("-target") + 1], "air64-apple-ios26.0")
                self.assertIn("-disable-disk-cache", argv)
                if (negative and negative_product) or (not negative and not missing_archive):
                    content = b"fake authored archive"
                    if specialized and not identical_specializations:
                        content += script.stem.encode()
                    Path(argv[argv.index("-o") + 1]).write_bytes(content)
            event = {"label": label, "argv": argv, "timed_out": label == timeout,
                     "budget_exhausted": label == budget, "returncode": 1 if label == fail else 0,
                     "stdout": label + ".stdout.txt", "stderr": label + ".stderr.txt",
                     "stdout_preview_truncated": label == truncated, "stderr_preview_truncated": False,
                     "process_started": True, "interrupted": False}
            if negative:
                event.update(returncode=negative_exit, **(negative_event or {}))
            if label == timeout or label == budget:
                event.update(returncode=None, error="authored timeout")
            (output / event["stdout"]).write_text(stdout)
            (output / event["stderr"]).write_text(stderr)
            report["commands"].append(event)
            if interrupt is not None and label == interrupt[0]:
                raise interrupt[1]
            return event, stdout, stderr
        with patch.object(probe.platform, "system", return_value=system), \
                patch.object(probe.control, "run_command", side_effect=command), \
                patch.dict(os.environ, {"CFLAGS": "must not reach tool"}), \
                patch.object(probe.time, "monotonic", return_value=100.0):
            return probe.run(self.args(specialized=specialized))

    def saved(self):
        return json.loads((self.output / "result.json").read_text())

    def test_fixed_constant_free_descriptors_are_metal4_not_legacy(self):
        compute = probe.descriptor("compute")
        self.assertEqual(compute["libraries"], [{"label": "authored", "path": "authored.metallib"}])
        self.assertEqual(compute["pipeline_descriptors"], {"compute_pipeline_descriptors": [
            {"compute_function_descriptor": "lib:authored#archive_recorded"}]})
        render = probe.descriptor("render")["pipeline_descriptors"]["render_pipeline_descriptors"]
        self.assertEqual(len(render), 1)
        self.assertEqual(render[0]["vertex_function_descriptor"], "lib:authored#archive_vertex")
        self.assertEqual(render[0]["fragment_function_descriptor"], "lib:authored#archive_fragment")
        self.assertEqual(render[0]["color_attachments"], [{"pixel_format": "RGBA8Unorm", "write_mask": "All", "blending_state": "Disabled"}])
        with self.assertRaises(ValueError):
            probe.descriptor("arbitrary-input")
        first = probe.descriptor("compute")
        first["libraries"][0]["path"] = "changed"
        self.assertEqual(probe.descriptor("compute")["libraries"][0]["path"], "authored.metallib")

    def test_two_serial_archives_have_exact_profile_hashes_and_no_runtime_claim(self):
        before = {name: (probe.HERE / name).read_bytes() for name in probe.SNAPSHOTS}
        report = self.invoke()
        self.assertEqual(report["status"], probe.SUCCESS)
        self.assertEqual(self.saved(), report)
        self.assertEqual(len(self.calls), 12)
        self.assertEqual([label for label, _ in self.calls][-4:], ["generate-compute-mtl4-archive", "inspect-compute-archive",
            "generate-render-mtl4-archive", "inspect-render-archive"])
        self.assertTrue(all(deadline == 280.0 for deadline in self.deadlines))
        self.assertTrue(all(report[name] is False for name in probe.FALSE_EVIDENCE))
        self.assertTrue(all(row["offline_mtl4_script_archive_generation_verified"] for row in report["cases"]))
        self.assertTrue(report["offline_ir_library_generation_verified"])
        self.assertEqual(report["requested"]["function_constants"], {})
        self.assertEqual(report["requested"]["preprocessor_macros"], {})
        compile_argv = dict(self.calls)["compile-authored-ios26"]
        self.assertIn("-fmetal-math-mode=safe", compile_argv)
        self.assertIn("-fmetal-math-fp32-functions=precise", compile_argv)
        self.assertIn("-std=metal3.1", compile_argv)
        self.assertFalse(any(value.startswith("-D") for value in compile_argv))
        self.assertEqual(Path(dict(self.calls)["link-authored-ir-library"][0]).name, "metallib.exe")
        self.assertEqual(report["schema_source"]["extension"], ".mtl4-json")
        self.assertEqual(len(report["observed_outputs"]), 4)
        for name, data in before.items():
            self.assertEqual((probe.HERE / name).read_bytes(), data)
            self.assertEqual((self.output / name).read_bytes(), data)
            self.assertEqual(report["inputs"][name]["sha256"], hashlib.sha256(data).hexdigest())
        for name, info in report["tools"].items():
            self.assertEqual(info["sha256"], hashlib.sha256((self.tools / (name + ".exe")).read_bytes()).hexdigest())
        self.assertFalse((self.output / "result.tmp").exists())

    def test_specialized_schema_uses_fixed_indexed_uint_values_and_explicit_omission(self):
        self.assertFalse(self.args().specialized)
        self.assertTrue(probe.parse_args(["--tools", str(self.tools), "--output", str(self.output), "--specialized"]).specialized)
        for value in (7, 11, None):
            script = probe.specialized_descriptor(value)
            function = script["function_descriptors"]["specialized_function_descriptors"][0]
            self.assertEqual(function["function_descriptor"], "lib:authored#archive_specialized")
            self.assertEqual(function["label"], "specialized")
            self.assertEqual(script["pipeline_descriptors"], {"compute_pipeline_descriptors": [
                {"compute_function_descriptor": "fnd:specialized"}]})
            if value is None:
                self.assertNotIn("constant_values", function)
            else:
                self.assertEqual(function["constant_values"], [{"id_type": "FunctionConstantIndex", "id": {"data": 0},
                    "value_type": "ConstantUInt", "value": {"data": value}}])
        for value in (True, 7.0, "7", 0, 12):
            with self.subTest(value=value), self.assertRaises(ValueError):
                probe.specialized_descriptor(value)

    def test_specialized_synthetic_rejection_has_three_distinct_controls_and_no_runtime_claim(self):
        report = self.invoke(specialized=True)
        self.assertEqual(report["status"], probe.SPECIALIZED_SUCCESS)
        self.assertEqual(report["scope"], probe.SPECIALIZED_SCOPE)
        self.assertEqual(self.saved(), report)
        self.assertTrue(report["specialized"])
        self.assertEqual(len(self.calls), 13)
        self.assertEqual([row["function_constant_uint_index_0"] for row in report["cases"]], [7, 11, None])
        self.assertTrue(report["distinct_specialization_archive_bytes_observed"])
        self.assertTrue(report["missing_required_constant_rejection_verified"])
        self.assertTrue(report["offline_mtl4_script_archive_generation_verified"])
        self.assertTrue(all(report[name] is False for name in probe.FALSE_EVIDENCE))
        self.assertTrue(all(row["offline_mtl4_script_archive_generation_verified"] for row in report["cases"][:2]))
        self.assertFalse(report["cases"][2]["offline_mtl4_script_archive_generation_verified"])
        self.assertTrue(report["cases"][2]["compiler_rejection_verified"])
        self.assertEqual(report["cases"][2]["returncode"], 1)
        self.assertEqual(report["cases"][2]["stderr_preview"], MISSING_ERROR)
        self.assertNotIn("archive_offline_fixture.metal", report["inputs"])
        source = (probe.HERE / "archive_specialized.metal").read_bytes()
        self.assertEqual((self.output / "archive_specialized.metal").read_bytes(), source)
        self.assertEqual(report["inputs"]["archive_specialized.metal"]["sha256"], hashlib.sha256(source).hexdigest())
        self.assertTrue(all(deadline == 280.0 for deadline in self.deadlines))
        self.assertFalse(any("-Werror" in argv for _, argv in self.calls))
        self.assertEqual(len(report["observed_outputs"]), 4)

    def test_observed_warning_exit_zero_and_archive_is_a_failed_negative_control(self):
        # Exact behavior observed from Windows tools 6.2; the mock is not new Apple evidence.
        report = self.invoke(specialized=True, negative_exit=0, negative_diagnostic=MISSING_WARNING,
                             negative_product=True)
        self.assertEqual(report["status"], "failed")
        self.assertIn("compiler exit status 0", report["reason"])
        self.assertEqual(self.saved(), report)
        self.assertTrue(all(row["offline_mtl4_script_archive_generation_verified"] for row in report["cases"][:2]))
        self.assertTrue(report["distinct_specialization_archive_bytes_observed"])
        self.assertFalse(report["offline_mtl4_script_archive_generation_verified"])
        self.assertFalse(report["missing_required_constant_rejection_verified"])
        negative = report["cases"][2]
        self.assertEqual((negative["status"], negative["returncode"]), ("failed", 0))
        self.assertFalse(negative["compiler_rejection_verified"])
        self.assertEqual(negative["stderr_preview"], MISSING_WARNING)
        self.assertEqual((self.output / "generate-missing-required-constant-mtl4-archive.stderr.txt").read_text(), MISSING_WARNING)
        self.assertIn("missing-required-constant.mtl4archive", [Path(row["path"]).name for row in report["observed_outputs"]])
        self.assertTrue(all(report[name] is False for name in probe.FALSE_EVIDENCE))

    def test_missing_constant_requires_named_error_not_warning_or_unrelated_failure(self):
        for index, diagnostic in enumerate(("", "unrelated compiler failure", MISSING_WARNING,
                MISSING_ERROR.replace("index 0", "index 1"), MISSING_ERROR.replace("archive_value", "another_value"))):
            with self.subTest(diagnostic=diagnostic):
                self.output = self.root / ("diagnostic-" + str(index))
                report = self.invoke(specialized=True, negative_diagnostic=diagnostic)
                self.assertEqual(report["status"], "failed")
                self.assertFalse(report["missing_required_constant_rejection_verified"])
                self.assertFalse(report["cases"][2]["compiler_rejection_verified"])

    def test_timeout_budget_abnormal_or_missing_result_cannot_count_as_rejection(self):
        label = "generate-missing-required-constant-mtl4-archive"
        controls = (
            {"timeout": label}, {"budget": label}, {"negative_exit": None},
            {"negative_exit": -9}, {"negative_exit": 3221225477}, {"negative_exit": True},
            {"negative_event": {"process_started": False}}, {"negative_event": {"interrupted": True}},
            {"negative_event": {"error": "OSError: could not start"}},
            {"negative_event": {"termination": {"tree_termination_confirmed": False}}},
            {"negative_event": {"stderr_preview_truncated": True}},
        )
        for index, options in enumerate(controls):
            with self.subTest(options=options):
                self.output = self.root / ("abnormal-" + str(index))
                report = self.invoke(specialized=True, **options)
                self.assertEqual(report["status"], "budget_exhausted" if "budget" in options else "failed")
                self.assertFalse(report["missing_required_constant_rejection_verified"])
                self.assertFalse(report["cases"][2]["compiler_rejection_verified"])
                self.assertTrue(all(row["offline_mtl4_script_archive_generation_verified"] for row in report["cases"][:2]))

    def test_identical_specialization_archives_fail_before_the_negative_control(self):
        report = self.invoke(specialized=True, identical_specializations=True)
        self.assertEqual(report["status"], "failed")
        self.assertIn("identical archive bytes", report["reason"])
        self.assertFalse(report["distinct_specialization_archive_bytes_observed"])
        self.assertEqual(report["cases"][2]["status"], "not_run")
        self.assertFalse(report["offline_mtl4_script_archive_generation_verified"])
        self.assertFalse(any("missing-required-constant" in label for label, _ in self.calls))

    def test_second_specialization_failure_keeps_first_success_and_partial_evidence(self):
        report = self.invoke(specialized=True, fail="generate-value-11-mtl4-archive")
        self.assertEqual(report["status"], "failed")
        self.assertTrue(report["cases"][0]["offline_mtl4_script_archive_generation_verified"])
        self.assertEqual(report["cases"][1]["status"], "failed")
        self.assertEqual(report["cases"][2]["status"], "not_run")
        self.assertIn("value-11.mtl4archive", [Path(row["path"]).name for row in report["observed_outputs"]])

    def test_interrupted_negative_control_keeps_positives_without_a_rejection_claim(self):
        with self.assertRaises(KeyboardInterrupt):
            self.invoke(specialized=True, interrupt=("generate-missing-required-constant-mtl4-archive", KeyboardInterrupt()))
        report = self.saved()
        self.assertEqual(report["status"], "interrupted")
        self.assertTrue(all(row["offline_mtl4_script_archive_generation_verified"] for row in report["cases"][:2]))
        self.assertEqual(report["cases"][2]["status"], "interrupted")
        self.assertFalse(report["missing_required_constant_rejection_verified"])

    def test_main_returns_failure_for_unenforced_negative_control(self):
        for status, expected in ((probe.SPECIALIZED_SUCCESS, 0), ("failed", 1)):
            with self.subTest(status=status), patch.object(probe, "run", return_value={"status": status}), \
                    patch("builtins.print"):
                self.assertEqual(probe.main(["--tools", str(self.tools), "--output", str(self.output), "--specialized"]), expected)

    def test_render_failure_preserves_compute_success_and_raw_partial_product(self):
        report = self.invoke(fail="generate-render-mtl4-archive")
        self.assertEqual(report["status"], "failed")
        self.assertTrue(report["cases"][0]["offline_mtl4_script_archive_generation_verified"])
        self.assertEqual(report["cases"][1]["status"], "failed")
        self.assertFalse(report["offline_mtl4_script_archive_generation_verified"])
        self.assertEqual(self.calls[-1][0], "generate-render-mtl4-archive")
        self.assertEqual((self.output / "generate-render-mtl4-archive.stderr.txt").read_text(), "authored failure")
        self.assertIn("render.mtl4archive", [Path(item["path"]).name for item in report["observed_outputs"]])

    def test_timeout_stops_before_other_case_without_promoting_partial_archive(self):
        report = self.invoke(timeout="generate-compute-mtl4-archive")
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["cases"][0]["status"], "failed")
        self.assertEqual(report["cases"][1]["status"], "not_run")
        self.assertEqual(self.calls[-1][0], "generate-compute-mtl4-archive")
        self.assertFalse(report["offline_mtl4_script_archive_generation_verified"])

    def test_overall_budget_exhaustion_is_separate_and_passes_same_absolute_deadline(self):
        report = self.invoke(budget="generate-compute-mtl4-archive")
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertEqual(report["cases"][1]["status"], "not_run")
        self.assertEqual(len(set(self.deadlines)), 1)
        self.assertEqual(self.calls[-1][0], "generate-compute-mtl4-archive")

    def test_wrong_library_or_missing_product_is_not_successful_exit_evidence(self):
        for invalid_library, missing_archive in ((True, False), (False, True)):
            with self.subTest(invalid_library=invalid_library, missing_archive=missing_archive):
                self.output = self.root / ("invalid-library" if invalid_library else "missing-archive")
                self.calls.clear()
                report = self.invoke(invalid_library=invalid_library, missing_archive=missing_archive)
                self.assertEqual(report["status"], "failed")
                self.assertFalse(report["offline_mtl4_script_archive_generation_verified"])
                self.assertFalse(any(label == "inspect-compute-archive" for label, _ in self.calls))

    def test_wrong_multiple_or_truncated_archive_architectures_fail_closed(self):
        for index, (inspected, truncated) in enumerate((("applegpu_g15g", None),
                ("applegpu_g16p applegpu_g17p", None), ("applegpu_g16p", "inspect-compute-archive"))):
            with self.subTest(inspected=inspected, truncated=truncated):
                self.output = self.root / ("architecture-" + str(index))
                self.calls.clear()
                report = self.invoke(inspected=inspected, truncated=truncated)
                self.assertEqual(report["status"], "failed")
                self.assertFalse(report["cases"][0]["offline_mtl4_script_archive_generation_verified"])
                self.assertEqual(report["cases"][1]["status"], "not_run")

    def test_missing_selected_arch_is_unsupported_but_query_failure_is_failed(self):
        report = self.invoke(query="applegpu_g15g")
        self.assertEqual(report["status"], "unsupported_architecture")
        self.assertEqual(self.calls[-1][0], "query-ios26-apple9-architectures")
        self.output = self.root / "query-error"
        report = self.invoke(fail="query-ios26-apple9-architectures")
        self.assertEqual(report["status"], "failed")

    def test_unsupported_host_and_missing_tool_do_not_launch_a_command(self):
        report = self.invoke(system="Darwin")
        self.assertEqual(report["status"], "unsupported_host")
        self.assertEqual(self.calls, [])
        self.output = self.root / "missing-tools"
        (self.tools / "metal-nt.exe").unlink()
        report = self.invoke()
        self.assertEqual(report["status"], "unsupported_tools")
        self.assertEqual(report["missing_tools"], ["metal-nt"])
        self.assertEqual(self.calls, [])

    def test_keyboard_and_system_exit_preserve_interrupted_report_then_propagate(self):
        for index, error in enumerate((KeyboardInterrupt(), SystemExit(7))):
            with self.subTest(error=type(error).__name__):
                self.output = self.root / ("interrupted-" + str(index))
                self.calls.clear()
                with self.assertRaises(type(error)):
                    self.invoke(interrupt=("generate-compute-mtl4-archive", error))
                report = self.saved()
                self.assertEqual(report["status"], "interrupted")
                self.assertEqual(report["cases"][0]["status"], "interrupted")
                self.assertEqual(report["cases"][1]["status"], "not_run")
                self.assertFalse(report["offline_mtl4_script_archive_generation_verified"])
                self.assertFalse((self.output / "result.tmp").exists())

    def test_existing_directory_is_refused_without_changing_it(self):
        self.output.mkdir()
        marker = self.output / "result.json"
        marker.write_bytes(b"prior evidence")
        with self.assertRaises(FileExistsError):
            self.invoke()
        self.assertEqual(marker.read_bytes(), b"prior evidence")
        self.assertEqual(list(self.output.iterdir()), [marker])
        self.assertEqual(self.calls, [])

    def test_public_checkout_output_is_refused_before_writing(self):
        self.output = ROOT / "experiments/metal-compile/forbidden-archive-evidence"
        with self.assertRaises(probe.batch.BatchError):
            self.invoke()
        self.assertFalse(self.output.exists())
        self.assertEqual(self.calls, [])

    def test_finite_limits_are_checked_even_when_run_is_called_directly(self):
        for key, value in (("timeout", 66), ("timeout", 0), ("timeout", float("nan")),
                           ("budget", 181), ("budget", 0), ("budget", float("inf"))):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                probe.run(self.args(**{key: value}))
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
