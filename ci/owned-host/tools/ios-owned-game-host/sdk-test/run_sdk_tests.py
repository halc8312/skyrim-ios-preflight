"""Build/test one fresh public-only app with two cases; accepts no game input."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import time

TEST = "testPublicOwnedHostFullChain"
CASE = "-[OwnedGameHostTests.OwnedHostFullChainTests " + TEST + "]"
PREFIX = "OWNED_HOST_PUBLIC_RESULT "
TRACE_TEST = "testDarwinUnwindAcrossPublicCppFrames"
TRACE_CASE = "-[OwnedGameHostTests.NativeTraceSDKProbeTests " + TRACE_TEST + "]"
TRACE_PREFIX = "OWNED_HOST_NATIVE_TRACE_RESULT "


def run_bounded(command, cwd: Path, prefix: Path, timeout: float) -> int:
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
                raise RuntimeError("public owned-host SDK command deadline exceeded") from None



LOG_TAIL_BYTES = 8192


def print_command_log_tails(prefix: Path) -> None:
    """Read only the two logs this SDK command created, never scan a directory."""
    for stream_name in ("stdout", "stderr"):
        path = prefix.with_suffix("." + stream_name + ".log")
        text = None
        truncated = False
        try:
            before = path.lstat()
            if not stat.S_ISREG(before.st_mode):
                raise OSError("not an owned regular command log")
            flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
            with os.fdopen(os.open(path, flags), "rb") as stream:
                actual = os.fstat(stream.fileno())
                if (not stat.S_ISREG(actual.st_mode)
                        or (actual.st_dev, actual.st_ino) != (before.st_dev, before.st_ino)):
                    raise OSError("command log changed before read")
                truncated = actual.st_size > LOG_TAIL_BYTES
                stream.seek(max(0, actual.st_size - LOG_TAIL_BYTES))
                raw = stream.read(LOG_TAIL_BYTES)
            rendered = raw.decode("utf-8", "replace").encode("utf-8")
            truncated = truncated or len(rendered) > LOG_TAIL_BYTES
            text = rendered[-LOG_TAIL_BYTES:].decode("utf-8", "ignore")
            # Keep line breaks but prevent terminal control sequences from logs.
            text = "".join(c if c in "\n\r\t" or (ord(c) >= 32 and ord(c) != 127)
                           else "?" for c in text)
        except OSError:
            pass
        print("PUBLIC_SDK_COMMAND_LOG_TAIL " + json.dumps(dict(
            stream=stream_name, available=text is not None, truncated=truncated,
            utf8_bytes=len(text.encode("utf-8")) if text is not None else 0)))
        if text is not None:
            print(text)
        print("PUBLIC_SDK_COMMAND_LOG_TAIL_END " + stream_name)


def require_sdk_command_success(code: int, prefix: Path, message: str,
                                result: Path | None = None) -> None:
    if code or (result is not None and not result.is_dir()):
        print_command_log_tails(prefix)
        raise RuntimeError(message)


def strict_json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON field")
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonfinite JSON")))


def validate_console(text: str) -> None:
    events = re.findall(r"(?m)^Test Case '([^'\r\n]+)' (started|passed|failed|skipped)\b", text)
    fullchain = [(CASE, "started"), (CASE, "passed")]
    trace = [(TRACE_CASE, "started"), (TRACE_CASE, "passed")]
    if events not in (fullchain + trace, trace + fullchain):
        raise ValueError("exactly two expected serial case start/pass pairs were not proven")
    terminals = [line.strip() for line in text.splitlines()
                 if line.strip().startswith("** TEST ") and line.strip().endswith(" **")]
    if terminals != ["** TEST EXECUTE SUCCEEDED **"]:
        raise ValueError("single test-without-building success marker missing")
    totals = re.findall(r"Executed (\d+) tests?, with (\d+) failures? \((\d+) unexpected\)", text)
    # Each test class can have its own one-case suite summary. The enclosing
    # suites must finish with the two-case aggregate, with no failure anywhere.
    if (not totals or totals[-1] != ("2", "0", "0")
            or any(row not in (("1", "0", "0"), ("2", "0", "0")) for row in totals)):
        raise ValueError("console totals differ from two cases without failures")


def one_report(text: str, prefix: str) -> dict:
    records = re.findall(re.escape(prefix) + r"([^\r\n]*)", text)
    if len(records) != 1:
        raise ValueError("one public structured outcome was not proven")
    result = strict_json(records[0])
    if type(result) is not dict:
        raise ValueError("public structured outcome is not an object")
    return result


def validate_result(text: str, hashes: dict[str, str]) -> dict:
    validate_console(text)
    result = one_report(text, PREFIX)
    expected = dict(kind="public_owned_host_fullchain_xctest", passed=True, state=5,
                    policy=0, exit_code=0, function_imports=2, registered_functions=2,
                    function_count=3, renderer_initialized=1, gpu_errors=0, decoded_frames=0,
                    separate_fresh_process=True, public_fixture_only=True,
                    game_executed=False, physical_iphone_verified=False, ready_to_play=False,
                    signature_authority_authenticated=False, private_guest_framework_verified=False,
                    game_frame_rendered=False, mapped_inode_attested=False, **hashes)
    if set(result) != set(expected) or any(type(result[k]) is not type(v) or result[k] != v
                                           for k, v in expected.items()):
        raise ValueError("public fullchain outcome differs from fixed contract")
    return result


def validate_native_trace_result(text: str) -> dict:
    validate_console(text)
    result = one_report(text, TRACE_PREFIX)
    expected = dict(abi_version=1, struct_size=32, expected_parent_count=2,
                    matched_parent_count=2, rx_parent_count=2, flags=15,
                    reserved=0, passed=True, public_cpp_only=True,
                    private_guest_fatal_path_verified=False)
    if (set(result) != set(expected) | {"captured_count"}
            or any(type(result[k]) is not type(v) or result[k] != v
                   for k, v in expected.items())
            or type(result["captured_count"]) is not int
            or not 3 <= result["captured_count"] <= 32):
        raise ValueError("public native trace outcome differs from fixed contract")
    return result


def validate_summary(summary: dict) -> None:
    # Apple's installed xcresulttool supplies these counts independently of
    # console parsing. Unknown/missing output schemas fail closed and are saved.
    for key, value in dict(totalTestCount=2, passedTests=2, failedTests=0,
                           skippedTests=0, expectedFailures=0).items():
        if type(summary.get(key)) is not int or summary[key] != value:
            raise ValueError("xcresult summary count differs: " + key)


def verify_package(app: Path, binding: bytes, xex: bytes, fixture_sha: str,
                   wgpu_sha: str, dependencies: str) -> None:
    if (app / "OwnedGuestBinding.json").read_bytes() != binding:
        raise ValueError("packaged independent binding changed")
    if (app / "PublicFixture/default.xex").read_bytes() != xex:
        raise ValueError("packaged self-authored XEX changed")
    for name, expected in [("PrivateGuest", fixture_sha), ("WgpuNative", wgpu_sha)]:
        binary = app / f"Frameworks/{name}.framework/{name}"
        if hashlib.sha256(binary.read_bytes()).hexdigest() != expected:
            raise ValueError("framework changed during app packaging")
    if "PrivateGuest" in dependencies or len(re.findall(
            r"(?m)^\s+@rpath/WgpuNative\.framework/WgpuNative\s+\(", dependencies)) != 1:
        raise ValueError("unexpected eager private load or missing WebGPU dependency")


def make_test_command(common, signing, result):
    return common + ["-resultBundlePath", str(result), "-parallel-testing-enabled", "NO",
                     "-test-timeouts-enabled", "YES", "-default-test-execution-time-allowance", "40",
                     "-maximum-test-execution-time-allowance", "40",
                     "-only-testing:OwnedGameHostTests/OwnedHostFullChainTests/" + TEST,
                     "-only-testing:OwnedGameHostTests/NativeTraceSDKProbeTests/" + TRACE_TEST,
                     *signing, "test-without-building"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--destination", required=True)
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise RuntimeError("Apple SDK driver requires Darwin")
    stage = args.stage.resolve(strict=True)
    if not re.fullmatch(r"platform=iOS Simulator,id=[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}", args.destination):
        raise ValueError("explicit single Simulator destination required")
    args.output.mkdir()
    output = args.output.resolve(strict=True)
    binding = (stage / "Resources/OwnedGuestBinding.json").read_bytes()
    if not 0 < len(binding) <= 16384:
        raise ValueError("bounded independent binding required")
    expected = strict_json(binding)
    xex = (stage / "Resources/PublicFixture/default.xex").read_bytes()
    if not 24 <= len(xex) <= 1_000_000 or hashlib.sha256(xex).hexdigest() != expected["xex_sha256"]:
        raise ValueError("self-authored public XEX does not match independent binding")
    hashes = dict(unsigned_fixture_sha256=expected["unsigned_framework_sha256"],
                  public_xex_sha256=expected["xex_sha256"],
                  trusted_binding_sha256=hashlib.sha256(binding).hexdigest())
    if any(not isinstance(v, str) or not re.fullmatch("[0-9a-f]{64}", v) for v in hashes.values()):
        raise ValueError("independent hash spelling differs")
    fixture_sha = hashlib.sha256((stage / "Frameworks/PrivateGuest.framework/PrivateGuest").read_bytes()).hexdigest()
    wgpu_sha = hashlib.sha256((stage / "Frameworks/WgpuNative.framework/WgpuNative").read_bytes()).hexdigest()
    if run_bounded(["xcodegen", "generate", "--spec", "project.yml"], stage, output / "xcodegen", 30):
        raise RuntimeError("project generation failed")
    deadline = time.monotonic() + 300
    common = ["xcodebuild", "-project", "OwnedGameHost.xcodeproj", "-scheme", "OwnedGameHost",
              "-configuration", "Debug", "-destination", args.destination,
              "-derivedDataPath", str(output / "DerivedData")]
    signing = ["CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO"]
    build_code = run_bounded(common + signing + ["build-for-testing"], stage,
                             output / "build-for-testing", max(.01, deadline - time.monotonic()))
    require_sdk_command_success(build_code, output / "build-for-testing",
                                "public app build-for-testing failed")
    app = output / "DerivedData/Build/Products/Debug-iphonesimulator/OwnedGameHost.app"
    dependencies = subprocess.check_output(["xcrun", "otool", "-L", str(app / "OwnedGameHost")],
                                           text=True, timeout=15)
    (output / "app-system-loads.txt").write_text(dependencies)
    verify_package(app, binding, xex, fixture_sha, wgpu_sha, dependencies)
    result = output / "OwnedGameHost.xcresult"
    code = run_bounded(make_test_command(common, signing, result), stage,
                       output / "xcodebuild", max(.01, deadline - time.monotonic()))
    require_sdk_command_success(code, output / "xcodebuild",
                                "public fullchain XCTest failed or result bundle missing", result)
    # Apple documents these commands and their SDK-owned help/output formats:
    # developer.apple.com/documentation/xcode-release-notes/xcode-16_3-release-notes
    for command in ("summary", "tests"):
        help_text = subprocess.check_output(["xcrun", "xcresulttool", "help", "get", "test-results", command],
                                            text=True, timeout=15)
        (output / f"xcresult-{command}-help.txt").write_text(help_text)
        raw = subprocess.check_output(["xcrun", "xcresulttool", "get", "test-results", command,
                                       "--path", str(result)], timeout=15)
        if len(raw) > 2_000_000:
            raise ValueError("xcresult JSON exceeds public proof budget")
        with (output / f"xcresult-{command}.json").open("xb") as stream:
            stream.write(raw)
        parsed = strict_json(raw)
        if command == "summary":
            validate_summary(parsed)
    console = (output / "xcodebuild.stdout.log").read_text()
    report = validate_result(console, hashes)
    native_trace_report = validate_native_trace_result(console)
    with (output / "public-fullchain-result.json").open("x") as stream:
        json.dump(dict(report, xcodebuild_exit_code=0, external_build_test_deadline_seconds=300), stream, indent=2)
    with (output / "native-trace-result.json").open("x") as stream:
        json.dump(dict(native_trace_report, xcodebuild_exit_code=0,
                       external_build_test_deadline_seconds=300), stream, indent=2)
    print(json.dumps(dict(public_owned_host_fullchain_passed=1, game_executed=False,
                          public_cpp_native_trace_passed=1,
                          physical_iphone_verified=False, private_guest_framework_verified=False)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
