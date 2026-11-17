"""Synthetic report/runner controls only, not Apple compilation or GPU evidence."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("macro_value_tested", ROOT / "experiments/metal-compile/macro_value.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def diagnostics(tokens):
    lines = []
    for name, line in probe.LINES.items():
        lines.append(f'program_source:{line}:1: error: static_assert failed "TOKEN_BEGIN_{name}__{tokens[name]}__TOKEN_END"')
        lines.append(probe.DIAGNOSTIC_SOURCE.splitlines()[line - 1])
    return "\n".join(lines)


def number_value(name):
    bits = probe.INPUT_BITS[name]
    value = struct.unpack(">f", bytes.fromhex(bits))[0]
    # A getter may report double even though the authored constructor took float.
    return {"status": "captured", "kind": "NSNumber", "original_declaration_recovered": False,
            "objc_type": "d", "category": "binary_float", "observed_width_bits": 64,
            "bits_hex": struct.pack(">d", value).hex(), "negative_zero": name == "F32_NEG_ZERO", "classification": "finite"}


def native_report():
    rows = []
    for route in ("runtime_number", "runtime_string"):
        numeric = route == "runtime_number"
        entries = [{"name": name, "value": number_value(name) if numeric else
                    {"status": "captured", "kind": "NSString", "text": probe.MACROS[name]}} for name in sorted(probe.MACROS)]
        values = {"languageVersion": {"raw": 0x30001, "known_name": None},
                  "mathMode": {"raw": 2, "known_name": "safe"},
                  "mathFloatingPointFunctions": {"raw": 1, "known_name": "precise"},
                  "preserveInvariance": False, "optimizationLevel": {"raw": 0, "is_default_enum": True},
                  "preprocessorMacros": {"status": "captured", "is_nil": False, "entries": entries}}
        fields = {key: {"availability": "supported", "origin": "explicit_fixture_assignment", "value": value} for key, value in values.items()}
        tokens = {"F32_NEXT": "1", "F32_NEG_ZERO": "-0"} if numeric else dict(probe.MACROS)
        words = ([0x3f800000, 0] if numeric else [0x3f800001, 0x80000000]) + list(probe.CONTROL_WORDS)
        rows.append({"case": route, "status": "readback_observed", "requested_options_getters_match": True,
                     "effective_options": {"fields": fields, "explicit_fixture_setters": list(probe.diagnostic.SETTERS),
                                           "fresh_unmodified_object": False, "runtime_assignment_provenance_verified": False},
                     "float_getters": {"status": "captured", "method": "NSNumber.floatValue", "bits_hex": dict(probe.INPUT_BITS)}
                        if numeric else {"status": "not_applicable_to_NSString"},
                     "token_diagnostic": {"library_created": False, "error": {"present": True, "diagnostics_truncated": False,
                                                                                "diagnostics": diagnostics(tokens)}},
                     "value_library": {"created": True, "error": {"present": False}},
                     "gpu": {"pipeline_created": True, "pipeline_error": {"present": False}, "command_status": "completed",
                             "command_error": {"present": False}, "input_word": 0x13579bdf, "initial_output_word": 0xcdcdcdcd,
                             "dispatch_threads": 1, "words_u32": words, "authored_controls_match": True,
                             "macro_bits_hex": {"F32_NEXT": f"{words[0]:08x}", "F32_NEG_ZERO": f"{words[1]:08x}"}}})
    return {"schema": 1, "scope": probe.SCOPE, "status": "authored_values_observed", "device_present": True,
            "authored_legacy_gpu_execution_verified": True, **{key: False for key in probe.FALSE_EVIDENCE},
            "authored_float32_input_bits": dict(probe.INPUT_BITS), "requested_string_macros": dict(probe.MACROS),
            "number_constructor": probe.CONSTRUCTOR, "cases": rows}


class ValueValidationTests(unittest.TestCase):
    def test_differences_are_observations_not_a_failed_control_or_production_fix(self):
        report = native_report()
        result = probe.validate_native(report)
        self.assertEqual(result["observed_tokens"]["runtime_number"], {"F32_NEXT": "1", "F32_NEG_ZERO": "-0"})
        self.assertEqual(result["observed_gpu_bits"]["runtime_number"], {"F32_NEXT": "3f800000", "F32_NEG_ZERO": "00000000"})
        self.assertEqual(result["number_vs_string_gpu_bits_equal"], {name: False for name in probe.MACROS})
        self.assertEqual(result["matches_authored_float_input"]["runtime_string"], {name: True for name in probe.MACROS})
        self.assertTrue(all(report[name] is False for name in probe.FALSE_EVIDENCE))

    def test_equal_gpu_results_are_allowed_without_assuming_tokens_or_types_match(self):
        report = native_report()
        report["cases"][0]["gpu"] = copy.deepcopy(report["cases"][1]["gpu"])
        result = probe.validate_native(report)
        self.assertTrue(all(result["number_vs_string_gpu_bits_equal"].values()))
        self.assertFalse(any(result["number_vs_string_token_equal"].values()))

    def test_getter_bits_are_not_inferred_from_the_token_or_gpu_result(self):
        for change in ("float_getter", "double_bits", "negative_zero"):
            report = native_report()
            row = report["cases"][0]
            if change == "float_getter": row["float_getters"]["bits_hex"]["F32_NEXT"] = "3f800000"
            else:
                entries = row["effective_options"]["fields"]["preprocessorMacros"]["value"]["entries"]
                value = next(entry["value"] for entry in entries if entry["name"] == ("F32_NEXT" if change == "double_bits" else "F32_NEG_ZERO"))
                if change == "double_bits": value["bits_hex"] = "3ff0000000000000"
                else: value["negative_zero"] = False
            with self.subTest(change=change), self.assertRaises(ValueError): probe.validate_native(report)

    def test_completion_literal_input_guard_and_bit_crosschecks_cannot_be_skipped(self):
        changes = [("pipeline_created", False), ("command_status", "error"), ("command_error", {"present": True}),
                   ("authored_controls_match", False), ("dispatch_threads", True), ("input_word", 0)]
        for key, value in changes:
            report = native_report(); report["cases"][0]["gpu"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): probe.validate_native(report)
        for index in range(2, 8):
            report = native_report(); report["cases"][0]["gpu"]["words_u32"][index] ^= 1
            with self.subTest(index=index), self.assertRaises(ValueError): probe.validate_native(report)
        for words in ([0] * 7, [True] * 8, [2**32] * 8):
            report = native_report(); report["cases"][0]["gpu"]["words_u32"] = words
            with self.subTest(words=words), self.assertRaises(ValueError): probe.validate_native(report)
        report = native_report(); report["cases"][0]["gpu"]["macro_bits_hex"]["F32_NEXT"] = "3f800001"
        with self.assertRaises(ValueError): probe.validate_native(report)

    def test_diagnostic_absence_echo_only_duplicate_and_truncation_do_not_prove_tokens(self):
        for change in ("echo", "duplicate", "truncated", "missing_rejection"):
            report = native_report(); rejection = report["cases"][0]["token_diagnostic"]
            if change == "echo": rejection["error"]["diagnostics"] = probe.DIAGNOSTIC_SOURCE
            elif change == "duplicate": rejection["error"]["diagnostics"] *= 2
            elif change == "truncated": rejection["error"]["diagnostics_truncated"] = True
            else: rejection["library_created"] = True
            with self.subTest(change=change), self.assertRaises(ValueError): probe.validate_native(report)

    def test_routes_profile_inputs_and_scope_are_exact(self):
        for key in probe.FALSE_EVIDENCE:
            report = native_report(); report[key] = True
            with self.subTest(key=key), self.assertRaises(ValueError): probe.validate_native(report)
        for change in ("duplicate_route", "missing_route", "math", "unsupported_field", "declaration", "schema", "no_library"):
            report = native_report()
            if change == "duplicate_route": report["cases"][1]["case"] = "runtime_number"
            elif change == "missing_route": report["cases"].pop()
            elif change == "math": report["cases"][0]["effective_options"]["fields"]["mathMode"]["value"]["known_name"] = "fast"
            elif change == "unsupported_field": report["cases"][0]["effective_options"]["fields"]["mathMode"]["availability"] = "unsupported_os"
            elif change == "declaration": report["authored_float32_input_bits"]["F32_NEXT"] = "3f800000"
            elif change == "schema": report["schema"] = True
            else: report["cases"][0]["value_library"]["created"] = False
            with self.subTest(change=change), self.assertRaises(ValueError): probe.validate_native(report)


class ValueRunnerTests(unittest.TestCase):
    def invoke(self, output, *, native=None, code=0, timed_out=False, sdk="26.5", fail_compile=False, duplicate_json=False):
        commands, deadlines = [], []
        def command(argv, label, directory, report, env, timeout, *, deadline):
            commands.append([str(item) for item in argv]); deadlines.append(deadline)
            self.assertFalse(any(name in env for name in probe.control.ENV_OPTIONS))
            event = {"label": label, "returncode": 0, "timed_out": False, "interrupted": False,
                     "stdout": label + ".stdout.txt", "stderr": label + ".stderr.txt"}
            stdout = sdk if label == "sdk-version" else str(output.parent.resolve()) if label == "sdk-path" else "synthetic control only"
            if label == "native-compile":
                event["returncode"] = 1 if fail_compile else 0
                if not fail_compile: Path(argv[-1]).write_bytes(b"synthetic non-executed binary")
            if label == "native-observe":
                event.update(returncode=code, timed_out=timed_out)
                payload = json.dumps(native_report() if native is None else native)
                if duplicate_json: payload = payload[:-1] + ', "schema":1}'
                (directory / "native.json").write_text(payload)
            (directory / event["stdout"]).write_text(stdout)
            (directory / event["stderr"]).write_text("synthetic stderr retained")
            report["commands"].append(event)
            return event, stdout, "synthetic stderr retained"
        with patch.object(probe.platform, "system", return_value="Darwin"), \
                patch.object(probe.control, "run_command", side_effect=command):
            report = probe.run(probe.parse_args(["--output", str(output), "--budget", "90"]))
        self.assertEqual(len(set(deadlines)), 1)
        return report, commands

    def test_runner_preserves_clangxx_driver_deadline_sources_and_observation_scope(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "new"
            with patch.dict(probe.os.environ, {"CPATH": "authored-invalid-include-control", "SDKROOT": "authored-invalid-sdk-control"}):
                report, commands = self.invoke(output)
            self.assertEqual(report["status"], "authored_values_observed")
            compile_command = next(command for command in commands if "-fobjc-arc" in command)
            self.assertEqual(compile_command[:4], ["/usr/bin/xcrun", "--sdk", "macosx", "clang++"])
            self.assertTrue(Path(compile_command[-3]).is_absolute())
            self.assertEqual((output / "authored-macro-value.metal").read_text(), probe.GPU_SOURCE)
            self.assertEqual(json.loads((output / "result.json").read_text()), report)
            self.assertTrue(all(report[name] is False for name in probe.FALSE_EVIDENCE))
            original = (output / "result.json").read_bytes()
            with self.assertRaises(FileExistsError): self.invoke(output)
            self.assertEqual((output / "result.json").read_bytes(), original)

    def test_non_mac_host_is_explicitly_unsupported_and_launches_nothing(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "new"
            with patch.object(probe.platform, "system", return_value="Windows"), patch.object(probe.control, "run_command") as launch:
                report = probe.run(probe.parse_args(["--output", str(output)]))
            launch.assert_not_called()
            self.assertEqual(report["status"], "unsupported_host")
            self.assertIs(report["authored_legacy_gpu_execution_verified"], False)
            self.assertTrue((output / "macro_value.mm").is_file())

    def test_unsupported_sdk_and_device_remain_unsupported_without_gpu_claim(self):
        with tempfile.TemporaryDirectory() as folder:
            report, commands = self.invoke(Path(folder) / "old", sdk="14.5")
            self.assertEqual(report["status"], "unsupported_sdk")
            self.assertEqual(len(commands), 1)
        native = {"schema": 1, "scope": probe.SCOPE, "status": "unsupported", "reason": "authored no-device control",
                  "cases": [], "authored_legacy_gpu_execution_verified": False, **{key: False for key in probe.FALSE_EVIDENCE}}
        with tempfile.TemporaryDirectory() as folder:
            report, _ = self.invoke(Path(folder) / "absent", native=native, code=78)
            self.assertEqual(report["status"], "unsupported_native")
            self.assertIs(report["authored_legacy_gpu_execution_verified"], False)
        for key, value in (("production_parity_verified", True), ("cases", [{}]), ("schema", True), ("reason", "")):
            invalid = copy.deepcopy(native); invalid[key] = value
            with self.subTest(key=key), tempfile.TemporaryDirectory() as folder:
                report, _ = self.invoke(Path(folder) / "bad", native=invalid, code=78)
                self.assertEqual(report["status"], "failed")

    def test_compile_failure_timeout_and_duplicate_native_json_cannot_pass(self):
        for values in ({"fail_compile": True}, {"timed_out": True}, {"code": 1}, {"duplicate_json": True}):
            with self.subTest(values=values), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "new"
                report, _ = self.invoke(output, **values)
                self.assertEqual(report["status"], "failed")
                self.assertIs(report["authored_legacy_gpu_execution_verified"], False)
                self.assertTrue(list(output.glob("*.stderr.txt")))
                self.assertEqual(json.loads((output / "result.json").read_text()), report)

    def test_unknown_diagnostic_format_retains_native_gpu_words_but_does_not_complete_comparison(self):
        native = native_report()
        native["cases"][0]["token_diagnostic"]["error"]["diagnostics"] = "unknown localized format"
        with tempfile.TemporaryDirectory() as folder:
            report, _ = self.invoke(Path(folder) / "new", native=native)
            self.assertEqual(report["status"], "unsupported_diagnostic_format")
            self.assertIn("words_u32", report["native"]["cases"][0]["gpu"])
            self.assertIs(report["authored_legacy_gpu_execution_verified"], False)

    def test_interruptions_keep_terminal_report_and_original_exception(self):
        for error in (KeyboardInterrupt(), SystemExit(43)):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "new"
                with patch.object(probe.platform, "system", return_value="Darwin"), \
                        patch.object(probe.control, "run_command", side_effect=error), self.assertRaises(type(error)) as raised:
                    probe.run(probe.parse_args(["--output", str(output)]))
                self.assertIs(raised.exception, error)
                report = json.loads((output / "result.json").read_text())
                self.assertEqual(report["status"], "interrupted")
                self.assertIs(report["authored_legacy_gpu_execution_verified"], False)

    def test_atomic_report_does_not_replace_complete_json_with_partial_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            probe.write_report(output, {"status": "old"})
            original = Path.write_text
            def interrupt(path, text, **kwargs):
                original(path, text[:7], **kwargs)
                raise KeyboardInterrupt
            with patch.object(Path, "write_text", interrupt), self.assertRaises(KeyboardInterrupt):
                probe.write_report(output, {"status": "new"})
            self.assertEqual(json.loads((output / "result.json").read_text()), {"status": "old"})

    def test_invalid_budgets_are_rejected(self):
        for option, value in (("--budget", "0"), ("--budget", "nan"), ("--budget", "721"), ("--timeout", "91"), ("--timeout", "inf")):
            with self.subTest(option=option, value=value), patch("sys.stderr"), self.assertRaises(SystemExit):
                probe.parse_args(["--output", "unused", option, value])


if __name__ == "__main__":
    unittest.main()
