"""Public device-feature SDK checks using existing Simulator build products only."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys
import time

_spec = importlib.util.spec_from_file_location(
    "owned_host_existing_mobile_driver", Path(__file__).with_name("run_mobile_controls_tests.py"))
_mobile = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mobile)
_sdk = _mobile._sdk

GRAPHICS_TESTS = ("testPublicOffscreenMetalPresentation", "testPublicOffscreenSampledMetalPresentation")
SETTINGS_TESTS = (
    "testPresetsProduceBoundedNativeTuplesAndKeepOriginalDefault",
    "testInvalidPersistedValuesCannotRequestUnboundedAllocationOrRate",
    "testNativeStructLayoutsMatchCContracts",
    "testAudioCommandWorkerCoalescesAndHonorsLatestIntent",
    "testLocalPreferencesRoundTripAndRefuseUnknownOrOversizedData",
    "testPublicSettingsScreensRenderWithoutStartingGuest",
)
AUDIO_TESTS = (
    "testAudioStatusLayoutAndValidatedControls",
    "testAudioStartStopUsesPlaybackAndReportsActualHardware",
    "testAudioInterruptionRequiresResumePermissionAndUserIntent",
    "testAudioOutputDisconnectRequiresExplicitStart",
    "testAudioMediaServicesResetRequiresExplicitStart",
    "testAudioPullIsPlanarSilenceWithoutClients",
)
GROUPS = {
    # Original 15s presenter + 25s constants + new 45s pass-plan watchdogs.
    "graphics": ("GraphicsPresentationTests", GRAPHICS_TESTS, 90),
    "settings": ("DeviceSettingsTests", SETTINGS_TESTS, 40),
    "audio": ("OwnedHostAudioTests", AUDIO_TESTS, 30),
}
GROUP_DEADLINE_SECONDS = 120
TOTAL_DEADLINE_SECONDS = 360
GRAPHICS_PREFIX = "PUBLIC_GRAPHICS_PRESENTATION "
AUDIO_PREFIX = "OWNED_HOST_AUDIO_RESULT "


def validate_console(group: str, text: str) -> None:
    cls, tests, _ = GROUPS[group]
    if not tests:
        raise ValueError("public feature test contract is not frozen")
    if re.search(r"Modifying properties of a view[’']s layer off the main thread", text):
        raise ValueError("UIKit layer mutation off the main thread")
    cases = {f"-[OwnedGameHostTests.{cls} {name}]" for name in tests}
    events = re.findall(r"(?m)^Test Case '([^'\r\n]+)' (started|passed|failed|skipped)\b", text)
    if len(events) != 2 * len(tests):
        raise ValueError("public feature start/pass count differs")
    seen = set()
    for index in range(0, len(events), 2):
        case, event = events[index]
        if case not in cases or case in seen or event != "started" or events[index + 1] != (case, "passed"):
            raise ValueError("public feature cases must each start and pass once, serially")
        seen.add(case)
    if seen != cases:
        raise ValueError("public feature selectors differ")
    terminals = [line.strip() for line in text.splitlines()
                 if line.strip().startswith("** TEST ") and line.strip().endswith(" **")]
    if terminals != ["** TEST EXECUTE SUCCEEDED **"]:
        raise ValueError("single test-without-building terminal required")
    totals = re.findall(r"Executed (\d+) tests?, with (\d+) failures? \((\d+) unexpected\)", text)
    if not totals or any(row != (str(len(tests)), "0", "0") for row in totals):
        raise ValueError("public feature aggregate counts differ")


def validate_summary(group: str, summary) -> None:
    if type(summary) is not dict:
        raise ValueError("feature xcresult summary must be an object")
    expected = dict(totalTestCount=len(GROUPS[group][1]), passedTests=len(GROUPS[group][1]),
                    failedTests=0, skippedTests=0, expectedFailures=0)
    for key, value in expected.items():
        if type(summary.get(key)) is not int or summary[key] != value:
            raise ValueError("feature xcresult count differs: " + key)


def validate_marker(group: str, text: str) -> dict | None:
    if group == "graphics":
        prefix, other = GRAPHICS_PREFIX, AUDIO_PREFIX
        expected = dict(schema=1, backend=5, checks=255, rendered_cases=6, offscreen=True,
                        displayed=False, game=False, iphone=False)
    elif group == "audio":
        prefix, other = AUDIO_PREFIX, GRAPHICS_PREFIX
        expected = dict(schema=1, audio_version=1, transport_bytes=160, native_status_bytes=256,
                        source_rate_hz=48000, source_channels=2, client_capacity=8,
                        queue_capacity_frames=8192, profiles=3, authored_only=True,
                        game_audio_verified=False, audible_output_verified=False, physical_iphone_verified=False)
    else:
        if GRAPHICS_PREFIX.strip() in text or AUDIO_PREFIX.strip() in text:
            raise ValueError("unexpected native feature marker in settings-only class")
        return None
    lines = [line for line in text.splitlines() if prefix.strip() in line]
    if len(lines) != 1 or not lines[0].startswith(prefix) or other.strip() in text:
        raise ValueError("exactly one matching public feature marker required")
    value = _sdk.strict_json(lines[0][len(prefix):])
    if type(value) is not dict or set(value) != set(expected):
        raise ValueError("public feature marker schema differs")
    for key, wanted in expected.items():
        if type(value[key]) is not type(wanted) or value[key] != wanted:
            raise ValueError("public feature marker differs: " + key)
    return value


def validate_frame_marker(text: str, sampled: bool = False) -> dict:
    prefix = "PUBLIC_SAMPLED_FRAME_COMPARISON_RESULT " if sampled else "PUBLIC_FRAME_COMPARISON_RESULT "
    lines = [line for line in text.splitlines() if prefix.strip() in line]
    if len(lines) != 1 or not lines[0].startswith(prefix):
        raise ValueError("one complete frame comparison marker required")
    value = _sdk.strict_json(lines[0][len(prefix):])
    expected = dict(schema=1, cases=15 if sampled else 9, renders=60 if sampled else 36, compared_bytes=245760 if sampled else 147456, flags=7,
                    offscreen=True, game=False, iphone=False, performance_measured=False)
    if type(value) is not dict or set(value) != set(expected):
        raise ValueError("frame comparison schema differs")
    if any(type(value[k]) is not type(v) or value[k] != v for k, v in expected.items()):
        raise ValueError("frame comparison incomplete or outside public scope")
    return value


def make_test_command(group: str, derived: Path, destination: str, result: Path) -> list[str]:
    cls, tests, allowance = GROUPS[group]
    if not tests:
        raise ValueError("public feature test contract is not frozen")
    return ["xcodebuild", "-project", "OwnedGameHost.xcodeproj", "-scheme", "OwnedGameHost",
            "-configuration", "Debug", "-destination", destination,
            "-derivedDataPath", str(derived), "-resultBundlePath", str(result),
            "-parallel-testing-enabled", "NO", "-test-timeouts-enabled", "YES",
            "-default-test-execution-time-allowance", str(allowance),
            "-maximum-test-execution-time-allowance", str(allowance),
            *(f"-only-testing:OwnedGameHostTests/{cls}/{name}" for name in tests),
            "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO", "test-without-building"]


def run_group(group: str, stage: Path, derived: Path, output: Path, destination: str) -> dict:
    evidence = output / group
    evidence.mkdir()
    deadline = time.monotonic() + GROUP_DEADLINE_SECONDS
    result = evidence / (GROUPS[group][0] + ".xcresult")
    _mobile.run_command(make_test_command(group, derived, destination, result), stage,
                        evidence / "xcodebuild", deadline, "public device-feature XCTest failed", result)
    console = b"\n".join(_mobile.read_command_log(evidence / ("xcodebuild." + stream + ".log"),
                                                  _mobile.CONSOLE_LIMIT) for stream in ("stdout", "stderr"))
    text = console.decode("utf-8", "strict")
    validate_console(group, text)
    marker = validate_marker(group, text)
    frame_marker = validate_frame_marker(text) if group == "graphics" else None
    sampled_marker = validate_frame_marker(text, sampled=True) if group == "graphics" else None
    _mobile.run_command(["xcrun", "xcresulttool", "get", "test-results", "summary", "--path", str(result)],
                        stage, evidence / "xcresult-summary", deadline, "public feature summary failed", cap=15)
    raw = _mobile.read_command_log(evidence / "xcresult-summary.stdout.log", _mobile.SUMMARY_LIMIT)
    validate_summary(group, _sdk.strict_json(raw))
    with (evidence / "xcresult-summary.json").open("xb") as stream:
        stream.write(raw)
    attachments = (_mobile.export_attachments(stage, evidence, result, deadline) if group == "settings"
                   else dict(attachment_export_available=0, attachment_exported=0, exported_png_count=0))
    if time.monotonic() >= deadline:
        raise RuntimeError("public feature deadline exceeded")
    count = len(GROUPS[group][1])
    report = dict(schema_version=1, public_feature_group=group, expected_cases=count,
                  started_cases=count, passed_cases=count, failed_cases=0, skipped_cases=0,
                  xcresult_total_tests=count, xcresult_passed_tests=count, xcresult_failed_tests=0,
                  xcresult_skipped_tests=0, xcresult_expected_failures=0,
                  build_performed=0, reused_build_products=1, public_tests_only=1,
                  game_executed=0, game_frame_rendered=0, physical_iphone_verified=0,
                  private_guest_framework_verified=0, ready_to_play=0,
                  visual_review_performed=0, external_test_deadline_seconds=GROUP_DEADLINE_SECONDS, **attachments)
    if marker is not None:
        report["native_public_probe"] = marker
    if frame_marker is not None:
        report["native_frame_comparison"] = frame_marker
    if sampled_marker is not None:
        report["native_sampled_frame_comparison"] = sampled_marker
    with (evidence / "public-feature-result.json").open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2); stream.write("\n")
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("stage", "built-output", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--destination", required=True)
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        raise RuntimeError("device feature SDK driver requires Darwin")
    if not AUDIO_TESTS:
        raise ValueError("audio public test contract has not been frozen")
    stage, derived, output = _mobile.prepare_paths(args.stage, args.built_output, args.output, args.destination)
    deadline = time.monotonic() + TOTAL_DEADLINE_SECONDS
    reports = {group: run_group(group, stage, derived, output, args.destination) for group in GROUPS}
    if time.monotonic() >= deadline:
        raise RuntimeError("public feature total deadline exceeded")
    report = dict(schema_version=1, public_device_features_passed=1, groups=reports,
                  expected_cases=sum(len(group[1]) for group in GROUPS.values()),
                  build_performed=0, reused_build_products=1, public_tests_only=1,
                  game_executed=0, game_frame_rendered=0, physical_iphone_verified=0,
                  private_guest_framework_verified=0, ready_to_play=0,
                  external_total_deadline_seconds=TOTAL_DEADLINE_SECONDS)
    with (output / "public-device-features-result.json").open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2); stream.write("\n")
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
