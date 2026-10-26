"""Execute mobile/settings/graphics SDK validators on one existing public build.

This scoped route does not execute or waive the production audio hardware gate.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import time


HELPER_PINS = {
    "run_sdk_tests.py": "238983d6945b0d5dbfb7b9d9d11278edfdd5c065b7333742d946518daa5371a2",
    "run_mobile_controls_tests.py": "c33dd044ab63d444053b4634a0074599d25add4a266d35e85675762075631d34",
    "run_device_features_tests.py": "b52c26d3cc0d091fcf13e7cc8ba09ca51ccc09a3f068b2ddebd5f2366a24e0b5",
}


def load_helpers(host: Path):
    host = host.resolve(strict=True)
    directory = host / "sdk-test"
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError("public_sdk_directory_not_regular")
    for name, expected in HELPER_PINS.items():
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("public_sdk_helper_not_regular")
        with path.open("rb") as stream:
            data = stream.read(100_001)
        if len(data) > 100_000 or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("public_sdk_helper_pin_mismatch:" + name)
    # All three transitive imports are checked before executing any helper.
    spec = importlib.util.spec_from_file_location("ui_route_features", directory / "run_device_features_tests.py")
    features = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(features)
    if (len(features._mobile.TESTS), len(features.SETTINGS_TESTS),
        len(features.GRAPHICS_TESTS), len(features.AUDIO_TESTS)) != (10, 6, 3, 6):
        raise ValueError("fixed_sdk_selector_count_changed")
    return features


def require_images(report):
    for key, expected in (("attachment_export_available", 1), ("attachment_exported", 1),
                          ("exported_png_count", 2)):
        if type(report.get(key)) is not int or report[key] != expected:
            raise ValueError("required_public_ui_png_export_missing:" + key)


def run_groups(features, stage: Path, built: Path, output: Path, destination: str):
    mobile = features._mobile
    # Each unchanged driver has its own bounded 120-second deadline. This adds
    # no second build-for-testing and no native audio test selection.
    stage, derived, output = mobile.prepare_paths(stage, built, output, destination)
    failures = []
    reports = {}
    started = time.monotonic()
    try:
        code = mobile.main(["--stage", str(stage), "--built-output", str(built),
                            "--output", str(output / "mobile"), "--destination", destination])
        if code != 0:
            raise RuntimeError("mobile_driver_nonzero")
        reports["mobile"] = mobile._sdk.strict_json(mobile.read_command_log(
            output / "mobile/public-mobile-controls-result.json", mobile.SUMMARY_LIMIT))
        require_images(reports["mobile"])
    except (OSError, ValueError, RuntimeError) as error:
        failures.append("mobile")
        print("PUBLIC_UI_GROUP_FAILURE " + json.dumps({"group": "mobile", "kind": type(error).__name__}))
    try:
        reports["settings"] = features.run_group("settings", stage, derived, output, destination)
        require_images(reports["settings"])
    except (OSError, ValueError, RuntimeError) as error:
        failures.append("settings")
        print("PUBLIC_UI_GROUP_FAILURE " + json.dumps({"group": "settings", "kind": type(error).__name__}))
    try:
        reports["graphics"] = features.run_group("graphics", stage, derived, output, destination)
    except (OSError, ValueError, RuntimeError) as error:
        failures.append("graphics")
        print("PUBLIC_UI_GROUP_FAILURE " + json.dumps({"group": "graphics", "kind": type(error).__name__}))
    if time.monotonic() - started > 380:
        failures.append("route_deadline")
    if failures:
        raise RuntimeError("public_ui_evidence_incomplete:" + ",".join(failures))
    result = dict(schema=1, scope="public-mobile-settings-graphics-evidence", mobile_cases_passed=10,
                  settings_cases_passed=6, graphics_cases_passed=3, exported_png_count=4, build_performed=0,
                  reused_build_products=True, public_fixture_only=True,
                  simulator_ui_tests_passed=True, public_offscreen_graphics_test_passed=True,
                  visual_review_performed=False,
                  audio_sdk_tests_executed=False, mandatory_audio_gate_satisfied=False,
                  audio_output_verified=False, full_integration_verified=False,
                  game_executed=False, physical_iphone_verified=False,
                  long_running_play_verified=False)
    with (output / "public-ui-scope.json").open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2); stream.write("\n")
    print("PUBLIC_UI_EVIDENCE " + json.dumps(result, sort_keys=True))
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("host", "stage", "built-output", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--destination", required=True)
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        raise RuntimeError("Apple SDK execution requires Darwin")
    run_groups(load_helpers(args.host), args.stage, args.built_output, args.output, args.destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
