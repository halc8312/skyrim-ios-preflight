"""Public mobile-controls command/parser tests; no SDK, game, or device is used."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    "mobile_controls_sdk_driver", Path(__file__).with_name("run_mobile_controls_tests.py"))
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)

DESTINATION = "platform=iOS Simulator,id=12345678-1234-1234-1234-123456789abc"


def console(order=None):
    cases = list(driver.TESTS if order is None else order)
    text = ""
    for name in cases:
        case = "-[OwnedGameHostTests.MobileControlsTests " + name + "]"
        text += f"Test Case '{case}' started.\nTest Case '{case}' passed (0.001 seconds).\n"
    return (text + "Executed 10 tests, with 0 failures (0 unexpected) in 0.010 seconds\n"
            "Executed 10 tests, with 0 failures (0 unexpected) in 0.010 seconds\n"
            "** TEST EXECUTE SUCCEEDED **\n")


def summary():
    return dict(totalTestCount=10, passedTests=10, failedTests=0, skippedTests=0, expectedFailures=0)


EXPORT_HELP = ("USAGE: xcresulttool export attachments [--path <path>] [--output-path <output-path>]\n"
               "OPTIONS:\n  --path <path>  Result bundle.\n  --output-path <output-path>  Export directory.\n")
PUBLIC_PNG = (b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR" + b"\x00" * 17)


def fixture_paths(root):
    stage = root / "stage"
    built = root / "xctest"
    output = root / "xctest-mobile"
    (stage / "OwnedGameHost.xcodeproj").mkdir(parents=True)
    (built / "DerivedData/Build/Products/Debug-iphonesimulator/OwnedGameHost.app").mkdir(parents=True)
    return stage, built, output


class MobileControlsDriverTests(unittest.TestCase):
    def test_exact_ten_cases_accept_any_serial_order(self):
        self.assertEqual(len(driver.TESTS), 10)
        self.assertEqual(len(set(driver.TESTS)), 10)
        for order in (driver.TESTS, tuple(reversed(driver.TESTS)),
                      driver.TESTS[4:] + driver.TESTS[:4]):
            with self.subTest(order=order):
                driver.validate_console(console(order))

    def test_missing_duplicate_extra_wrong_failed_skipped_and_interleaved_refuse(self):
        valid = console()
        lines = valid.splitlines(keepends=True)
        interleaved = lines[:]
        interleaved[1], interleaved[2] = interleaved[2], interleaved[1]
        bad = [console(driver.TESTS[:-1]), console(driver.TESTS[:-1] + driver.TESTS[:1]),
               console(driver.TESTS + ("testUnexpected",)),
               console(("testUnexpected",) + driver.TESTS[1:]),
               valid.replace("started.", "skipped.", 1),
               valid.replace("passed (", "failed (", 1),
               "".join(lines[1:]), "".join(lines[:1] + lines[2:]), "".join(interleaved),
               valid + "Test Case 'unexpected' skipped.\n"]
        for value in bad:
            with self.subTest(value=value), self.assertRaises(ValueError):
                driver.validate_console(value)

    def test_wrong_totals_or_nonunique_terminal_refuse(self):
        valid = console()
        for value in [valid.replace("Executed 10 tests", "Executed 9 tests"),
                      valid.replace("Executed 10 tests", "Executed 11 tests"),
                      valid.replace("0 failures", "1 failure"),
                      valid.replace("0 unexpected", "1 unexpected"),
                      "\n".join(line for line in valid.splitlines() if not line.startswith("Executed")),
                      valid.replace("** TEST EXECUTE SUCCEEDED **", "** TEST SUCCEEDED **"),
                      valid.replace("** TEST EXECUTE SUCCEEDED **", "** TEST EXECUTE FAILED **"),
                      valid + "** TEST EXECUTE SUCCEEDED **\n", valid + "** TEST EXECUTE FAILED **\n"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                driver.validate_console(value)

    def test_off_main_layer_warning_refuses(self):
        for apostrophe in ("'", "’"):
            with self.subTest(apostrophe=apostrophe), self.assertRaises(ValueError):
                driver.validate_console(console() +
                    f"Modifying properties of a view{apostrophe}s layer off the main thread is not allowed\n")

    def test_summary_exact_integer_counts_and_malformed_refusal(self):
        driver.validate_summary(summary())
        for key in summary():
            for value in (None, True, False, 9.0, "9", -1, summary()[key] + 1):
                altered = dict(summary(), **{key: value})
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    driver.validate_summary(altered)
            missing = summary()
            del missing[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                driver.validate_summary(missing)
        for value in (None, [], 9, "summary"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                driver.validate_summary(value)
        for raw in ('{"totalTestCount":9,"totalTestCount":9}', '{"value":NaN}', '{"value":Infinity}'):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                driver._sdk.strict_json(raw)

    def test_command_reuses_exact_products_destination_and_only_ten_cases(self):
        derived = Path("existing") / "DerivedData"
        result = Path("fresh") / "MobileControls.xcresult"
        command = driver.make_test_command(derived, DESTINATION, result)
        self.assertEqual(command[0], "xcodebuild")
        self.assertEqual(command[-1], "test-without-building")
        self.assertEqual(command[command.index("-derivedDataPath") + 1], str(derived))
        self.assertEqual(command[command.index("-destination") + 1], DESTINATION)
        self.assertEqual(command[command.index("-resultBundlePath") + 1], str(result))
        self.assertEqual(command[command.index("-parallel-testing-enabled") + 1], "NO")
        self.assertEqual(command[command.index("-test-timeouts-enabled") + 1], "YES")
        for flag in ("-default-test-execution-time-allowance", "-maximum-test-execution-time-allowance"):
            self.assertEqual(command[command.index(flag) + 1], "40")
        self.assertEqual([arg for arg in command if arg.startswith("-only-testing:")],
                         ["-only-testing:OwnedGameHostTests/MobileControlsTests/" + name for name in driver.TESTS])
        self.assertTrue({"CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO"}.issubset(command))
        for token in ("build", "build-for-testing", "xcodegen", "-retry-tests-on-failure",
                      "-test-iterations", "-run-tests-until-failure"):
            self.assertNotIn(token, command)
        self.assertFalse(any("OwnedHostFullChainTests" in arg or "NativeTraceSDKProbeTests" in arg
                             for arg in command))

    def test_fresh_separate_output_and_existing_products_required(self):
        with tempfile.TemporaryDirectory() as temp:
            stage, built, output = fixture_paths(Path(temp))
            resolved = driver.prepare_paths(stage, built, output, DESTINATION)
            self.assertEqual(resolved, (stage.resolve(), (built / "DerivedData").resolve(), output.resolve()))
            with self.assertRaises(FileExistsError):
                driver.prepare_paths(stage, built, output, DESTINATION)
            for bad in (stage / "evidence", built / "evidence", stage, built, Path(temp)):
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    driver.prepare_paths(stage, built, bad, DESTINATION)
            for destination in ("platform=iOS Simulator", DESTINATION + ",name=extra", "id=1234"):
                fresh = Path(temp) / "invalid-output"
                with self.subTest(destination=destination), self.assertRaises(ValueError):
                    driver.prepare_paths(stage, built, fresh, destination)
                self.assertFalse(fresh.exists())
            app = built / "DerivedData/Build/Products/Debug-iphonesimulator/OwnedGameHost.app"
            app.rmdir()
            with self.assertRaises(ValueError):
                driver.prepare_paths(stage, built, Path(temp) / "missing-build-output", DESTINATION)

    def test_log_reader_is_bounded_and_rejects_directory_or_symlink(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "public.log"
            path.write_bytes(b"authored")
            self.assertEqual(driver.read_command_log(path, 8), b"authored")
            with self.assertRaises(ValueError):
                driver.read_command_log(path, 7)
            with self.assertRaises(ValueError):
                driver.read_command_log(Path(temp), 100)
            link = Path(temp) / "link.log"
            try:
                link.symlink_to(path)
            except (OSError, NotImplementedError):
                pass
            else:
                with self.assertRaises(ValueError):
                    driver.read_command_log(link, 100)

    def test_summary_command_shares_external_deadline_and_cap(self):
        with mock.patch.object(driver.time, "monotonic", return_value=100), \
             mock.patch.object(driver._sdk, "run_bounded", return_value=0) as run:
            driver.run_command(["authored"], Path("stage"), Path("out"), 107, "failed", cap=15)
            self.assertEqual(run.call_args.args[-1], 7)
            driver.run_command(["authored"], Path("stage"), Path("out"), 140, "failed", cap=15)
            self.assertEqual(run.call_args.args[-1], 15)
            with self.assertRaises(RuntimeError):
                driver.run_command(["authored"], Path("stage"), Path("out"), 100, "failed")
            self.assertEqual(run.call_count, 2)

    def test_non_darwin_refuses_before_output_or_command(self):
        with tempfile.TemporaryDirectory() as temp:
            stage, built, output = fixture_paths(Path(temp))
            with mock.patch.object(driver.sys, "platform", "linux"), \
                 mock.patch.object(driver._sdk, "run_bounded") as run, self.assertRaises(RuntimeError):
                driver.main(["--stage", str(stage), "--built-output", str(built), "--output", str(output),
                             "--destination", DESTINATION])
            run.assert_not_called()
            self.assertFalse(output.exists())

    def simulate(self, *, summary_value=None, console_value=None, code=0,
                 help_text=EXPORT_HELP, help_code=0, export_code=0, png_count=2,
                 png_data=PUBLIC_PNG, manifest=None, extra_file=None):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        stage, built, output = fixture_paths(Path(temp.name))
        seen = []

        def run(command, cwd, prefix, timeout):
            seen.append(command)
            self.assertEqual(cwd, stage.resolve())
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, driver.EXTERNAL_DEADLINE_SECONDS)
            if command[0] == "xcodebuild":
                Path(command[command.index("-resultBundlePath") + 1]).mkdir()
                stdout = console() if console_value is None else console_value
            elif command[:5] == ["xcrun", "xcresulttool", "get", "test-results", "summary"]:
                stdout = json.dumps(summary() if summary_value is None else summary_value)
            elif command == ["xcrun", "xcresulttool", "export", "attachments", "--help"]:
                stdout = help_text
            else:
                self.assertEqual(command[:4], ["xcrun", "xcresulttool", "export", "attachments"])
                self.assertNotIn("--only-failures", command)
                screenshots = Path(command[command.index("--output-path") + 1])
                self.assertEqual(screenshots, output.resolve() / "screenshots")
                (screenshots / "manifest.json").write_text(json.dumps([] if manifest is None else manifest))
                for number in range(png_count):
                    (screenshots / f"public-{number}.png").write_bytes(png_data)
                if extra_file is not None:
                    (screenshots / extra_file).write_bytes(b"authored")
                stdout = ""
            prefix.with_suffix(".stdout.log").write_text(stdout, encoding="utf-8")
            prefix.with_suffix(".stderr.log").write_text("", encoding="utf-8")
            if command[-1] == "--help":
                return help_code
            if command[:4] == ["xcrun", "xcresulttool", "export", "attachments"]:
                return export_code
            return code

        with mock.patch.object(driver.sys, "platform", "darwin"), \
             mock.patch.object(driver._sdk, "run_bounded", side_effect=run), \
             contextlib.redirect_stdout(io.StringIO()):
            try:
                result = driver.main(["--stage", str(stage), "--built-output", str(built),
                                      "--output", str(output), "--destination", DESTINATION])
            except (RuntimeError, ValueError) as error:
                return output, seen, error
        self.assertEqual(result, 0)
        return output, seen, None

    def test_synthetic_success_captures_separate_evidence_with_numeric_public_scope(self):
        output, commands, error = self.simulate()
        self.assertIsNone(error)
        self.assertEqual(len(commands), 4)
        self.assertTrue((output / "MobileControls.xcresult").is_dir())
        self.assertEqual(json.loads((output / "xcresult-summary.json").read_text()), summary())
        report = json.loads((output / "public-mobile-controls-result.json").read_text())
        self.assertTrue(all(type(value) is int for value in report.values()))
        self.assertEqual(report["public_mobile_controls_passed"], 1)
        self.assertEqual(report["reused_build_products"], 1)
        self.assertEqual(report["public_tests_only"], 1)
        for field in ("build_performed", "game_executed", "game_frame_rendered", "physical_iphone_verified",
                      "private_guest_framework_verified", "ready_to_play", "visual_review_performed"):
            self.assertEqual(report[field], 0)
        self.assertEqual(report["attachment_export_available"], 1)
        self.assertEqual(report["attachment_exported"], 1)
        self.assertEqual(report["exported_png_count"], 2)
        self.assertTrue((output / "screenshots/manifest.json").exists())
        self.assertTrue((output / "xcresult-attachments-help.txt").exists())
        for prefix in ("xcodebuild", "xcresult-summary", "xcresult-attachments-help", "xcresult-attachments-export"):
            for stream in ("stdout", "stderr"):
                self.assertTrue((output / f"{prefix}.{stream}.log").is_file())

    def test_failure_never_writes_success_outcome(self):
        for kwargs in (dict(code=65), dict(console_value=console().replace("passed (", "failed (", 1)),
                       dict(summary_value=dict(summary(), passedTests=9, failedTests=1)),
                       dict(summary_value=dict(summary(), skippedTests=1)), dict(export_code=1),
                       dict(png_count=1), dict(png_count=3), dict(png_data=b"not a PNG"),
                       dict(manifest=42), dict(extra_file="unexpected.txt")):
            with self.subTest(kwargs=kwargs):
                output, _, error = self.simulate(**kwargs)
                self.assertIsNotNone(error)
                self.assertFalse((output / "public-mobile-controls-result.json").exists())
                self.assertTrue((output / "xcodebuild.stdout.log").exists())

    def test_unsupported_attachment_interface_is_explicit_without_guessed_fallback(self):
        for kwargs in (dict(help_code=64), dict(help_text="unknown command"),
                       dict(help_text=EXPORT_HELP.replace("--output-path", "--some-other-option"))):
            with self.subTest(kwargs=kwargs):
                output, commands, error = self.simulate(**kwargs)
                self.assertIsNone(error)
                self.assertEqual(len(commands), 3)
                self.assertFalse((output / "screenshots").exists())
                report = json.loads((output / "public-mobile-controls-result.json").read_text())
                for key in ("attachment_export_available", "attachment_exported", "exported_png_count",
                            "visual_review_performed"):
                    self.assertEqual(report[key], 0)
                self.assertEqual(report["public_mobile_controls_passed"], 1)
                self.assertTrue((output / "xcresult-attachments-help.txt").exists())


if __name__ == "__main__":
    unittest.main()
