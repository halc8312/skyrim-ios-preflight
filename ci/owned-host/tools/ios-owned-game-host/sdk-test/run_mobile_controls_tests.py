"""Run only public mobile-controls tests using an existing simulator SDK build."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import sys
import time


_spec = importlib.util.spec_from_file_location(
    "owned_host_existing_sdk_driver", Path(__file__).with_name("run_sdk_tests.py"))
_sdk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_sdk)

TESTS = (
    "testSimultaneousMovementCameraAndButtonsPreserveFullSnapshot",
    "testTwoFingersHoldingSameButtonReleaseIndependently",
    "testCancellationAndReconfigurationNeutralizeAllInputs",
    "testStickAndSwipeDirectionsSensitivityAndNeutralization",
    "testTouchOwnershipDoesNotStealActiveStick",
    "testButtonSlideAndOppositeDpadCancelSafely",
    "testAxisAndLayoutBoundsRejectNonFiniteInputs",
    "testLayoutDragPreservesGrabOffsetAndClampsSafeBounds",
    "testSettingsRoundTripAndVersionFallback",
    "testRenderedOverlayDefaultAndLayoutEditing",
)
CASES = frozenset("-[OwnedGameHostTests.MobileControlsTests " + name + "]" for name in TESTS)
EXTERNAL_DEADLINE_SECONDS = 120
CONSOLE_LIMIT = 32_000_000
SUMMARY_LIMIT = 2_000_000
DESTINATION = r"platform=iOS Simulator,id=[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}"


def validate_console(text: str) -> None:
    if re.search(r"Modifying properties of a view[’']s layer off the main thread", text):
        raise ValueError("UIKit reported layer mutation off the main thread")
    events = re.findall(r"(?m)^Test Case '([^'\r\n]+)' (started|passed|failed|skipped)\b", text)
    if len(events) != 2 * len(TESTS):
        raise ValueError("exactly ten mobile-controls start/pass pairs were not proven")
    seen = set()
    for index in range(0, len(events), 2):
        name, event = events[index]
        if (name not in CASES or name in seen or event != "started"
                or events[index + 1] != (name, "passed")):
            raise ValueError("mobile-controls cases must each start and pass once, serially")
        seen.add(name)
    if seen != CASES:
        raise ValueError("mobile-controls case names differ from the fixed contract")
    terminals = [line.strip() for line in text.splitlines()
                 if line.strip().startswith("** TEST ") and line.strip().endswith(" **")]
    if terminals != ["** TEST EXECUTE SUCCEEDED **"]:
        raise ValueError("single test-without-building success marker missing")
    totals = re.findall(r"Executed (\d+) tests?, with (\d+) failures? \((\d+) unexpected\)", text)
    # The selected class and its enclosing suites all contain these same ten cases.
    if not totals or any(row != (str(len(TESTS)), "0", "0") for row in totals):
        raise ValueError("console totals differ from ten cases without failures")


def validate_summary(summary) -> None:
    if type(summary) is not dict:
        raise ValueError("xcresult summary must be an object")
    for key, value in dict(totalTestCount=len(TESTS), passedTests=len(TESTS),
                           failedTests=0, skippedTests=0, expectedFailures=0).items():
        if type(summary.get(key)) is not int or summary[key] != value:
            raise ValueError("xcresult summary count differs: " + key)


def make_test_command(derived_data: Path, destination: str, result: Path) -> list[str]:
    return ["xcodebuild", "-project", "OwnedGameHost.xcodeproj", "-scheme", "OwnedGameHost",
            "-configuration", "Debug", "-destination", destination,
            "-derivedDataPath", str(derived_data), "-resultBundlePath", str(result),
            "-parallel-testing-enabled", "NO", "-test-timeouts-enabled", "YES",
            "-default-test-execution-time-allowance", "40",
            "-maximum-test-execution-time-allowance", "40",
            *("-only-testing:OwnedGameHostTests/MobileControlsTests/" + name for name in TESTS),
            "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO", "test-without-building"]


def prepare_paths(stage_arg: Path, built_arg: Path, output_arg: Path,
                  destination: str) -> tuple[Path, Path, Path]:
    if not re.fullmatch(DESTINATION, destination):
        raise ValueError("explicit single Simulator destination required")
    stage = stage_arg.resolve(strict=True)
    built = built_arg.resolve(strict=True)
    derived = (built / "DerivedData").resolve(strict=True)
    app = derived / "Build/Products/Debug-iphonesimulator/OwnedGameHost.app"
    if (not stage.is_dir() or not built.is_dir() or not derived.is_dir()
            or not (stage / "OwnedGameHost.xcodeproj").is_dir() or not app.is_dir()
            or not derived.is_relative_to(built)
            or not app.resolve(strict=True).is_relative_to(derived)):
        raise ValueError("existing simulator project and build products required")
    output = output_arg.resolve()
    for existing in (stage, built):
        if output == existing or output.is_relative_to(existing) or existing.is_relative_to(output):
            raise ValueError("mobile-controls evidence must be separate from stage and build products")
    output.mkdir()  # Fresh evidence only; never overwrite a previous run.
    return stage, derived, output


def read_command_log(path: Path, limit: int) -> bytes:
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode) or before.st_size > limit:
        raise ValueError("SDK command log is not a bounded regular file")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(path, flags), "rb") as stream:
        actual = os.fstat(stream.fileno())
        if (not stat.S_ISREG(actual.st_mode)
                or (actual.st_dev, actual.st_ino) != (before.st_dev, before.st_ino)):
            raise ValueError("SDK command log changed before read")
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError("SDK command log exceeds proof budget")
    return data


def run_command(command: list[str], stage: Path, prefix: Path, deadline: float,
                message: str, result: Path | None = None, cap: float | None = None,
                probe: bool = False) -> int:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise RuntimeError("public mobile-controls SDK deadline exceeded")
    try:
        code = _sdk.run_bounded(command, stage, prefix,
                                min(remaining, cap) if cap is not None else remaining)
    except RuntimeError:
        _sdk.print_command_log_tails(prefix)
        raise
    if not probe:
        _sdk.require_sdk_command_success(code, prefix, message, result)
    return code


def supports_attachment_export(help_text: str) -> bool:
    # Inspect the installed SDK's interface, not a guessed version or fallback.
    return (re.search(r"\bUSAGE:\s+xcresulttool export attachments\b", help_text) is not None
            and re.search(r"(?m)^\s+--path\s+<[^>]+>", help_text) is not None
            and re.search(r"(?m)^\s+--output-path\s+<[^>]+>", help_text) is not None)


def export_attachments(stage: Path, output: Path, result: Path, deadline: float) -> dict[str, int]:
    fields = dict(attachment_export_available=0, attachment_exported=0, exported_png_count=0)
    code = run_command(["xcrun", "xcresulttool", "export", "attachments", "--help"], stage,
                       output / "xcresult-attachments-help", deadline,
                       "mobile-controls attachment help failed", cap=15, probe=True)
    help_raw = b"\n".join(read_command_log(
        output / ("xcresult-attachments-help." + stream + ".log"), SUMMARY_LIMIT)
        for stream in ("stdout", "stderr"))
    with (output / "xcresult-attachments-help.txt").open("xb") as stream:
        stream.write(help_raw)
    if code or not supports_attachment_export(help_raw.decode("utf-8", "strict")):
        # The xcresult still retains keepAlways attachments. Report this explicit
        # extraction limitation; do not invent another SDK command or schema.
        return fields
    screenshots = output / "screenshots"
    screenshots.mkdir()
    run_command(["xcrun", "xcresulttool", "export", "attachments", "--path", str(result),
                 "--output-path", str(screenshots)], stage, output / "xcresult-attachments-export",
                deadline, "mobile-controls attachment export failed", cap=20)
    # Preserve the SDK-owned manifest verbatim. Only validate its JSON envelope;
    # no undocumented fields are interpreted as proof of a particular image.
    manifest = _sdk.strict_json(read_command_log(screenshots / "manifest.json", SUMMARY_LIMIT))
    if type(manifest) not in (dict, list):
        raise ValueError("unrecognized attachment manifest envelope")
    count = 0
    for index, path in enumerate(screenshots.iterdir()):
        if index >= 32 or not stat.S_ISREG(path.lstat().st_mode):
            raise ValueError("unrecognized or oversized attachment export layout")
        if path.name == "manifest.json":
            continue
        if path.suffix.lower() != ".png":
            raise ValueError("unexpected public mobile-controls attachment type")
        raw = read_command_log(path, 16_000_000)
        if len(raw) < 33 or raw[:8] != b"\x89PNG\r\n\x1a\n" or raw[12:16] != b"IHDR":
            raise ValueError("public mobile-controls attachment is not a PNG")
        count += 1
    if count != 2:
        raise ValueError("exactly two public mobile-controls PNG attachments required")
    return dict(attachment_export_available=1, attachment_exported=1, exported_png_count=count)


def success_report(attachments: dict[str, int]) -> dict[str, int]:
    return dict(schema_version=1, public_mobile_controls_passed=1, expected_cases=len(TESTS),
                started_cases=len(TESTS), passed_cases=len(TESTS), failed_cases=0, skipped_cases=0,
                xcresult_total_tests=len(TESTS), xcresult_passed_tests=len(TESTS),
                xcresult_failed_tests=0, xcresult_skipped_tests=0, xcresult_expected_failures=0,
                xcodebuild_exit_code=0, reused_build_products=1, build_performed=0,
                public_tests_only=1, game_executed=0, game_frame_rendered=0,
                physical_iphone_verified=0, private_guest_framework_verified=0, ready_to_play=0,
                ui_snapshot_test_passed=1, ui_snapshot_attachments_expected=2,
                visual_review_performed=0,
                external_test_deadline_seconds=EXTERNAL_DEADLINE_SECONDS, **attachments)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--built-output", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--destination", required=True)
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        raise RuntimeError("Apple SDK driver requires Darwin")
    stage, derived, output = prepare_paths(args.stage, args.built_output, args.output,
                                           args.destination)
    deadline = time.monotonic() + EXTERNAL_DEADLINE_SECONDS
    result = output / "MobileControls.xcresult"
    run_command(make_test_command(derived, args.destination, result), stage,
                output / "xcodebuild", deadline,
                "public mobile-controls XCTest failed or result bundle missing", result)
    # Both streams belong to this run. Extra case/failure events in either refuse.
    console = b"\n".join(read_command_log(output / ("xcodebuild." + stream + ".log"),
                                          CONSOLE_LIMIT) for stream in ("stdout", "stderr"))
    validate_console(console.decode("utf-8", "strict"))
    run_command(["xcrun", "xcresulttool", "get", "test-results", "summary", "--path", str(result)],
                stage, output / "xcresult-summary", deadline,
                "mobile-controls xcresult summary failed", cap=15)
    raw = read_command_log(output / "xcresult-summary.stdout.log", SUMMARY_LIMIT)
    with (output / "xcresult-summary.json").open("xb") as stream:
        stream.write(raw)
    validate_summary(_sdk.strict_json(raw))
    attachments = export_attachments(stage, output, result, deadline)
    if time.monotonic() >= deadline:
        raise RuntimeError("public mobile-controls SDK deadline exceeded")
    report = success_report(attachments)
    with (output / "public-mobile-controls-result.json").open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
