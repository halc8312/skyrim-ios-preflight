"""One fresh public V2 fullchain process; does not replace any V1/audio gate."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
import time

HERE = Path(__file__).resolve().parent
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
sdk = load("public_v2_existing_sdk", HERE / "run_sdk_tests.py")
mobile = load("public_v2_log_helpers", HERE / "run_mobile_controls_tests.py")
TEST = "testPublicOwnedHostV2FullChain"
CASE = "-[OwnedGameHostTests.OwnedHostV2FullChainTests " + TEST + "]"
PREFIX = "OWNED_HOST_PUBLIC_V2_RESULT "
BUNDLE = "com.halc8312.ownedgamehost.v2fixture"
PROOF = dict(version=0x10000, bytes=64, abi_version=0x20000, layout_words=51,
             host_bytes=416, requirements_bytes=216, provider_refusals=10,
             provider_preserved=1, frequency=50_000_000, cleanup_count=1,
             jump_result=0x1111, normal_result=0x2222, mmio_result=1,
             mmio_lock_word=0x80000001, imports_once=2, republish_ok=1)

def exact(actual, expected):
    if type(actual) is not type(expected):
        return False
    if type(expected) is dict:
        return set(actual) == set(expected) and all(exact(actual[k], v) for k, v in expected.items())
    return actual == expected

def validate_console(text):
    if re.search(r"Modifying properties of a view[’']s layer off the main thread", text):
        raise ValueError("UIKit layer thread violation")
    events = re.findall(r"(?m)^Test Case '([^'\r\n]+)' (started|passed|failed|skipped)\b", text)
    if events != [(CASE, "started"), (CASE, "passed")]:
        raise ValueError("exactly one V2 start/pass pair required")
    terminals = [line.strip() for line in text.splitlines()
                 if line.strip().startswith("** TEST ") and line.strip().endswith(" **")]
    totals = re.findall(r"Executed (\d+) tests?, with (\d+) failures? \((\d+) unexpected\)", text)
    if terminals != ["** TEST EXECUTE SUCCEEDED **"] or not totals or any(t != ("1", "0", "0") for t in totals):
        raise ValueError("V2 test terminal/count mismatch")

def validate_result(text, hashes):
    validate_console(text)
    report = sdk.one_report(text, PREFIX)
    expected = dict(kind="public_owned_host_v2_fullchain_xctest", passed=True, state=5,
                    policy=0, exit_code=0, function_imports=2, registered_functions=2,
                    function_count=3, renderer_initialized=1, gpu_errors=0, decoded_frames=0,
                    separate_fresh_process=True, public_fixture_only=True,
                    game_executed=False, physical_iphone_verified=False, ready_to_play=False,
                    signature_authority_authenticated=False, private_guest_framework_verified=False,
                    game_frame_rendered=False, mapped_inode_attested=False,
                    main_thread_attach_rejected=True, metal_layer_required_setters_on_main=True,
                    metal_layer_off_main_calls=0, v2_proof=PROOF,
                    audio_output_verified=False, xma_pcm_verified=False, **hashes)
    if not exact(report, expected):
        raise ValueError("V2 public result differs from exact typed contract")
    return report

def validate_summary(summary):
    if type(summary) is not dict:
        raise ValueError("V2 xcresult summary must be an object")
    for key, value in dict(totalTestCount=1, passedTests=1, failedTests=0,
                           skippedTests=0, expectedFailures=0).items():
        if type(summary.get(key)) is not int or summary[key] != value:
            raise ValueError("V2 xcresult count mismatch: " + key)

def make_test_command(common, signing, result):
    return common + ["-resultBundlePath", str(result), "-parallel-testing-enabled", "NO",
                     "-test-timeouts-enabled", "YES", "-default-test-execution-time-allowance", "40",
                     "-maximum-test-execution-time-allowance", "40",
                     "-only-testing:OwnedGameHostTests/OwnedHostV2FullChainTests/" + TEST,
                     *signing, "test-without-building"]

def validate_stage(stage):
    project = mobile.read_command_log(stage / "project.yml", 128_000).decode("utf-8").replace("\r\n", "\n")
    if (project.count("PRODUCT_BUNDLE_IDENTIFIER: " + BUNDLE + "\n") != 1
            or project.count("PUBLIC_V2_SDK_FIXTURE") != 1
            or "PRODUCT_BUNDLE_IDENTIFIER: com.halc8312.ownedgamehost\n" in project):
        raise ValueError("fresh V2 app identity/compile condition required")
    binding = mobile.read_command_log(stage / "Resources/OwnedGuestBinding.json", 16384)
    value = sdk.strict_json(binding)
    if type(value) is not dict or type(value.get("abi_version")) is not int or value["abi_version"] != 0x20000:
        raise ValueError("explicit V2 binding required")
    xex = mobile.read_command_log(stage / "Resources/PublicFixture/default.xex", 1_000_000)
    if not 24 <= len(xex) or hashlib.sha256(xex).hexdigest() != value.get("xex_sha256"):
        raise ValueError("authored XEX hash mismatch")
    hashes = dict(unsigned_fixture_sha256=value.get("unsigned_framework_sha256"),
                  public_xex_sha256=value.get("xex_sha256"),
                  trusted_binding_sha256=hashlib.sha256(binding).hexdigest())
    if any(type(v) is not str or not re.fullmatch("[0-9a-f]{64}", v) for v in hashes.values()):
        raise ValueError("independent hash spelling differs")
    return binding, xex, hashes

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--destination", required=True)
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        raise RuntimeError("V2 Apple SDK driver requires Darwin")
    if not re.fullmatch(mobile.DESTINATION, args.destination):
        raise ValueError("one explicit Simulator destination required")
    stage = args.stage.resolve(strict=True)
    output = args.output.resolve()
    if output.is_relative_to(stage) or stage.is_relative_to(output):
        raise ValueError("V2 evidence must be outside the source stage")
    binding, xex, hashes = validate_stage(stage)
    output.mkdir()
    fixture_sha = hashlib.sha256(mobile.read_command_log(
        stage / "Frameworks/PrivateGuest.framework/PrivateGuest", 16 * 1024 * 1024)).hexdigest()
    wgpu_sha = hashlib.sha256(mobile.read_command_log(
        stage / "Frameworks/WgpuNative.framework/WgpuNative", 128 * 1024 * 1024)).hexdigest()
    deadline = time.monotonic() + 300
    def run(command, name, cap=None, result=None):
        return mobile.run_command(command, stage, output / name, deadline,
                                  "public V2 SDK command failed: " + name, cap=cap, result=result)
    run(["xcodegen", "generate", "--spec", "project.yml"], "xcodegen", 30)
    common = ["xcodebuild", "-project", "OwnedGameHost.xcodeproj", "-scheme", "OwnedGameHost",
              "-configuration", "Debug", "-destination", args.destination,
              "-derivedDataPath", str(output / "DerivedData")]
    signing = ["CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO"]
    run(common + signing + ["build-for-testing"], "build-for-testing")
    app = output / "DerivedData/Build/Products/Debug-iphonesimulator/OwnedGameHost.app"
    run(["xcrun", "otool", "-L", str(app / "OwnedGameHost")], "app-system-loads", 15)
    dependencies = mobile.read_command_log(output / "app-system-loads.stdout.log", 128_000).decode()
    sdk.verify_package(app, binding, xex, fixture_sha, wgpu_sha, dependencies)
    import plistlib
    if plistlib.loads(mobile.read_command_log(app / "Info.plist", 128_000)).get("CFBundleIdentifier") != BUNDLE:
        raise ValueError("built app is not the separate V2 bundle")
    result = output / "OwnedGameHostV2.xcresult"
    run(make_test_command(common, signing, result), "xcodebuild", result=result)
    for command in ("summary", "tests"):
        run(["xcrun", "xcresulttool", "help", "get", "test-results", command], "xcresult-" + command + "-help", 15)
        run(["xcrun", "xcresulttool", "get", "test-results", command, "--path", str(result)], "xcresult-" + command, 15)
        raw = mobile.read_command_log(output / ("xcresult-" + command + ".stdout.log"), 2_000_000)
        (output / ("xcresult-" + command + ".json")).write_bytes(raw)
        parsed = sdk.strict_json(raw)
        if command == "summary":
            validate_summary(parsed)
    report = validate_result(mobile.read_command_log(output / "xcodebuild.stdout.log", 32_000_000).decode(), hashes)
    report = dict(report, xcodebuild_exit_code=0, external_build_test_deadline_seconds=300)
    with (output / "public-v2-fullchain-result.json").open("x") as stream:
        json.dump(report, stream, indent=2)
    print(json.dumps(dict(public_owned_host_v2_fullchain_passed=1, game_executed=False,
                          physical_iphone_verified=False, audio_output_verified=False,
                          private_guest_framework_verified=False)))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
