"""Portable controls only. Authored JSON/tool stubs are not Apple/GPU evidence."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_archive_test", ROOT / "experiments/metal-compile/archive.py")
archive = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(archive)


def success(field):
    return {field: True, "error_present": False, "error": {}}


def diagnostic():
    return {"domain": "SyntheticControlError", "code": 3, "description": "authored test control"}


def native_report(mode, pid=None):
    """Synthetic parser input, never returned by an actual Metal process."""
    recorded = [3, 7, 10, 305419896, 4, 14, 10, 305419896,
                5, 21, 10, 305419896, 6, 28, 10, 305419896]
    unrecorded = [11, 20, 29, 2271560481, 16, 19, 28, 2271560481,
                  21, 18, 27, 2271560481, 26, 17, 26, 2271560481]
    result = {"schema": 1, "scope": archive.SCOPE, "mode": mode, "status": "passed",
              "process_id": pid or (101 if mode == "harvest" else 102), "os_version": [13, 0, 0],
              "device": {"available": True, "name": "SYNTHETIC NO GPU"},
              "source_library_compiles": 0, "url_library_loads": 1, "gpu_execution_verified": True,
              "library_load": success("created"),
              "archive_factory": {"kind": "fresh" if mode == "harvest" else "saved", **success("created")},
              "harvested_kernels": ["archive_recorded"] if mode == "harvest" else [],
              "archive_rejections": [], "pipelines": [], **{key: False for key in archive.FALSE_EVIDENCE}}
    hit = {"case": mode + "_recorded_strict", "kernel": "archive_recorded", "strict": True,
           "archive_attached": True, **success("created"), "command_status": "completed",
           "command_error": {}, "compute_words": recorded}
    result["pipelines"].append(hit)
    if mode == "harvest":
        result["archive_add"] = success("succeeded")
        result["archive_serialize"] = success("succeeded")
    else:
        result["pipelines"] += [
            {"case": "restore_unrecorded_strict", "kernel": "archive_unrecorded", "strict": True,
             "archive_attached": True, "created": False, "error_present": True, "error": diagnostic()},
            {"case": "restore_unrecorded_fallback", "kernel": "archive_unrecorded", "strict": False,
             "archive_attached": False, **success("created"), "command_status": "completed",
             "command_error": {}, "compute_words": unrecorded},
        ]
        result["archive_rejections"] = [{"case": case, "returned_nil": True, "error_present": True,
                                          "error": diagnostic()} for case in ("empty_file", "malformed_file")]
    return result


def unsupported_report(mode):
    result = native_report(mode)
    result.update(status="unsupported", unsupported_reason="no_default_metal_device",
                  device={"available": False, "name": ""}, url_library_loads=0,
                  gpu_execution_verified=False, harvested_kernels=[], pipelines=[], archive_rejections=[])
    for key in ("library_load", "archive_factory", "archive_add", "archive_serialize"):
        result.pop(key, None)
    return result


class ArchiveReportTests(unittest.TestCase):
    def test_valid_synthetic_harvest_restore_and_no_gpu_reports(self):
        for mode in ("harvest", "restore"):
            archive.validate_native(native_report(mode), mode)
            archive.validate_native(unsupported_report(mode), mode)

    def test_every_readback_word_matters(self):
        for row_index in (0, 2):
            for index in range(16):
                with self.subTest(row=row_index, index=index):
                    report = native_report("restore")
                    report["pipelines"][row_index]["compute_words"][index] ^= 1
                    with self.assertRaisesRegex(ValueError, "readback"):
                        archive.validate_native(report, "restore")

    def test_strict_hit_miss_and_explicit_fallback_cannot_be_substituted(self):
        mutations = (
            lambda r: r["pipelines"][0].__setitem__("strict", False),
            lambda r: r["pipelines"][0].__setitem__("archive_attached", False),
            lambda r: r["pipelines"][1].__setitem__("created", True),
            lambda r: r["pipelines"][1].__setitem__("error_present", False),
            lambda r: r["pipelines"][1].__setitem__("error", {}),
            lambda r: r["pipelines"][2].__setitem__("archive_attached", True),
            lambda r: r["pipelines"][2].__setitem__("kernel", "archive_recorded"),
            lambda r: r["pipelines"][2].__setitem__("command_status", "failed"),
            lambda r: r["pipelines"].reverse(),
            lambda r: r["pipelines"].pop(),
        )
        for mutate in mutations:
            report = native_report("restore")
            mutate(report)
            with self.assertRaises(ValueError):
                archive.validate_native(report, "restore")

    def test_invalid_archive_and_save_errors_remain_failures(self):
        for case in range(2):
            for key, value in (("returned_nil", False), ("error_present", False), ("error", {})):
                report = native_report("restore")
                report["archive_rejections"][case][key] = value
                with self.assertRaises(ValueError):
                    archive.validate_native(report, "restore")
        for operation in ("archive_add", "archive_serialize"):
            report = native_report("harvest")
            report[operation]["succeeded"] = False
            with self.assertRaises(ValueError):
                archive.validate_native(report, "harvest")

    def test_evidence_boundaries_and_numeric_shapes_are_strict(self):
        for key in archive.FALSE_EVIDENCE:
            report = native_report("restore")
            report[key] = True
            with self.assertRaisesRegex(ValueError, "evidence"):
                archive.validate_native(report, "restore")
        for key, value in (("source_library_compiles", 1), ("source_library_compiles", False),
                           ("url_library_loads", True), ("os_version", None), ("device", []),
                           ("harvested_kernels", ["archive_unrecorded"])):
            report = native_report("restore")
            report[key] = value
            with self.assertRaises(ValueError):
                archive.validate_native(report, "restore")
        report = unsupported_report("harvest")
        report["gpu_execution_verified"] = True
        with self.assertRaises(ValueError):
            archive.validate_native(report, "harvest")


class ArchiveRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.output = self.root / "evidence"
        self.sdk = self.root / "SDK"
        self.sdk.mkdir()
        self.tools = {}
        for name in ("metal", "metallib", "clang++", "xcrun"):
            path = self.root / name
            path.write_bytes(b"synthetic non-executable tool")
            self.tools[name] = path
        self.calls = []
        self.mutate = None
        self.fail_label = None
        self.returncode = 1
        self.interrupt_label = None
        self.native_mutation = None

    def args(self):
        return archive.parse_args(["--output", str(self.output)])

    def fake_command(self, command, label, output, report, env, timeout, deadline=None):
        argv = [str(part) for part in command]
        self.calls.append((label, argv, dict(env), timeout, deadline))
        event = {"label": label, "argv": argv, "returncode": 0, "timed_out": False,
                 "budget_exhausted": False, "stderr": label + ".stderr.txt", "stdout": label + ".stdout.txt"}
        stdout = "synthetic tool version"
        if label == "resolve-clangxx":
            stdout = str(self.tools["clang++"])
        if label == "sdk-version":
            stdout = "26.5"
        if label in ("compile-metal", "link-metallib", "compile-native"):
            Path(argv[argv.index("-o") + 1]).write_bytes(b"MTLBsynthetic" if label == "link-metallib" else b"synthetic artifact")
        if label.startswith("native-"):
            mode = label.removeprefix("native-")
            native = native_report(mode)
            if self.native_mutation:
                self.native_mutation(mode, native, event)
            if mode == "harvest" and event["returncode"] == 0:
                (output / "device.binary.metallib").write_bytes(b"synthetic archive; not Apple data")
            if self.mutate and mode == "restore":
                self.mutate(output)
            Path(argv[argv.index("--report") + 1]).write_text(json.dumps(native), encoding="utf-8")
        if label == self.fail_label:
            event["returncode"] = self.returncode
            event["timed_out"] = self.returncode is None
        (output / event["stdout"]).write_text(stdout, encoding="utf-8")
        (output / event["stderr"]).write_text("synthetic retained diagnostic" if event["returncode"] != 0 else "", encoding="utf-8")
        report["commands"].append(event)
        if label == self.interrupt_label:
            event["interrupted"] = True
            raise KeyboardInterrupt()
        return event, stdout, ""

    def patched_run(self, command=None):
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(archive.platform, "system", return_value="Darwin"))
            stack.enter_context(patch.object(archive.shutil, "which", return_value=str(self.tools["xcrun"])))
            stack.enter_context(patch.object(archive.offline, "resolve_tools", return_value=({key: self.tools[key] for key in ("metal", "metallib")}, self.sdk)))
            stack.enter_context(patch.object(archive.offline, "run_command", side_effect=command or self.fake_command))
            stack.enter_context(patch.dict(os.environ, {"SDKROOT": "ios-sdk", "IPHONEOS_DEPLOYMENT_TARGET": "26.0", "CPATH": "wrong-headers"}))
            return archive.run(self.args())

    def test_one_binary_two_process_modes_explicit_sdk_and_fixed_deadline(self):
        report, code = self.patched_run()
        self.assertEqual((report["status"], code), ("passed", 0))
        self.assertTrue(report["legacy_archive_save_reload_verified"])
        self.assertTrue(all(report[key] is False for key in archive.FALSE_EVIDENCE))
        native = [call for call in self.calls if call[0].startswith("native-")]
        self.assertEqual([call[0] for call in native], ["native-harvest", "native-restore"])
        self.assertEqual(native[0][1][0], native[1][1][0])
        self.assertEqual(len({call[4] for call in self.calls}), 1)
        for _, _, env, timeout, _ in self.calls:
            self.assertEqual(timeout, 60)
            for key in ("SDKROOT", "IPHONEOS_DEPLOYMENT_TARGET", "CPATH"):
                self.assertNotIn(key, env)
        for label in ("compile-metal", "compile-native"):
            command = next(call[1] for call in self.calls if call[0] == label)
            self.assertEqual(command[command.index("-isysroot") + 1], str(self.sdk))
        self.assertEqual((self.output / "empty.binary.metallib").stat().st_size, 0)
        self.assertEqual(json.loads((self.output / "result.json").read_text())["status"], "passed")

    def test_native_no_gpu_preserves_unsupported_report_and_false_flags(self):
        def unsupported(mode, native, event):
            native.clear()
            native.update(unsupported_report(mode))
            event["returncode"] = 78
        self.native_mutation = unsupported
        report, code = self.patched_run()
        self.assertEqual((report["status"], code), ("unsupported", 78))
        self.assertEqual(len(report["native_reports"]), 1)
        self.assertFalse(report["gpu_execution_verified"])
        self.assertFalse(report["legacy_archive_save_reload_verified"])
        self.assertNotIn("native-restore", [call[0] for call in self.calls])

    def test_clangxx_symlink_keeps_cxx_driver_invocation(self):
        target = self.root / "clang"
        target.write_bytes(b"synthetic non-executable C-family driver")
        self.tools["clang++"].unlink()
        try:
            self.tools["clang++"].symlink_to(target)
        except (OSError, NotImplementedError):
            self.skipTest("host does not permit symbolic links")
        report, code = self.patched_run()
        self.assertEqual((report["status"], code), ("passed", 0))
        command = next(call[1] for call in self.calls if call[0] == "compile-native")
        self.assertEqual(command[0], str(self.tools["clang++"]))
        self.assertNotEqual(command[0], str(target))

    def test_compile_failure_and_timeout_stop_later_commands_and_keep_logs(self):
        for returncode in (1, None):
            with self.subTest(returncode=returncode):
                self.output = self.root / ("failed" + str(returncode))
                self.calls = []
                self.fail_label, self.returncode = "compile-metal", returncode
                report, code = self.patched_run()
                self.assertEqual((report["status"], code), ("failed", 1))
                self.assertEqual(self.calls[-1][0], "compile-metal")
                self.assertIn("synthetic retained", (self.output / "compile-metal.stderr.txt").read_text())
                self.assertFalse(report["gpu_execution_verified"])

    def test_metallib_version_probe_failure_is_retained_but_timeout_stops(self):
        self.fail_label = "version-metallib"
        report, code = self.patched_run()
        self.assertEqual((report["status"], code), ("passed", 0))
        self.assertEqual(report["versions"]["metallib"]["returncode"], 1)
        self.assertFalse(report["versions"]["metallib"]["available"])
        self.assertIn("synthetic retained", report["versions"]["metallib"]["text"])
        self.output = self.root / "version-timeout"
        self.returncode = None
        self.calls = []
        report, code = self.patched_run()
        self.assertEqual((report["status"], code), ("failed", 1))
        self.assertEqual(self.calls[-1][0], "version-metallib")

    def test_false_native_pass_or_reused_process_id_is_rejected(self):
        for wrong in ("readback", "pid"):
            self.output = self.root / wrong
            def mutation(mode, native, event):
                if mode == "restore":
                    if wrong == "pid":
                        native["process_id"] = 101
                    else:
                        native["pipelines"][2]["compute_words"][15] = 0
            self.native_mutation = mutation
            report, code = self.patched_run()
            self.assertEqual((report["status"], code), ("failed", 1))
            self.assertFalse(report["legacy_archive_save_reload_verified"])

    def test_restore_cannot_change_the_saved_archive(self):
        self.mutate = lambda output: (output / "device.binary.metallib").write_bytes(b"changed archive")
        report, code = self.patched_run()
        self.assertEqual(code, 1)
        self.assertIn("modified", report["error"])

    def test_native_error_report_is_retained(self):
        def failure(mode, native, event):
            native.update(status="failed", failure="synthetic archive serialization error", gpu_execution_verified=False)
            event["returncode"] = 1
        self.native_mutation = failure
        report, code = self.patched_run()
        self.assertEqual(code, 1)
        self.assertIn("serialization error", report["native_reports"][0]["failure"])
        self.assertFalse(report["legacy_archive_save_reload_verified"])

    def test_interrupt_is_reported_and_propagated(self):
        self.interrupt_label = "compile-native"
        with self.assertRaises(KeyboardInterrupt):
            self.patched_run()
        self.assertEqual(json.loads((self.output / "result.json").read_text())["status"], "interrupted")

    def test_expired_budget_uses_real_helper_refusal_before_popen(self):
        # Stub resolution only. The real shared helper enforces the fake clock at admission.
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(archive.platform, "system", return_value="Darwin"))
            stack.enter_context(patch.object(archive.shutil, "which", return_value=str(self.tools["xcrun"])))
            stack.enter_context(patch.object(archive.offline, "resolve_tools", return_value=({key: self.tools[key] for key in ("metal", "metallib")}, self.sdk)))
            stack.enter_context(patch.object(archive.time, "monotonic", side_effect=[0.0, 181.0, 181.0]))
            popen = stack.enter_context(patch.object(archive.offline.subprocess, "Popen"))
            report, code = archive.run(self.args())
        popen.assert_not_called()
        self.assertEqual((report["status"], code), ("budget_exhausted", 1))
        self.assertTrue(report["commands"][0]["budget_exhausted"])
        self.assertFalse(report["commands"][0]["process_started"])

    def test_fresh_output_and_non_mac_refusal(self):
        self.output.mkdir()
        marker = self.output / "retained.txt"
        marker.write_text("retain", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            archive.run(self.args())
        self.assertEqual(marker.read_text(), "retain")
        self.output = self.root / "nonmac"
        with patch.object(archive.platform, "system", return_value="Windows"), patch.object(archive.offline, "run_command") as child:
            report, code = archive.run(self.args())
        child.assert_not_called()
        self.assertEqual((report["status"], code), ("unsupported", 78))

    def test_budget_bounds_and_native_source_contract(self):
        for value in ("0", "-1", "nan", "inf", "181"):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                archive.parse_args(["--output", str(self.output), "--budget", value])
        source = (archive.HERE / "archive_runtime.mm").read_text()
        self.assertIn("newLibraryWithURL:", source)
        self.assertNotIn("newLibraryWithSource:", source)
        self.assertNotIn("MTL4", source)
        self.assertNotIn("supportsFamily", source)
        self.assertIn("MTLPipelineOptionFailOnBinaryArchiveMiss", source)
        self.assertIn("missing.binaryArchives = @[]", source)
        self.assertEqual(source.count("addComputePipelineFunctionsWithDescriptor:"), 1)
        self.assertNotIn("[[function_constant", (archive.HERE / "archive_fixture.metal").read_text())


if __name__ == "__main__":
    unittest.main()
