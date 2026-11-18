"""Portable validation of authored option reports; no Apple getter execution."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_options_probe_test", ROOT / "experiments/metal-compile/options_probe.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def number(value, encoding, width):
    category = "binary_float" if encoding in "fd" else "signed_integer" if encoding in "csilq" else "unsigned_integer"
    result = {"status": "captured", "kind": "NSNumber", "objc_type": encoding,
              "original_declaration_recovered": False, "category": category, "observed_width_bits": width}
    if category == "binary_float":
        bits = struct.pack(">f" if width == 32 else ">d", value).hex()
        result.update(bits_hex=bits, classification="finite", negative_zero=value == 0 and bool(int(bits, 16) >> (width - 1)))
    else:
        result.update(bits_hex=f"{value & ((1 << width) - 1):0{width // 4}x}", decimal=str(value))
    return result


def observed_macros():
    # Deliberate representation normalization: I8 becomes an observed int32,
    # U32 becomes int64, and both declared float widths become observed double.
    entries = {"I8": number(-7, "i", 32), "I16": number(-300, "i", 32), "I32": number(-70000, "i", 32),
               "I64_MIN": number(-(1 << 63), "q", 64), "U8": number(255, "i", 32), "U16": number(65535, "i", 32),
               "U32": number((1 << 32) - 1, "q", 64), "U64_MAX": number((1 << 64) - 1, "Q", 64),
               "F32": number(1.25, "d", 64), "F32_NEG_ZERO": number(-0.0, "d", 64),
               "F64": number(1.25, "d", 64), "F64_NEG_ZERO": number(-0.0, "d", 64),
               "BOOL": {"status": "captured", "kind": "NSNumber", "objc_type": "c", "category": "boolean",
                        "original_declaration_recovered": False, "value": True},
               "TOKEN_STRING": {"status": "captured", "kind": "NSString", "text": "0x1234u"}}
    return {"is_nil": False, "status": "captured", "entries": [{"name": name, "value": value} for name, value in sorted(entries.items())]}


def synthetic_report(sdk=260000, os_major=26):
    values = {"languageVersion": {"raw": 0, "known_name": None}, "fastMathEnabled": True,
              "preserveInvariance": False, "optimizationLevel": {"raw": 0, "is_default_enum": True},
              "preprocessorMacros": {"is_nil": True, "status": "captured", "entries": []}, "libraryType": 0,
              "compileSymbolVisibility": 0, "allowReferencingUndefinedSymbols": False, "maxTotalThreadsPerThreadgroup": 0,
              "installName": None, "libraries": {"is_nil": True, "count": 0, "status": "empty_observed", "nonempty_library_identity_verified": False},
              "mathMode": {"raw": 0, "known_name": "fast"}, "mathFloatingPointFunctions": {"raw": 0, "known_name": "fast"},
              "enableLogging": False, "requiredThreadsPerThreadgroup": [0, 0, 0], "floatingPointConversionRoundingMode": 0}
    defaults = {}
    for name, value in values.items():
        min_sdk, min_os = probe.LATE_FIELDS.get(name, (0, (0, 0)))
        if sdk < min_sdk:
            defaults[name] = {"availability": "unsupported_sdk", "origin": "unobserved"}
        elif (os_major, 3) < min_os:
            defaults[name] = {"availability": "unsupported_os", "origin": "unobserved"}
        else:
            defaults[name] = {"availability": "supported", "origin": "default_observed", "value": value}
    cases = []

    def case(name, setters):
        fields = copy.deepcopy(defaults)
        for field, observation in fields.items():
            if observation["availability"] != "supported":
                continue
            observation["origin"] = "not_explicitly_assigned_by_fixture"
            if field in setters:
                observation["origin"] = "explicit_fixture_assignment"
        row = {"case": name, "fields": fields, "explicit_fixture_setters": setters,
               "fresh_unmodified_object": False, "runtime_assignment_provenance_verified": False,
               "authored_macro_declarations": {}}
        cases.append(row)
        return fields

    cases.append({"case": "fresh_defaults", "fields": copy.deepcopy(defaults), "explicit_fixture_setters": [],
                  "fresh_unmodified_object": True, "runtime_assignment_provenance_verified": False, "authored_macro_declarations": {}})
    for enabled in (False, True):
        fields = case("legacy_fast" if enabled else "legacy_safe", ["fastMathEnabled"])
        fields["fastMathEnabled"]["value"] = enabled
        if os_major >= 15:
            fields["mathMode"]["value"] = {"raw": 0 if enabled else 2, "known_name": "fast" if enabled else "safe"}
            fields["mathFloatingPointFunctions"]["value"] = {"raw": 0 if enabled else 1, "known_name": "fast" if enabled else "precise"}
            fields["mathMode"]["origin"] = fields["mathFloatingPointFunctions"]["origin"] = "affected_by_legacy_math_assignment"
    if os_major >= 15:
        for mode in ("safe", "relaxed", "fast"):
            fields = case("modern_" + mode, probe.MODERN_SETTERS)
            fields["fastMathEnabled"]["origin"] = "observed_after_modern_math_assignment"
            fields["languageVersion"]["value"] = {"raw": 0x20004, "known_name": "2.4"}
            fields["mathMode"]["value"] = {"raw": 0, "known_name": mode}
            fields["mathFloatingPointFunctions"]["value"] = {"raw": 0, "known_name": "fast" if mode == "fast" else "precise"}
            fields["preserveInvariance"]["value"] = True
    fields = case("typed_macros", ["preprocessorMacros"])
    fields["preprocessorMacros"]["value"] = observed_macros()
    cases[-1]["authored_macro_declarations"] = probe.expected_macros()
    fields = case("explicit_empty_and_output", ["preprocessorMacros", "libraries", "libraryType", "installName"])
    fields["preprocessorMacros"]["value"]["is_nil"] = False
    fields["libraries"]["value"]["is_nil"] = False
    fields["installName"]["value"] = "authored-option-snapshot"
    result = {"schema": 1, "scope": probe.SCOPE, "status": "captured", "sdk_max_allowed": sdk, "os_version": [os_major, 3, 0],
              "native_option_getters_verified": True, "unsupported_macro_value_rejected": True,
              **{name: False for name in probe.FALSE_EVIDENCE}, "cases": cases}
    if os_major < 15:
        result["modern_authored_cases"] = "unsupported_os"
    return result


def macro(report, name):
    case = next(case for case in report["cases"] if case["case"] == "typed_macros")
    return next(row["value"] for row in case["fields"]["preprocessorMacros"]["value"]["entries"] if row["name"] == name)


class SnapshotValidationTests(unittest.TestCase):
    def test_valid_authored_snapshot_keeps_declared_and_observed_widths_separate(self):
        report = synthetic_report()
        result = probe.validate_native(report)
        self.assertTrue(result["all_authored_macro_values_preserved"])
        self.assertFalse(result["original_declaration_recovered"])
        self.assertEqual(macro(report, "I8")["observed_width_bits"], 32)
        self.assertEqual(probe.expected_macros()["I8"]["declared_width_bits"], 8)
        self.assertEqual(macro(report, "F32")["observed_width_bits"], 64)

    def test_signed_min_unsigned_max_are_exact_decimal_strings(self):
        self.assertEqual(probe.numeric_value(macro(synthetic_report(), "I64_MIN")), -(1 << 63))
        self.assertEqual(probe.numeric_value(macro(synthetic_report(), "U64_MAX")), (1 << 64) - 1)
        report = synthetic_report()
        macro(report, "U64_MAX")["decimal"] = (1 << 64) - 1
        with self.assertRaisesRegex(ValueError, "decimal malformed"):
            probe.validate_native(report)

    def test_numeric_bits_width_and_sign_are_cross_checked(self):
        changes = [("I8", "bits_hex", "00000000"), ("I16", "observed_width_bits", 16),
                   ("I32", "category", "unsigned_integer"), ("U32", "objc_type", "Q"),
                   ("U64_MAX", "decimal", "18446744073709551616"), ("F32", "bits_hex", "3fa00000")]
        for name, field, value in changes:
            report = synthetic_report()
            macro(report, name)[field] = value
            with self.subTest(name=name, field=field), self.assertRaises(ValueError):
                probe.validate_native(report)

    def test_negative_zero_is_recorded_and_loss_is_an_observation(self):
        report = synthetic_report()
        self.assertTrue(macro(report, "F32_NEG_ZERO")["negative_zero"])
        macro(report, "F32_NEG_ZERO").update(bits_hex="0000000000000000", negative_zero=False)
        result = probe.validate_native(report)
        self.assertFalse(result["authored_macro_getter_value_preserved"]["F32_NEG_ZERO"])
        self.assertFalse(result["all_authored_macro_values_preserved"])
        self.assertFalse(result["metal_preprocessor_text_equivalence_verified"])

    def test_inconsistent_negative_zero_or_nonfinite_labels_are_rejected(self):
        for field, value in (("negative_zero", False), ("classification", "nan")):
            report = synthetic_report()
            macro(report, "F64_NEG_ZERO")[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                probe.validate_native(report)

    def test_scope_and_assignment_provenance_cannot_be_promoted(self):
        for name in probe.FALSE_EVIDENCE:
            report = synthetic_report()
            report[name] = True
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "unsupported evidence"):
                probe.validate_native(report)
        report = synthetic_report()
        report["cases"][1]["fields"]["mathMode"]["origin"] = "default_observed"
        with self.assertRaisesRegex(ValueError, "provenance"):
            probe.validate_native(report)
        report = synthetic_report()
        macro(report, "I8")["original_declaration_recovered"] = True
        with self.assertRaisesRegex(ValueError, "cannot recover"):
            probe.validate_native(report)

    def test_availability_is_distinct_for_sdk_os_and_missing_field(self):
        for sdk, os_major in ((150000, 26), (260000, 13), (270000, 26), (270000, 27)):
            with self.subTest(sdk=sdk, os=os_major):
                probe.validate_native(synthetic_report(sdk, os_major))
        report = synthetic_report()
        field = report["cases"][0]["fields"]["floatingPointConversionRoundingMode"]
        field["value"] = 0
        with self.assertRaisesRegex(ValueError, "invented value"):
            probe.validate_native(report)
        report = synthetic_report()
        del report["cases"][0]["fields"]["enableLogging"]
        with self.assertRaisesRegex(ValueError, "field set"):
            probe.validate_native(report)

    def test_legacy_mapping_modern_settings_and_empty_vs_nil_are_verified(self):
        report = synthetic_report()
        report["cases"][1]["fields"]["mathMode"]["value"]["known_name"] = "fast"
        with self.assertRaisesRegex(ValueError, "legacy math mapping"):
            probe.validate_native(report)
        report = synthetic_report()
        report["cases"][3]["fields"]["preserveInvariance"]["value"] = False
        with self.assertRaisesRegex(ValueError, "modern setter"):
            probe.validate_native(report)
        report = synthetic_report()
        report["cases"][-1]["fields"]["libraries"]["value"]["is_nil"] = True
        with self.assertRaisesRegex(ValueError, "empty/output"):
            probe.validate_native(report)

    def test_unknown_nonempty_libraries_are_not_silently_declared_equivalent(self):
        report = synthetic_report()
        libraries = report["cases"][0]["fields"]["libraries"]["value"]
        libraries.update(is_nil=False, count=1, status="nonempty_library_identity_unverified")
        probe.validate_native(report)
        libraries["nonempty_library_identity_verified"] = True
        with self.assertRaisesRegex(ValueError, "identity cannot"):
            probe.validate_native(report)

    def test_macro_duplicates_unsupported_values_and_changed_declarations_fail(self):
        report = synthetic_report()
        case = next(case for case in report["cases"] if case["case"] == "typed_macros")
        entries = case["fields"]["preprocessorMacros"]["value"]["entries"]
        entries.append(copy.deepcopy(entries[0]))
        with self.assertRaisesRegex(ValueError, "order/nil"):
            probe.validate_native(report)
        report = synthetic_report()
        macro(report, "TOKEN_STRING")["kind"] = "NSDate"
        with self.assertRaisesRegex(ValueError, "unsupported macro"):
            probe.validate_native(report)
        report = synthetic_report()
        next(case for case in report["cases"] if case["case"] == "typed_macros")["authored_macro_declarations"]["I8"]["declared_width_bits"] = 32
        with self.assertRaisesRegex(ValueError, "declarations differ"):
            probe.validate_native(report)


class OptionsRunnerTests(unittest.TestCase):
    def test_unsupported_host_keeps_sources_without_claiming_native_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "new"
            with patch.object(probe.platform, "system", return_value="Windows"), patch.object(probe.control, "run_command") as command:
                result = probe.run(probe.parse_args(["--output", str(output)]))
            command.assert_not_called()
            self.assertEqual(result["status"], "unsupported")
            self.assertFalse(result["native_compilation_verified"])
            self.assertFalse(result["native_option_getters_verified"])
            for name in probe.FALSE_EVIDENCE:
                self.assertIs(result[name], False)
            self.assertTrue((output / "options_probe.mm").is_file())
            self.assertEqual(json.loads((output / "result.json").read_text()), result)
            with self.assertRaises(FileExistsError):
                probe.run(probe.parse_args(["--output", str(output)]))

    def test_budget_stops_before_next_tool_and_retains_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "new"
            calls = []

            def completed(argv, label, out, report, env, timeout, *, deadline):
                calls.append((label, timeout, deadline))
                event = {"label": label, "returncode": 0, "timed_out": False}
                report["commands"].append(event)
                return event, "26.0", ""

            with patch.object(probe.platform, "system", return_value="Darwin"), \
                    patch.object(probe.time, "monotonic", side_effect=[0, 0, 1, 181, 182]), \
                    patch.object(probe.control, "run_command", side_effect=completed):
                result = probe.run(probe.parse_args(["--output", str(output), "--budget", "180"]))
            self.assertEqual(calls, [("sdk-version", 30, 180)])
            self.assertEqual(result["status"], "failed")
            self.assertIn("budget exhausted", result["reason"])
            self.assertFalse(result["native_compilation_verified"])
            self.assertEqual(json.loads((output / "result.json").read_text()), result)

    def test_interruption_keeps_partial_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "new"
            with patch.object(probe.platform, "system", return_value="Darwin"), \
                    patch.object(probe.control, "run_command", side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
                probe.run(probe.parse_args(["--output", str(output)]))
            result = json.loads((output / "result.json").read_text())
            self.assertEqual(result["status"], "interrupted")
            self.assertFalse(result["native_option_getters_verified"])

    def test_budget_must_be_finite_positive_and_bounded(self):
        for value in ("0", "-1", "nan", "inf", "721"):
            with self.subTest(value=value), patch("sys.stderr"), self.assertRaises(SystemExit):
                probe.parse_args(["--output", "unused", "--budget", value])


if __name__ == "__main__":
    unittest.main()
