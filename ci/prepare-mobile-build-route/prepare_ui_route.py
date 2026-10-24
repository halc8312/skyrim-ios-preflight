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
    "ci/prepare-fix-build-route/reference-pins.json": "a256acde2b4fc29b3a5fabfb8c57ad50eccc78ff9d4369c759b49026152565ca",
    "ci/run-bounded-command.py": "f7391d02a43dc5dc97ea7f017cd5e502c70b7a4f06b4ce05a9e4972b49a890dd",
}
INPUT_PINS = {
    "app/MobilePadState.swift": "8df06a806565cff42ab2bf4fe3d39b7de009149f190b02eefdcc5183c46ee2ff",
    "app/MobileControlsSettings.swift": "a16a9bb231b60a11c155321a1762ac34b1ae053238a0953014d9f95db53cafb8",
    "app/MobileControlsView.swift": "0de5944765e6d0e524f9d96096742f10786ce2b3996064414168071ce390ddc7",
    "sdk-test/tests/MobileControlsTests.swift": "9c1bd7b322b00e0faba222b56d6f2c28c9d8f2bc915be8364074e6b1ed082d40",
    "src/alpha_coverage_probe.rs": "b95c96f882ef3502f11668a353cb107e29835152dec6ed63e4dcb10b76cce678",
    "native/GraphicsPresentationProbe.h": "451769e952e8d7c95cd8c2f296476cc29f3d49ebf374f13e88774eeaf9b79f2a",
    "sdk-test/tests/GraphicsPresentationTests.swift": "640182cb2850ca1c67c9a1ead0b08c3b6e99c18c05d0700efee0ae0ec5dc4dd4",
}
KERNEL_PINS = {
    "lib.rs": "6751fbbdbabd662a2cc68c043b6c6a0280d6241d478c015d0274c0d69bdbdfcd",
    "dispatch.rs": "9747e76edf9cd3007dde77afeb8f6125c6b4151c8023dbe4c0c8ebe5e154e714",
    "dispatch_counter_tests.rs": "ce8e30b56ed4f25390a2583ab1c16087085763ff767b927353b5e6e8b66d3e1b",
}
UI_RUNNER_SHA = "901299a790abc0ee7f34d48de3e06a8b8411baef1cd7535121e1272f76784883"
HARNESS = 'harness="$(cd "$(dirname "$0")/.." && pwd)"'
LOOP = "for variant in device simulator; do\n"
AFTER = "\n# The V2 image is independently compiled from public authored sources."
STAGE_CALL = 'bash "$harness/ci/run-ios-owned-host-tests.sh" "$work" "$simde"'
SELECTION = 'selection="$(python3 - <<\'PY\'\n'
SELECTION_END = "\naudio_watch_pid=\n"
GUI = "# Controlled environment comparison: preserve the same source, runtime, UDID\n"
GUI_END = 'python3 "$harness/ci/probe-host-audio.py" --phase post_gui'
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
    loop, _ = rest.split(AFTER)
    if not loop.endswith("\ndone\n"):
        raise ValueError("simulator_loop_boundary")
    selection = stages[stages.index(SELECTION):stages.index(SELECTION_END) + 1]
    gui = stages[stages.index(GUI):stages.index(GUI_END)]
    if not gui.endswith("PY\n"):
        raise ValueError("simulator_gui_boundary")
    # Preserve the original device+simulator loop exactly, after one pass over
    # the source/worker/policy gates and WGPU builder. No second app build loop.
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
