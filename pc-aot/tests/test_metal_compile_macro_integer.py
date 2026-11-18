"""Portable authored controls; synthetic reports are not real Metal/GPU evidence."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("macro_integer_tested", ROOT / "experiments/metal-compile/macro_integer.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def native_report():
    rows = []
    for route in probe.CASES:
        number = route == "runtime_number"
        # Foundation may normalize unsigned int storage into a signed 64-bit NSNumber.
        value = {"status": "captured", "kind": "NSNumber", "original_declaration_recovered": False,
                 "objc_type": "q", "category": "signed_integer", "observed_width_bits": 64,
                 "bits_hex": "00000000ffffffff", "decimal": "4294967295"} if number else \
                {"status": "captured", "kind": "NSString", "text": probe.STRINGS[route]}
        values = {"languageVersion": {"raw": 0x30001, "known_name": None},
                  "mathMode": {"raw": 2, "known_name": "safe"},
                  "mathFloatingPointFunctions": {"raw": 1, "known_name": "precise"},
                  "preserveInvariance": False, "optimizationLevel": {"raw": 0, "is_default_enum": True},
                  "preprocessorMacros": {"status": "captured", "is_nil": False,
                                         "entries": [{"name": "U32_MAX", "value": value}]}}
        fields = {key: {"availability": "supported", "origin": "explicit_fixture_assignment", "value": value}
                  for key, value in values.items()}
        # These 64-bit results are fabricated validator inputs, not a prediction or Apple observation.
        observed = [0xffffffff, 4, 0] if route == "runtime_uint_string" else [0xffffffff, 8, 1]
        controls = [0xffffffff, 4, 0, 0xffffffff, 4, 0, 0x373f373f, 0x4d434933] + [0xcdcdcdcd] * 5
        rows.append({"case": route, "status": "readback_observed",
                     "effective_options": {"fields": fields, "explicit_fixture_setters": list(probe.SETTERS),
                                           "fresh_unmodified_object": False, "runtime_assignment_provenance_verified": False},
                     "integer_getter": {"method": "NSNumber.unsignedIntValue", "bits_hex": "ffffffff"} if number else
                                       {"status": "not_applicable_to_NSString"},
                     "value_library": {"created": True, "error": {"present": False}},
                     "gpu": {"pipeline_created": True, "pipeline_error": {"present": False},
                             "command_status": "completed", "command_error": {"present": False},
                             "input_words": [0xffffffff, 0x13579bdf], "initial_output_word": 0xcdcdcdcd,
                             "dispatch_threads": 1, "words_u32": observed + controls, "authored_controls_match": True}})
    return {"schema": 1, "scope": probe.SCOPE, "status": "authored_expressions_observed", "device_present": True,
            "authored_legacy_gpu_execution_verified": True, **{key: False for key in probe.FALSE_EVIDENCE},
            "number_constructor": dict(probe.DECLARATION), "requested_string_macros": dict(probe.STRINGS), "cases": rows}


class IntegerValidationTests(unittest.TestCase):
    def test_observations_do_not_require_unsigned_type_or_current_token_equivalence(self):
        report = native_report()
        result = probe.validate_native(report)
        self.assertEqual(result["observed_expressions"]["runtime_number"],
                         {"uint_value": 0xffffffff, "sizeof_bytes": 8, "minus_one_less_than_value": 1})
        self.assertEqual(result["matches_explicit_uint_expectation"],
                         {"runtime_number": False, "runtime_token_string": False, "runtime_uint_string": True})
        self.assertTrue(result["number_vs_token_string_expressions_equal"])
        report["cases"][0]["gpu"]["words_u32"][:3] = [0xffffffff, 4, 0]
        result = probe.validate_native(report)
        self.assertFalse(result["number_vs_token_string_expressions_equal"])
        self.assertTrue(result["matches_explicit_uint_expectation"]["runtime_number"])

    def test_explicit_uint_expectations_and_every_control_word_are_required(self):
        for route in range(3):
            for index in range(3, 16):
                report = native_report(); report["cases"][route]["gpu"]["words_u32"][index] ^= 1
                with self.subTest(route=route, index=index), self.assertRaisesRegex(ValueError, "control"):
                    probe.validate_native(report)
        for index in range(3):
            report = native_report(); report["cases"][2]["gpu"]["words_u32"][index] ^= 1
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "explicit uint"):
                probe.validate_native(report)

    def test_gpu_completion_shape_and_input_cannot_be_invented(self):
        mutations = [("pipeline_created", False), ("command_status", "failed"), ("command_error", {"present": True}),
                     ("authored_controls_match", False), ("dispatch_threads", True), ("input_words", [0, 0]),
                     ("initial_output_word", 0), ("words_u32", [True] * 16), ("words_u32", [0] * 15),
                     ("words_u32", [2**32] * 16)]
        for key, value in mutations:
            report = native_report(); report["cases"][0]["gpu"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError): probe.validate_native(report)

    def test_getters_scope_routes_and_original_constructor_are_independent(self):
        for key in probe.FALSE_EVIDENCE:
            report = native_report(); report[key] = True
            with self.subTest(key=key), self.assertRaises(ValueError): probe.validate_native(report)
        mutations = [lambda r: r.__setitem__("schema", True), lambda r: r["cases"].pop(),
                     lambda r: r["cases"][1].__setitem__("case", "runtime_number"),
                     lambda r: r["number_constructor"].__setitem__("factory", "numberWithLongLong"),
                     lambda r: r["cases"][0]["integer_getter"].__setitem__("bits_hex", "7fffffff"),
                     lambda r: r["cases"][0]["effective_options"]["fields"]["mathMode"]["value"].__setitem__("known_name", "fast"),
                     lambda r: r["cases"][0]["effective_options"]["fields"]["preprocessorMacros"]["value"]["entries"][0]["value"].__setitem__("decimal", "1")]
        for index, mutate in enumerate(mutations):
            report = native_report(); mutate(report)
            with self.subTest(index=index), self.assertRaises(ValueError): probe.validate_native(report)

    def test_warning_library_is_not_rejection_but_compile_rejection_never_passes(self):
        report = native_report()
        report["cases"][0]["value_library"]["error"] = {"present": True, "diagnostics": "authored warning"}
        probe.validate_native(report)
        for change in ("case_status", "nil_library", "global_failure"):
            invalid = copy.deepcopy(report)
            if change == "case_status": invalid["cases"][0]["status"] = "compile_rejected"
            elif change == "nil_library": invalid["cases"][0]["value_library"]["created"] = False
            else: invalid["status"] = "failed"
            with self.subTest(change=change), self.assertRaises(ValueError): probe.validate_native(invalid)


class IntegerRunnerTests(unittest.TestCase):
    def invoke(self, output, *, native=None, code=0, timed_out=False, sdk="26.5", fail_compile=False,
               duplicate_json=False, advance=None, interrupt=None):
        calls = []
        def command(argv, label, directory, report, env, timeout, *, deadline):
            calls.append((label, [str(item) for item in argv], timeout, deadline))
            self.assertFalse(any(name in env for name in probe.control.ENV_OPTIONS))
            event = {"label": label, "returncode": 0, "timed_out": False, "interrupted": False,
                     "stdout": label + ".stdout.txt", "stderr": label + ".stderr.txt"}
            stdout = sdk if label == "sdk-version" else str(output.parent.resolve()) if label == "sdk-path" else "synthetic only"
            if label == "native-compile":
                event["returncode"] = 1 if fail_compile else 0
                if not fail_compile: Path(argv[-1]).write_bytes(b"synthetic non-executed binary")
            if label == "native-observe":
                event.update(returncode=code, timed_out=timed_out)
                payload = json.dumps(native_report() if native is None else native)
                if duplicate_json: payload = payload[:-1] + ', "schema":1}'
                (directory / "native.json").write_text(payload, encoding="utf-8")
            (directory / event["stdout"]).write_text(stdout, encoding="utf-8")
            (directory / event["stderr"]).write_text("original synthetic compiler diagnostic", encoding="utf-8")
            report["commands"].append(event)
            if advance: advance(label)
            if interrupt and label == "native-observe": raise interrupt
            return event, stdout, "original synthetic compiler diagnostic"
        with patch.object(probe.platform, "system", return_value="Darwin"), \
                patch.object(probe.control, "run_command", side_effect=command):
            report = probe.run(probe.parse_args(["--output", str(output), "--budget", "90"]))
        self.assertEqual(len({call[3] for call in calls}), 1)
        return report, calls

    def test_runner_owns_fresh_sources_driver_spelling_and_failure_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "new"
            with patch.dict(probe.os.environ, {"SDKROOT": "bad-sdk", "CPATH": "bad-includes"}):
                report, calls = self.invoke(output)
            self.assertEqual(report["status"], "authored_expressions_observed")
            command = next(call[1] for call in calls if call[0] == "native-compile")
            self.assertEqual(command[:4], ["/usr/bin/xcrun", "--sdk", "macosx", "clang++"])
            self.assertIn("-mmacosx-version-min=15.0", command)
            self.assertEqual(command[command.index("-isysroot") + 1], str(output.parent.resolve()))
            self.assertEqual((output / "authored-macro-integer.metal").read_text(), probe.GPU_SOURCE)
            self.assertEqual(json.loads((output / "result.json").read_text()), report)
            self.assertTrue(all(report[name] is False for name in probe.FALSE_EVIDENCE))
            original = (output / "result.json").read_bytes()
            with self.assertRaises(FileExistsError): self.invoke(output)
            self.assertEqual((output / "result.json").read_bytes(), original)

    def test_compile_rejection_preserves_remaining_case_observations_and_raw_error(self):
        native = native_report()
        native.update(status="failed", authored_legacy_gpu_execution_verified=False)
        row = native["cases"][0]
        row.update(status="compile_rejected", value_library={"created": False,
                   "error": {"present": True, "domain": "authored-test", "code": 3, "diagnostics": "original rejection"}})
        row.pop("gpu")
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "new"
            report, _ = self.invoke(output, native=native, code=1)
            self.assertEqual(report["status"], "failed")
            self.assertIs(report["authored_legacy_gpu_execution_verified"], False)
            self.assertEqual(report["native"], native)
            self.assertEqual(report["native"]["cases"][2]["status"], "readback_observed")
            self.assertNotIn("number_vs_token_string_expressions_equal", report)
            self.assertEqual((output / "native-observe.stderr.txt").read_text(), "original synthetic compiler diagnostic")

    def test_timeout_nonzero_compile_and_duplicate_json_never_pass(self):
        for kwargs in ({"timed_out": True}, {"code": 1}, {"fail_compile": True}, {"duplicate_json": True}):
            with self.subTest(kwargs=kwargs), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "new"
                report, _ = self.invoke(output, **kwargs)
                self.assertEqual(report["status"], "failed")
                self.assertIs(report["authored_legacy_gpu_execution_verified"], False)
                self.assertTrue(list(output.glob("*.stderr.txt")))
                self.assertEqual(json.loads((output / "result.json").read_text()), report)

    def test_non_mac_old_sdk_and_no_device_are_explicitly_unsupported(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(probe.platform, "system", return_value="Windows"), \
                patch.object(probe.control, "run_command") as launch:
            report = probe.run(probe.parse_args(["--output", str(Path(folder) / "host")]))
            self.assertEqual(report["status"], "unsupported_host"); launch.assert_not_called()
        with tempfile.TemporaryDirectory() as folder:
            report, calls = self.invoke(Path(folder) / "sdk", sdk="14.5")
            self.assertEqual(report["status"], "unsupported_sdk"); self.assertEqual(len(calls), 1)
        native = {"schema": 1, "scope": probe.SCOPE, "status": "unsupported", "reason": "authored no-device control",
                  "cases": [], "authored_legacy_gpu_execution_verified": False, **{key: False for key in probe.FALSE_EVIDENCE}}
        with tempfile.TemporaryDirectory() as folder:
            report, _ = self.invoke(Path(folder) / "device", native=native, code=78)
            self.assertEqual(report["status"], "unsupported_native")
        native["runtime_macro_token_capture_verified"] = True
        with tempfile.TemporaryDirectory() as folder:
            report, _ = self.invoke(Path(folder) / "bad", native=native, code=78)
            self.assertEqual(report["status"], "failed")

    def test_shared_deadline_reduces_cap_and_stops_later_admission(self):
        now = [100.0]
        def advance(label): now[0] += 65 if label == "sdk-version" else 1
        with tempfile.TemporaryDirectory() as folder, patch.object(probe.time, "monotonic", side_effect=lambda: now[0]):
            report, calls = self.invoke(Path(folder) / "caps", advance=advance)
        self.assertEqual(report["status"], "authored_expressions_observed")
        self.assertEqual([call[2] for call in calls], [30, 25, 24, 23, 22])
        self.assertTrue(all(call[3] == 190 for call in calls))
        now[0] = 100
        with tempfile.TemporaryDirectory() as folder, patch.object(probe.time, "monotonic", side_effect=lambda: now[0]):
            report, calls = self.invoke(Path(folder) / "expired", advance=lambda _: now.__setitem__(0, 190))
        self.assertEqual(report["status"], "failed")
        self.assertIn("BudgetExhausted", report["reason"])
        self.assertEqual(len(calls), 1)

    def test_interruption_keeps_native_partial_report_and_original_exception(self):
        partial = {"status": "running", "cases": [{"case": "runtime_number", "status": "gpu_pending"}]}
        for error in (KeyboardInterrupt(), SystemExit(47)):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "new"
                with self.assertRaises(type(error)) as raised:
                    self.invoke(output, native=partial, interrupt=error)
                self.assertIs(raised.exception, error)
                report = json.loads((output / "result.json").read_text())
                self.assertEqual(report["status"], "interrupted")
                self.assertEqual(report["native"], partial)
                self.assertIs(report["authored_legacy_gpu_execution_verified"], False)

    def test_invalid_budgets_and_atomic_report_replacement(self):
        for option, value in (("--budget", "0"), ("--budget", "nan"), ("--budget", "181"), ("--timeout", "61"), ("--timeout", "inf")):
            with self.subTest(option=option, value=value), patch("sys.stderr"), self.assertRaises(SystemExit):
                probe.parse_args(["--output", "unused", option, value])
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder); probe.write_report(output, {"status": "old"})
            write = Path.write_text
            def interrupt(path, text, **kwargs):
                write(path, text[:5], **kwargs)
                raise KeyboardInterrupt
            with patch.object(Path, "write_text", interrupt), self.assertRaises(KeyboardInterrupt):
                probe.write_report(output, {"status": "new"})
            self.assertEqual(json.loads((output / "result.json").read_text()), {"status": "old"})


if __name__ == "__main__":
    unittest.main()
