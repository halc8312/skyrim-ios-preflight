"""Derive one manual public device build and Simulator UI evidence route."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import sys


ROOT = Path(__file__).resolve().parent
NOT_BEFORE = datetime(2026, 10, 23, 22, 30, tzinfo=timezone.utc)  # 2026-10-24 07:30 JST
SHA = re.compile(r"[0-9a-f]{40}\Z")
BOOTSTRAP_PINS = {
    "ci/prepare-fix-build-route/prepare_route.py": "a8c087b7cd8a0ea10925d226adbe813cca8a9b928ddd79c20cd2adeff8bfb7ce",
    "ci/prepare-fix-build-route/reference-pins.json": "6654eb167e3f7fc74dad2b6c7383d462811f70447bcafb99632d0df950edacb5",
    "ci/run-bounded-command.py": "f7391d02a43dc5dc97ea7f017cd5e502c70b7a4f06b4ce05a9e4972b49a890dd",
}
INPUT_PINS = {
    "sdk-test/tests/NativeTraceSDKProbeTests.swift": "5bfc259bf8d1ccec43386177785b0e691bb4ce5b9368669ded2ae76f8b2dbbf9",
    "project.yml": "d4f5102d619ae110bb28eafb10d9981afa089f58c3da8d59e54afc112ba01ddb",
    "native/MetalFrameProbe.h": "d0193c1c66017d3f8085ed61706b1991081354e160ffdacf4b14e59f27997b47",
    "native/MetalFrameProbe.mm": "8730fb9ca6002e4f11ff07aaccb56608299d6d46fb844de1d34dae600897097c",
    "native/TargetFrameLifetime.h": "fb46ab5555ca7f3fe7dbada25688405c500536e0424f5a6d43c494a2c0fb4c16",
    "native/TargetFramePacket.h": "59470df9899d133451f235527f9dd19c3422a61d46ecbb630bafdf63451c221c",
    "app/MobilePadState.swift": "8df06a806565cff42ab2bf4fe3d39b7de009149f190b02eefdcc5183c46ee2ff",
    "app/MobileControlsSettings.swift": "a16a9bb231b60a11c155321a1762ac34b1ae053238a0953014d9f95db53cafb8",
    "app/MobileControlsView.swift": "0de5944765e6d0e524f9d96096742f10786ce2b3996064414168071ce390ddc7",
    "sdk-test/tests/MobileControlsTests.swift": "9c1bd7b322b00e0faba222b56d6f2c28c9d8f2bc915be8364074e6b1ed082d40",
    "src/alpha_coverage_probe.rs": "b95c96f882ef3502f11668a353cb107e29835152dec6ed63e4dcb10b76cce678",
    "native/GraphicsPresentationProbe.h": "6e7f24e1cba0d370f32eaa6a7b3c902fbe7d084902207a8db027438d3b175104",
    "sdk-test/tests/GraphicsPresentationTests.swift": "a692c083d9d9121e2fe59e8fdd0141b4eb72aa870117cec48c060e189f665507",
}
KERNEL_PINS = {
    "lib.rs": "6751fbbdbabd662a2cc68c043b6c6a0280d6241d478c015d0274c0d69bdbdfcd",
    "dispatch.rs": "9747e76edf9cd3007dde77afeb8f6125c6b4151c8023dbe4c0c8ebe5e154e714",
    "dispatch_counter_tests.rs": "ce8e30b56ed4f25390a2583ab1c16087085763ff767b927353b5e6e8b66d3e1b",
}
UI_RUNNER_SHA = "060108bca734f34df2027c869eddb869b3d2f257bf139aaa7085c5ff8f04e393"
HARNESS = 'harness="$(cd "$(dirname "$0")/.." && pwd)"'
LOOP = "for variant in device simulator; do\n"
AFTER = "\n# The V2 image is independently compiled from public authored sources."
STAGE_CALL = 'bash "$harness/ci/run-ios-owned-host-tests.sh" "$work" "$simde"'
SELECTION = 'selection="$(python3 - <<\'PY\'\n'
SELECTION_END = "\naudio_watch_pid=\n"
GUI = "# Controlled environment comparison: preserve the same source, runtime, UDID\n"
GUI_END = 'python3 "$harness/ci/probe-host-audio.py" --phase post_gui'
METAL_STAGE_ANCHOR = '    "$host/native/GraphicsPresentationProbe.h" "$stage/native/"\n'
METAL_STAGE_GATE = '  # Explicit authored Metal probe sources; original source and loader gates remain.\n  cp "$host/native/ConstantReplayKernel.h" "$host/native/MetalConstantReplay.metal" \\\n    "$host/native/MetalConstantProbe.h" "$host/native/MetalConstantProbe.mm" \\\n    "$host/native/MetalConstantValidation.h" "$stage/native/"\n  cp "$host/native/FramePlanPacket.h" "$host/native/MetalFrameProbe.h" \\\n    "$host/native/MetalFrameProbe.mm" "$stage/native/"\n  mkdir -p "$stage/native/public-shaders"\n  cp "$host/native/public-shaders/PublicFrameShaders.h" "$stage/native/public-shaders/"\n  python3 "$host/native/compile_public_frame_shaders.py" "$variant" \\\n    "$host/native/public-shaders" "$stage/native/public-frame-libraries" \\\n    > "$output/$variant-public-xenos-shaders.log" 2>&1\n'
SAMPLED_STAGE_GATE = '''  # Separate authored sampled-resource library; no game shader input.
  cp "$host/native/SampledFramePacket.h" "$stage/native/"
  mkdir -p "$stage/native/public-sampled-shaders"
  cp "$host/native/public-sampled-shaders/PublicSampledShaders.h" "$stage/native/public-sampled-shaders/"
  python3 "$host/native/compile_public_frame_shaders.py" "$variant" \\
    "$host/native/public-sampled-shaders" "$stage/native/public-sampled-libraries" sampled \\
    > "$output/$variant-public-sampled-shaders.log" 2>&1
'''
TARGET_STAGE_GATE = '''  # Explicit authored ordered-target validators and frame-local lifetime plan.
  cp "$host/native/TargetFramePacket.h" "$host/native/TargetFrameLifetime.h" "$stage/native/"
'''
METAL_TOOLCHAIN_GATE = '''# Compile the authored MSL offline; Xcode 26 may need its official optional component.
if ! xcrun -sdk iphoneos metal --version > "$output/metal-toolchain.log" 2>&1; then
  xcodebuild -downloadComponent metalToolchain >> "$output/metal-toolchain.log" 2>&1
  xcrun -sdk iphoneos metal --version >> "$output/metal-toolchain.log" 2>&1
fi
'''
COUNTER_ANCHOR = '  audio::tests -- --test-threads=1 2>&1 | tee "$output/public-audio-observer-tests.log"\n'
COUNTER_GATE = '''# Focused public counter regression; reuse the existing kernel test products.
cargo +nightly-2026-10-11 test --locked --offline \\
  --manifest-path "$kit/skyrim-recomp-web/Cargo.toml" -p kernel-hle --lib \\
  dispatch::counter_tests -- --test-threads=1 \\
  2>&1 | tee "$output/public-dispatch-counter-tests.log"
'''


def digest(data):
    return hashlib.sha256(data).hexdigest()


def validate_request(env, commit, now=None):
    if env.get("CIRCLECI") != "true" or env.get("PUBLIC_BUILD_AND_UI_ENABLED") != "true":
        raise ValueError("explicit_manual_combined_parameter_required")
    if not SHA.fullmatch(commit) or env.get("REVIEWED_SOURCE_COMMIT") != commit or env.get("CIRCLE_SHA1") != commit:
        raise ValueError("externally_reviewed_exact_commit_required")
    if env.get("SKYRT_OWNED_HOST_SCOPE", ""):
        raise ValueError("unexpected_full_route_scope")
    if (datetime.now(timezone.utc) if now is None else now) < NOT_BEFORE:
        raise ValueError("cloud_execution_before_0730_JST_refused")


def checked(path, expected, limit=1_000_000):
    if path.is_symlink() or not path.is_file():
        raise ValueError("public_helper_not_regular")
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit or digest(data) != expected:
        raise ValueError("public_source_pin_mismatch:" + path.name)
    return data


def load_route(repo):
    for name, expected in BOOTSTRAP_PINS.items():
        path = repo / name
        for ancestor in [path, *path.parents]:
            if ancestor == repo:
                break
            if ancestor.is_symlink():
                raise ValueError("public_helper_parent_symlink")
        checked(path, expected)
    spec = importlib.util.spec_from_file_location("original_public_route", repo / "ci/prepare-fix-build-route/prepare_route.py")
    route = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(route)
    return route


def require_input_snapshot(repo):
    base = repo / "ci/owned-host/tools/ios-owned-game-host"
    for name, expected in INPUT_PINS.items():
        checked(base / name, expected)
    kernel = repo / "ci/owned-host/skyrim-recomp-web/crates/kernel-hle/src"
    for name, expected in KERNEL_PINS.items():
        checked(kernel / name, expected)
    require_build_alignment(base)


def require_build_alignment(base):
    project = (base / "project.yml").read_text(encoding="utf-8")
    native = (base / "sdk-test/tests/NativeTraceSDKProbeTests.swift").read_text(encoding="utf-8")
    declared = re.findall(r'(?m)^\s+CFBundleVersion:\s*"([1-9][0-9]{0,5})"\s*$', project)
    expected = re.findall(r'XCTAssertEqual\(Bundle\.main\.object\(forInfoDictionaryKey: "CFBundleVersion"\) as\? String, "([1-9][0-9]{0,5})"\)', native)
    if len(declared) != 1 or declared != expected:
        raise ValueError("public_host_build_expectation_mismatch")
    return declared[0]


def derive(setup, stages, repo, output):
    setup, stages = [x.decode("utf-8").replace("\r\n", "\n") for x in (setup, stages)]
    for text, anchor in ((setup, HARNESS), (stages, HARNESS), (setup, STAGE_CALL),
                         (stages, LOOP), (stages, AFTER), (stages, SELECTION), (stages, SELECTION_END),
                         (stages, GUI), (stages, GUI_END), (stages, COUNTER_ANCHOR)):
        if text.count(anchor) != 1:
            raise ValueError("reviewed_route_anchor_changed")
    before, rest = stages.split(LOOP)
    if before.count(COUNTER_ANCHOR) != 1:
        raise ValueError("counter_gate_must_precede_build_loop")
    before = before.replace(COUNTER_ANCHOR, COUNTER_ANCHOR + COUNTER_GATE)
    before += METAL_TOOLCHAIN_GATE
    loop, _ = rest.split(AFTER)
    if not loop.endswith("\ndone\n"):
        raise ValueError("simulator_loop_boundary")
    selection = stages[stages.index(SELECTION):stages.index(SELECTION_END) + 1]
    gui = stages[stages.index(GUI):stages.index(GUI_END)]
    if not gui.endswith("PY\n"):
        raise ValueError("simulator_gui_boundary")
    if loop.count(METAL_STAGE_ANCHOR) != 1:
        raise ValueError("metal_probe_stage_anchor_changed")
    loop = loop.replace(METAL_STAGE_ANCHOR, METAL_STAGE_ANCHOR + METAL_STAGE_GATE + SAMPLED_STAGE_GATE + TARGET_STAGE_GATE)
    # Preserve the original device+simulator loop plus the explicit probe copy,
    # the source/worker/policy gates and native Metal oracles. No second app build loop.
    selected = before + LOOP + loop + "\n" + selection
    selected += '''cleanup() {
  python3 "$harness/ci/run-bounded-command.py" 15 xcrun simctl shutdown "$device_id" >/dev/null 2>&1 || true
  python3 "$harness/ci/run-bounded-command.py" 15 xcrun simctl delete "$device_id" >/dev/null 2>&1 || true
}
trap cleanup EXIT
xcrun simctl boot "$device_id"
# Cold iOS 26.5 migration has taken 273 seconds. Do not spend the test budget on it.
python3 "$harness/ci/run-bounded-command.py" 360 xcrun simctl bootstatus "$device_id" -b
'''
    selected += gui
    selected += '''# Build once and retain the unchanged V1 loader/native trace two-case gate.
python3 "$host/sdk-test/run_sdk_tests.py" --stage "$work/owned-app-simulator" \\
  --output "$output/xctest" --destination "platform=iOS Simulator,id=$device_id" \\
  2>&1 | tee "$output/owned-host-sdk-driver.log"
'''
    selected += "python3 " + shlex.quote(str(output / "run_ui_groups.py")) + ''' --host "$host" \\
  --stage "$work/owned-app-simulator" --built-output "$output/xctest" \\
  --output "$output/xctest-ui" --destination "platform=iOS Simulator,id=$device_id" \\
  2>&1 | tee "$output/mobile-settings-sdk-driver.log"
'''
    selected += "cp " + shlex.quote(str(output / "source-provenance.json")) + ' "$output/ui-route-source-provenance.json"\n'
    selected = selected.replace(HARNESS, "harness=" + shlex.quote(str(repo)))
    setup = setup.replace(HARNESS, "harness=" + shlex.quote(str(repo)))
    setup = setup.replace(STAGE_CALL, "bash " + shlex.quote(str(output / "build-and-ui-stage.sh")) + ' "$work" "$simde"')
    return {"preflight.sh": setup.encode(), "build-and-ui-stage.sh": selected.encode()}


def prepare(repo, commit, output):
    repo, output = Path(repo).resolve(strict=True), Path(output)
    if output.exists() or output.is_symlink() or output.resolve().is_relative_to(repo):
        raise ValueError("fresh_separate_route_output_required")
    route = load_route(repo)
    pins = json.loads((repo / "ci/prepare-fix-build-route/reference-pins.json").read_text())
    source = route.inputs(repo, commit, pins)  # exact HEAD, origin, clean tree, all reference pins
    require_input_snapshot(repo)  # refuse old controls/graphics before spending runner time
    runner = checked(ROOT / "run_ui_groups.py", UI_RUNNER_SHA)
    scripts = derive(source["ci/run-ios-owned-host-preflight.sh"], source["ci/run-ios-owned-host-tests.sh"],
                     repo, output.resolve())
    scripts["run_ui_groups.py"] = runner
    if route.inputs(repo, commit, pins) != source:
        raise ValueError("source_changed_during_preparation")
    require_input_snapshot(repo)
    output.mkdir(parents=False)
    for name, data in scripts.items():
        (output / name).write_bytes(data)
    record = dict(schema=1, scope="public-device-build-and-ui-evidence", source_commit=commit,
                  source_pins=pins["files"], required_input_pins=INPUT_PINS, required_kernel_pins=KERNEL_PINS,
                  derived_scripts={name: digest(data) for name, data in scripts.items()},
                  simulator_boot_deadline_seconds=360, reused_stage_deadline_seconds=1500,
                  source_commit_signature_authenticated=False, prepared_only=True,
                  sdk_execution_verified=False, audio_output_verified=False,
                  full_integration_verified=False, game_executed=False, physical_iphone_verified=False)
    (output / "source-provenance.json").write_text(json.dumps(record, indent=2) + "\n")
    return record


def recheck(repo, commit, output):
    repo, output = Path(repo).resolve(strict=True), Path(output).resolve(strict=True)
    if output.is_relative_to(repo):
        raise ValueError("separate_route_output_required")
    route = load_route(repo)
    pins = json.loads((repo / "ci/prepare-fix-build-route/reference-pins.json").read_text())
    source = route.inputs(repo, commit, pins)
    require_input_snapshot(repo)
    runner = checked(ROOT / "run_ui_groups.py", UI_RUNNER_SHA)
    scripts = derive(source["ci/run-ios-owned-host-preflight.sh"], source["ci/run-ios-owned-host-tests.sh"], repo, output)
    scripts["run_ui_groups.py"] = runner
    for name, data in scripts.items():
        checked(output / name, digest(data))
    # The unchanged SDK drivers are the test authorities. This final step only
    # rechecks public source/recipe bytes; it does not manufacture test outcomes.
    return dict(source_commit=commit, public_source_rechecked_after_route=True,
                source_commit_signature_authenticated=False, full_integration_verified=False,
                audio_output_verified=False, game_executed=False, physical_iphone_verified=False)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("prepare", "recheck"))
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    validate_request(os.environ, args.source_commit)
    if sys.platform != "darwin" or os.uname().machine != "arm64":
        raise ValueError("Apple_Silicon_runner_required")
    operation = prepare if args.operation == "prepare" else recheck
    print(json.dumps(operation(args.repo, args.source_commit, args.output), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
