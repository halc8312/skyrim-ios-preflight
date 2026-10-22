"""Bounded driver for the separate, public-only Apple resident XCTest app."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

TEST = "testResidentRustKernelWithPublicFixture"
PREFIX = "RESIDENT_APPLE_PUBLIC_RESULT "


def run_bounded(command, cwd: Path, prefix: Path, timeout: float) -> int:
    """Preserve logs exclusively; terminate the command's whole process group."""
    with prefix.with_suffix(".stdout.log").open("xb") as stdout:
        with prefix.with_suffix(".stderr.log").open("xb") as stderr:
            process = subprocess.Popen(command, cwd=cwd, stdout=stdout, stderr=stderr,
                                       stdin=subprocess.DEVNULL, start_new_session=True)
            try:
                return process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait(timeout=3)
                raise RuntimeError("public SDK command deadline exceeded") from None


def validate_result(text: str) -> dict:
    case = r"Test Case '-\[ResidentAppleSDKProbeTests\.ResidentAppleProbeTests " + TEST + r"\]' "
    cases = re.findall(case + "passed", text)
    starts = re.findall(case + "started", text)
    failures = re.findall(case + "failed", text)
    terminals = [line.strip() for line in text.splitlines()
                 if line.strip().startswith("** TEST ") and line.strip().endswith(" **")]
    if (len(cases) != 1 or len(starts) != 1 or failures
            or terminals != ["** TEST EXECUTE SUCCEEDED **"]):
        raise ValueError("exactly one expected XCTest pass was not proven")
    reports = re.findall(re.escape(PREFIX) + r"(\{[^\r\n]*\})", text)
    if len(reports) != 1:
        raise ValueError("one structured public outcome was not proven")
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError("duplicate public result field")
            value[key] = item
        return value
    report = json.loads(reports[0], object_pairs_hook=unique)
    expected = dict(kind="public_apple_resident_xctest", passed=True, checked_flags=0x3fff,
                    frequency=50_000_000, cleanup_count=1, dispatched_imports=2,
                    exit_code=0, function_count=3, inner_call_deadline_seconds=15,
                    xctest_allowance_seconds=20, fresh_dedicated_app_process=True,
                    physical_iphone_verified=False, game_executed=False, ready_to_play=False,
                    compatibility_verified=False, mapped_inode_attested=False,
                    signature_authority_authenticated=False,
                    private_guest_framework_verified=False, loader_integration_allowed=False)
    if set(report) != set(expected) | {"independent_post_sign_sha256"}:
        raise ValueError("public result schema differs")
    if any(type(report[key]) is not type(value) or report[key] != value for key, value in expected.items()):
        raise ValueError("public result contract differs")
    if not isinstance(report["independent_post_sign_sha256"], str) or not re.fullmatch(
            "[0-9a-f]{64}", report["independent_post_sign_sha256"]):
        raise ValueError("trusted full hash shape differs")
    return report


def verify_package(app: Path, trusted: bytes, dependencies: str) -> None:
    candidate = app / "Frameworks/ResidentAppleFixture.framework/ResidentAppleFixture"
    if hashlib.sha256(candidate.read_bytes()).hexdigest().encode() + b"\n" != trusted:
        raise ValueError("embedded signed public binary changed during packaging")
    if (app / "ResidentAppleFixture.signed-sha256.txt").read_bytes() != trusted:
        raise ValueError("packaged trusted hash resource changed")
    if "ResidentAppleFixture" in dependencies:
        raise ValueError("fixture was linked for early loading rather than embedded only")


def make_test_command(common: list[str], signing: list[str], result: Path) -> list[str]:
    # Xcode's normal test action runs once. Its explicit iteration option is
    # reserved for counts above one; no retry/iteration mode is enabled here.
    return common + ["-resultBundlePath", str(result),
                     "-parallel-testing-enabled", "NO",
                     "-test-timeouts-enabled", "YES", "-default-test-execution-time-allowance", "20",
                     "-maximum-test-execution-time-allowance", "20", "-only-testing:" +
                     "ResidentAppleSDKProbeTests/ResidentAppleProbeTests/" + TEST,
                     *signing, "test-without-building"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--destination", required=True)
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise RuntimeError("Apple SDK runner requires Darwin; pure unit tests are portable")
    stage = args.stage.resolve(strict=True)
    if not re.fullmatch(r"platform=iOS Simulator,id=[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}", args.destination):
        raise ValueError("this driver runs only one explicitly selected Simulator")
    args.output.mkdir()  # Refuse reuse before either tool can write an artifact.
    output = args.output.resolve(strict=True)
    expected = stage / "Resources/ResidentAppleFixture.signed-sha256.txt"
    trusted = expected.read_bytes()
    if not re.fullmatch(b"[0-9a-f]{64}\n", trusted):
        raise ValueError("separate post-sign SHA resource is invalid")
    status = run_bounded(["xcodegen", "generate", "--spec", "project.yml"], stage,
                         output / "xcodegen", 30)
    if status:
        raise RuntimeError("isolated project generation failed")
    deadline = time.monotonic() + 180
    result = output / "ResidentAppleSDKProbe.xcresult"
    common = ["xcodebuild", "-project", "ResidentAppleSDKProbe.xcodeproj", "-scheme",
               "ResidentAppleSDKProbe", "-configuration", "Debug", "-destination", args.destination,
               "-derivedDataPath", str(output / "DerivedData")]
    signing = ["CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO"]
    status = run_bounded(common + signing + ["build-for-testing"], stage,
                         output / "build-for-testing", max(0.01, deadline - time.monotonic()))
    if status != 0:
        raise RuntimeError("isolated SDK build-for-testing failed")
    app = output / "DerivedData/Build/Products/Debug-iphonesimulator/ResidentAppleSDKProbe.app"
    executable = app / "ResidentAppleSDKProbe"
    inspect = subprocess.run(["xcrun", "otool", "-L", str(executable)], capture_output=True,
                             check=True, text=True, timeout=max(0.01, min(15, deadline - time.monotonic())))
    verify_package(app, trusted, inspect.stdout)
    command = make_test_command(common, signing, result)
    status = run_bounded(command, stage, output / "xcodebuild", max(0.01, deadline - time.monotonic()))
    if status != 0 or not result.is_dir():
        raise RuntimeError("isolated XCTest failed or result bundle missing")
    report = validate_result((output / "xcodebuild.stdout.log").read_text())
    if report["independent_post_sign_sha256"] != trusted[:64].decode("ascii"):
        raise ValueError("XCTest did not use the independently supplied full hash")
    with (output / "resident-xctest-result.json").open("x") as stream:
        json.dump(dict(report, xcodebuild_exit_code=0, external_build_test_deadline_seconds=180),
                  stream, indent=2)
    print(json.dumps(dict(public_resident_xctests_passed=1, game_executed=False,
                          private_guest_framework_verified=False, physical_iphone_verified=False)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
