"""Authored parser/runner controls only; no Apple or GPU result is simulated as evidence."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("macro_probe_tested", ROOT / "experiments/metal-compile/macro_probe.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)
SOURCE = (ROOT / "experiments/metal-compile/macro_diagnostics.metal").read_text()
LINES = probe.source_lines(SOURCE)

def diagnostics(values=None, path="program_source", include_echo=True):
    values = probe.TOKENS if values is None else values
    result = []
    for name, line in LINES.items():
        result.append(f'{path}:{line}:1: error: static_assert failed "TOKEN_BEGIN_{name}__{values[name]}__TOKEN_END"')
        if include_echo:
            result.append(SOURCE.splitlines()[line - 1])
            result.append("^             ~~~~~")
    return "\n".join(result) + "\n8 errors generated.\n"

def string_value(text):
    return {"status": "captured", "kind": "NSString", "text": text}

def observed_number(name):
    result = {"status": "captured", "kind": "NSNumber", "original_declaration_recovered": False}
    if name == "BOOL_VALUE":
        return {**result, "objc_type": "c", "category": "boolean", "value": True}
    if name.startswith("F32"):
        value = -0.0 if name == "F32_NEG_ZERO" else 1.0 + 2.0 ** -23
        return {**result, "objc_type": "d", "category": "binary_float", "observed_width_bits": 64,
                "bits_hex": struct.pack(">d", value).hex(), "negative_zero": name == "F32_NEG_ZERO", "classification": "finite"}
    value = {"U64_MAX": 2**64 - 1, "U64_HIGH": 2**63, "I64_MIN": -2**63, "U32_MAX": 2**32 - 1}[name]
    unsigned = name.startswith("U64")
    return {**result, "objc_type": "Q" if unsigned else "q", "category": "unsigned_integer" if unsigned else "signed_integer",
            "observed_width_bits": 64, "decimal": str(value), "bits_hex": f"{value & (2**64-1):016x}"}

def native_report():
    rows = []
    for name in ("runtime_number", "runtime_string"):
        macros = [{"name": key, "value": string_value(probe.TOKENS[key]) if name == "runtime_string" or key == "TOKEN_STRING" else observed_number(key)} for key in sorted(probe.TOKENS)]
        values = {"languageVersion": {"raw": 0x30001, "known_name": None},
                  "mathMode": {"raw": 0, "known_name": "safe"},
                  "mathFloatingPointFunctions": {"raw": 0, "known_name": "precise"},
                  "preserveInvariance": False, "optimizationLevel": {"raw": 0, "is_default_enum": True},
                  "preprocessorMacros": {"status": "captured", "is_nil": False, "entries": macros}}
        fields = {key: {"availability": "supported", "origin": "explicit_fixture_assignment", "value": value} for key, value in values.items()}
        rows.append({"case": name, "status": "deliberately_rejected", "library_created": False, "error_present": True,
                     "diagnostics_truncated": False, "requested_options_getters_match": True, "diagnostics": diagnostics(),
                     "effective_options": {"explicit_fixture_setters": probe.SETTERS, "fresh_unmodified_object": False,
                                           "runtime_assignment_provenance_verified": False, "fields": fields}})
    return {"schema": 1, "scope": probe.SCOPE, "status": "deliberate_rejections_observed", "expected_compilation_failure": True,
            **{name: False for name in probe.FALSE_EVIDENCE}, "device_present": True, "requested_string_macros": probe.TOKENS,
            "authored_constructor_arguments": probe.declarations(), "cases": rows}

class DiagnosticTests(unittest.TestCase):
    def test_actual_observed_format_separates_expansion_and_source_echo(self):
        self.assertEqual(probe.parse_diagnostics(diagnostics(), LINES), probe.TOKENS)
        path = r"F:\authored directory\macro_diagnostics.metal"
        self.assertEqual(probe.parse_diagnostics(diagnostics(path=path), LINES, Path(path)), probe.TOKENS)
        self.assertEqual(probe.parse_diagnostics(diagnostics(path=path.replace("\\", "\\\\")), LINES, Path(path)), probe.TOKENS)

    def test_source_echo_cannot_supply_an_observation(self):
        with self.assertRaises(probe.UnsupportedDiagnostics):
            probe.parse_diagnostics(SOURCE, LINES)
        text = diagnostics().replace('error: static_assert failed "TOKEN_BEGIN_U64_MAX__18446744073709551615ull__TOKEN_END"', 'note: static_assert failed "TOKEN_BEGIN_U64_MAX__18446744073709551615ull__TOKEN_END"')
        with self.assertRaises(probe.UnsupportedDiagnostics):
            probe.parse_diagnostics(text, LINES)

    def test_changed_format_missing_and_unrelated_errors_are_unsupported(self):
        original = diagnostics(include_echo=False)
        cases = [original.replace("static_assert failed", "static assertion failed:"),
                 original.replace("8 errors generated.", "program_source:3:1: error: unrelated failure"),
                 "\n".join(original.splitlines()[1:]), "x" * 65537]
        for text in cases:
            with self.subTest(text=text[:30]), self.assertRaises(probe.UnsupportedDiagnostics):
                probe.parse_diagnostics(text, LINES)

    def test_identical_or_conflicting_duplicate_markers_are_ambiguous(self):
        original = diagnostics()
        first = original.splitlines()[0]
        for extra in (first, first.replace("18446744073709551615ull", "0")):
            with self.subTest(extra=extra), self.assertRaises(probe.UnsupportedDiagnostics):
                probe.parse_diagnostics(original + extra, LINES)

    def test_source_line_column_and_marker_injection_fail_closed(self):
        original = diagnostics()
        cases = [original.replace("program_source:", "other_source:"),
                 original.replace(f":{LINES['U64_MAX']}:1:", ":999:1:"),
                 original.replace(":1: error:", ":2: error:"),
                 original.replace("18446744073709551615ull", '1" __TOKEN_END'),
                 original.replace("18446744073709551615ull", "TOKEN_BEGIN_OTHER__1")]
        for text in cases:
            with self.subTest(text=text[:70]), self.assertRaises(probe.UnsupportedDiagnostics):
                probe.parse_diagnostics(text, LINES)

    def test_value_spelling_changes_are_observations_not_numeric_equivalence(self):
        report = native_report()
        altered = {**probe.TOKENS, "F32_NEG_ZERO": "0", "U64_MAX": "18446744073709551615"}
        report["cases"][0]["diagnostics"] = diagnostics(altered)
        captured = probe.validate_native(report, LINES)
        self.assertEqual(captured["runtime_number"]["F32_NEG_ZERO"], "0")
        self.assertNotEqual(captured["runtime_number"], captured["runtime_string"])
        self.assertIs(report["typed_value_equivalence_verified"], False)
        # Returned NSNumber double width can differ from its authored float input.
        self.assertEqual(report["authored_constructor_arguments"]["F32_NEXT"]["input_width_bits"], 32)

    def test_native_success_instead_of_rejection_or_truncated_text_is_not_observation(self):
        for key, value in (("library_created", True), ("error_present", False), ("diagnostics_truncated", True), ("requested_options_getters_match", False)):
            report = native_report()
            report["cases"][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate_native(report, LINES)

    def test_scope_setter_and_constructor_claims_cannot_be_promoted(self):
        for key in probe.FALSE_EVIDENCE:
            report = native_report(); report[key] = True
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate_native(report, LINES)
        report = native_report(); report["authored_constructor_arguments"]["U32_MAX"]["input_width_bits"] = 64
        with self.assertRaises(ValueError): probe.validate_native(report, LINES)
        report = native_report(); report["cases"][0]["effective_options"]["fields"]["mathMode"]["value"]["known_name"] = "fast"
        with self.assertRaises(ValueError): probe.validate_native(report, LINES)

class RunnerTests(unittest.TestCase):
    def fake_tool(self, output, report, *, code=1, text=None, timed_out=False):
        source = output / "macro_diagnostics.metal"
        content = diagnostics(path=str(source)) if text is None else text
        (output / "fake.stderr.txt").write_text(content)
        event = {"label": "synthetic", "returncode": code, "timed_out": timed_out, "interrupted": False, "stderr": "fake.stderr.txt"}
        report["commands"].append(event)
        return event, "", content

    def invoke(self, output, *, code=1, text=None, timed_out=False):
        deadlines = []
        def command(argv, label, directory, report, env, timeout, *, deadline):
            deadlines.append(deadline)
            if label == "metal-version": return {"returncode": 0, "timed_out": False}, "fake control only", ""
            return self.fake_tool(directory, report, code=code, text=text, timed_out=timed_out)
        with patch.object(probe.platform, "system", return_value="Windows"), \
                patch.object(probe.control, "resolve_tools", return_value=({"metal": Path(__file__), "metallib": Path(__file__)}, None)), \
                patch.object(probe.control, "run_command", side_effect=command):
            report = probe.run(probe.parse_args(["--output", str(output), "--budget", "90"]))
        self.assertEqual(len(set(deadlines)), 1)
        return report

    def test_expected_offline_rejection_keeps_evidence_and_is_not_shader_success(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "new"
            report = self.invoke(output)
            self.assertEqual(report["status"], "offline_tokens_observed")
            self.assertIs(report["native_tokens_observed"], False)
            for name in probe.FALSE_EVIDENCE: self.assertIs(report[name], False)
            self.assertEqual(json.loads((output / "result.json").read_text()), report)
            with self.assertRaises(FileExistsError): self.invoke(output)

    def test_unexpected_success_timeout_or_bad_format_never_pass(self):
        for values, status in (({"code": 0}, "failed"), ({"timed_out": True}, "failed"), ({"text": SOURCE}, "unsupported_diagnostic_format")):
            with self.subTest(values=values), tempfile.TemporaryDirectory() as folder:
                report = self.invoke(Path(folder) / "new", **values)
                self.assertEqual(report["status"], status)
                self.assertIs(report["offline_tokens_observed"], False)

    def test_interruption_preserves_terminal_report_and_original_exception(self):
        for error in (KeyboardInterrupt(), SystemExit(43)):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "new"
                with patch.object(probe.platform, "system", return_value="Windows"), \
                        patch.object(probe.control, "resolve_tools", side_effect=error), self.assertRaises(type(error)) as raised:
                    probe.run(probe.parse_args(["--output", str(output)]))
                self.assertIs(raised.exception, error)
                report = json.loads((output / "result.json").read_text())
                self.assertEqual(report["status"], "interrupted")
                self.assertEqual(report["reason"], type(error).__name__)
                self.assertIs(report["native_tokens_observed"], False)

    def test_atomic_report_retains_previous_json_when_staging_or_publish_is_interrupted(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            before, after = {"status": "old_complete"}, {"status": "new_complete"}
            probe.write_report(output, before)
            original_write = Path.write_text
            def interrupted_write(path, text, **kwargs):
                original_write(path, text[:7], **kwargs)
                raise SystemExit(44)
            with patch.object(Path, "write_text", interrupted_write), self.assertRaises(SystemExit):
                probe.write_report(output, after)
            self.assertEqual(json.loads((output / "result.json").read_text()), before)
            self.assertEqual(len((output / "result.tmp").read_text()), 7)
            with patch.object(Path, "replace", side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
                probe.write_report(output, after)
            self.assertEqual(json.loads((output / "result.json").read_text()), before)
            self.assertEqual(json.loads((output / "result.tmp").read_text()), after)
            probe.write_report(output, after)
            self.assertEqual(json.loads((output / "result.json").read_text()), after)
            self.assertFalse((output / "result.tmp").exists())

    def test_bad_budgets_are_rejected(self):
        for option, value in (("--budget", "0"), ("--budget", "nan"), ("--budget", "721"), ("--timeout", "91"), ("--timeout", "inf")):
            with self.subTest(option=option, value=value), patch("sys.stderr"), self.assertRaises(SystemExit):
                probe.parse_args(["--output", "unused", option, value])

if __name__ == "__main__":
    unittest.main()
