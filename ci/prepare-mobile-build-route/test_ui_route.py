"""Authored route boundary tests; no Apple execution or device input."""
import importlib.util
import hashlib
from datetime import timedelta
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent
CLOUD = HERE.parents[1]
REPO = CLOUD / "ci/owned-host"
HOST = REPO / "tools/ios-owned-game-host"


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


route = load("prepare_ui_route")
runner = load("run_ui_groups")


def input_scripts():
    return ((CLOUD / "ci/run-ios-owned-host-preflight.sh").read_bytes(),
            (CLOUD / "ci/run-ios-owned-host-tests.sh").read_bytes())


class PublicUIRouteTests(unittest.TestCase):
    def test_build_number_matches_the_preserved_native_trace_assertion_before_runner_work(self):
        self.assertEqual(route.require_build_alignment(HOST), "17")
        with tempfile.TemporaryDirectory() as temp:
            host = Path(temp)
            (host / "sdk-test/tests").mkdir(parents=True)
            project = (HOST / "project.yml").read_text(encoding="utf-8")
            native = (HOST / "sdk-test/tests/NativeTraceSDKProbeTests.swift").read_text(encoding="utf-8")
            (host / "project.yml").write_text(project,encoding="utf-8")
            test = host / "sdk-test/tests/NativeTraceSDKProbeTests.swift"
            test.write_text(native,encoding="utf-8")
            self.assertEqual(route.require_build_alignment(host), "17")
            stale = native.replace('as? String, "17")','as? String, "16")')
            self.assertNotEqual(stale,native)
            test.write_text(stale,encoding="utf-8")
            with self.assertRaisesRegex(ValueError,"build_expectation_mismatch"):
                route.require_build_alignment(host)

    def test_actual_reference_pin_chain_matches_every_reviewed_input(self):
        route.load_route(CLOUD)
        pins=json.loads((CLOUD/'ci/prepare-fix-build-route/reference-pins.json').read_text())
        for name,wanted in pins['files'].items():
            with self.subTest(source=name):
                self.assertEqual(hashlib.sha256((CLOUD/name).read_bytes()).hexdigest(),wanted)
        route.require_input_snapshot(CLOUD)
        runner.load_helpers(HOST)

    def test_manual_false_default_exact_commit_and_scope_refusal(self):
        commit = "a" * 40
        env = dict(CIRCLECI="true", PUBLIC_BUILD_AND_UI_ENABLED="true", REVIEWED_SOURCE_COMMIT=commit,
                   CIRCLE_SHA1=commit, SKYRT_OWNED_HOST_SCOPE="")
        route.validate_request(env, commit, route.NOT_BEFORE)
        for change in (dict(PUBLIC_BUILD_AND_UI_ENABLED="false"), dict(PUBLIC_BUILD_AND_UI_ENABLED=True),
                       dict(CIRCLECI="false"), dict(CIRCLE_SHA1="b" * 40),
                       dict(REVIEWED_SOURCE_COMMIT="A" * 40), dict(SKYRT_OWNED_HOST_SCOPE="normal-play-evidence")):
            with self.subTest(change=change), self.assertRaises(ValueError):
                route.validate_request(dict(env, **change), commit, route.NOT_BEFORE)
        with self.assertRaises(ValueError):
            route.validate_request(env, "not-a-source-pin", route.NOT_BEFORE)
        with self.assertRaisesRegex(ValueError, "before_0730"):
            route.validate_request(env, commit, route.NOT_BEFORE - timedelta(microseconds=1))

    def test_original_prebuild_loop_loader_and_single_build_are_retained(self):
        setup, stages = input_scripts()
        scripts = route.derive(setup, stages, Path("/public/repo"), Path("/public/route"))
        original = stages.decode().replace("\r\n", "\n")
        selected = scripts["build-and-ui-stage.sh"].decode()
        prebuild, rest = original.split(route.LOOP)
        loop, _ = rest.split(route.AFTER)
        expected = (prebuild + route.LOOP + loop).replace(route.HARNESS, "harness=/public/repo")
        self.assertTrue(selected.replace(route.COUNTER_GATE, "").replace(route.METAL_STAGE_GATE, "").replace(route.SAMPLED_STAGE_GATE, "").replace(route.TARGET_STAGE_GATE, "").replace(route.METAL_TOOLCHAIN_GATE, "").startswith(expected))
        self.assertEqual(selected.count(route.COUNTER_GATE), 1)
        self.assertEqual(selected.count(route.METAL_STAGE_GATE), 1)
        self.assertEqual(selected.count(route.SAMPLED_STAGE_GATE), 1)
        self.assertEqual(selected.count(route.TARGET_STAGE_GATE), 1)
        self.assertIn('"$host/native/TargetFramePacket.h"',route.TARGET_STAGE_GATE)
        self.assertIn('"$host/native/TargetFrameLifetime.h"',route.TARGET_STAGE_GATE)
        self.assertIn('"$host/native/SampledFramePacket.h"',route.SAMPLED_STAGE_GATE)
        self.assertIn('"$host/native/public-sampled-shaders/PublicSampledShaders.h"',route.SAMPLED_STAGE_GATE)
        self.assertIn('"$stage/native/public-sampled-libraries" sampled',route.SAMPLED_STAGE_GATE)
        self.assertEqual(route.SAMPLED_STAGE_GATE.count('compile_public_frame_shaders.py'),1)
        self.assertEqual(selected.count(route.METAL_TOOLCHAIN_GATE), 1)
        self.assertLess(selected.index(route.METAL_TOOLCHAIN_GATE), selected.index(route.LOOP))
        self.assertIn('xcodebuild -downloadComponent metalToolchain', route.METAL_TOOLCHAIN_GATE)
        self.assertIn('"$host/native/MetalConstantReplay.metal"', route.METAL_STAGE_GATE)
        self.assertIn('"$host/native/MetalConstantProbe.mm"', route.METAL_STAGE_GATE)
        self.assertIn('"$host/native/MetalFrameProbe.mm"', route.METAL_STAGE_GATE)
        self.assertIn('"$host/native/FramePlanPacket.h"', route.METAL_STAGE_GATE)
        self.assertEqual(route.METAL_STAGE_GATE.count('compile_public_frame_shaders.py'), 1)
        self.assertIn('"$stage/native/public-frame-libraries"', route.METAL_STAGE_GATE)
        self.assertLess(selected.index(route.COUNTER_GATE), selected.index(route.LOOP))
        self.assertIn('--locked --offline', route.COUNTER_GATE)
        self.assertIn('dispatch::counter_tests -- --test-threads=1', route.COUNTER_GATE)
        self.assertEqual(selected.count(route.LOOP), 1)
        self.assertEqual(selected.count('"$harness/ci/package-owned-host-ipa.py"'), 1)
        self.assertEqual(selected.count('"$host/sdk-test/run_sdk_tests.py"'), 1)
        self.assertEqual(selected.count("run_ui_groups.py"), 1)
        self.assertIn('run-bounded-command.py" 360 xcrun simctl bootstatus "$device_id" -b', selected)
        self.assertIn('trap cleanup EXIT', selected)
        self.assertIn('same_selected_device_booted=same', selected)
        self.assertNotIn("run_v2_sdk_tests.py", selected)
        self.assertNotIn("run_device_features_tests.py", selected)
        self.assertNotIn("probe-macos-output.py", selected)
        self.assertNotIn("build-for-testing", selected.split("# Build once")[1].splitlines()[1])
        # Setup stays exact apart from the two reviewed absolute callsite replacements.
        expected_setup = setup.decode().replace("\r\n", "\n").replace(route.HARNESS, "harness=/public/repo")
        expected_setup = expected_setup.replace(route.STAGE_CALL,
            'bash /public/route/build-and-ui-stage.sh "$work" "$simde"')
        self.assertEqual(scripts["preflight.sh"].decode(), expected_setup)
        self.assertIn("owned_host_stage_seconds=1500", expected_setup)

    def test_ambiguous_or_missing_derivation_boundaries_refuse(self):
        setup, stages = input_scripts()
        stages = stages.replace(b"\r\n", b"\n")
        for anchor in (route.LOOP, route.AFTER, route.SELECTION, route.GUI, route.GUI_END,
                       route.SELECTION_END, route.COUNTER_ANCHOR):
            for changed in (stages.replace(anchor.encode(), b"", 1), stages + anchor.encode()):
                with self.subTest(anchor=anchor), self.assertRaises(ValueError):
                    route.derive(setup, changed, Path("/repo"), Path("/new"))

    def test_new_input_bytes_required_before_costly_work(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(strict=True)
            for name in route.INPUT_PINS:
                destination = root / "ci/owned-host/tools/ios-owned-game-host" / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO / "tools/ios-owned-game-host" / name, destination)
            for name in route.KERNEL_PINS:
                relative = Path("skyrim-recomp-web/crates/kernel-hle/src") / name
                destination = root / "ci/owned-host" / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO / relative, destination)
            route.require_input_snapshot(root)
            for relative in ("tools/ios-owned-game-host/app/MobilePadState.swift",
                             "tools/ios-owned-game-host/sdk-test/tests/GraphicsPresentationTests.swift",
                             "skyrim-recomp-web/crates/kernel-hle/src/dispatch_counter_tests.rs"):
                target = root / "ci/owned-host" / relative
                original = target.read_bytes()
                target.write_bytes(original + b"\n")
                with self.subTest(path=relative), self.assertRaisesRegex(ValueError, "public_source_pin_mismatch"):
                    route.require_input_snapshot(root)
                target.write_bytes(original)

    def test_fresh_outside_output_required_before_helper_import(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(strict=True)
            with mock.patch.object(route, "load_route") as load:
                for output in (root, root / "inside"):
                    with self.assertRaises(ValueError):
                        route.prepare(root, "a" * 40, output)
                load.assert_not_called()

    def test_post_route_source_recheck_refuses_changed_derived_script(self):
        setup, stages = input_scripts()
        source = {"ci/run-ios-owned-host-preflight.sh": setup, "ci/run-ios-owned-host-tests.sh": stages}
        original = mock.Mock(inputs=mock.Mock(return_value=source))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(strict=True)
            repo, output = root / "repo", root / "prepared"
            (repo / "ci/prepare-fix-build-route").mkdir(parents=True)
            (repo / "ci/prepare-fix-build-route/reference-pins.json").write_text("{}")
            output.mkdir()
            scripts = route.derive(setup, stages, repo, output)
            scripts["run_ui_groups.py"] = (HERE / "run_ui_groups.py").read_bytes()
            for name, data in scripts.items():
                (output / name).write_bytes(data)
            with mock.patch.object(route, "load_route", return_value=original), \
                 mock.patch.object(route, "require_input_snapshot") as source_check:
                report = route.recheck(repo, "a" * 40, output)
                self.assertIs(report["public_source_rechecked_after_route"], True)
                self.assertIs(report["full_integration_verified"], False)
                source_check.assert_called_once_with(repo)
                original.inputs.assert_called_once_with(repo, "a" * 40, {})
                path = output / "build-and-ui-stage.sh"
                path.write_bytes(path.read_bytes() + b"echo altered\n")
                with self.assertRaisesRegex(ValueError, "public_source_pin_mismatch"):
                    route.recheck(repo, "a" * 40, output)

    def test_helper_import_closure_pinned_before_execution(self):
        self.assertEqual(hashlib.sha256((HERE / "run_ui_groups.py").read_bytes()).hexdigest(), route.UI_RUNNER_SHA)
        loaded = runner.load_helpers(HOST)
        self.assertEqual((len(loaded._mobile.TESTS), len(loaded.SETTINGS_TESTS),
                          len(loaded.GRAPHICS_TESTS), len(loaded.AUDIO_TESTS)), (10, 6, 3, 6))
        with tempfile.TemporaryDirectory() as temp:
            host = Path(temp).resolve(strict=True)
            (host / "sdk-test").mkdir()
            for name in runner.HELPER_PINS:
                shutil.copyfile(HOST / "sdk-test" / name, host / "sdk-test" / name)
            path = host / "sdk-test/run_sdk_tests.py"
            path.write_bytes(path.read_bytes() + b"\n")
            with mock.patch.object(runner.importlib.util, "spec_from_file_location") as execution:
                with self.assertRaisesRegex(ValueError, "pin_mismatch"):
                    runner.load_helpers(host)
                execution.assert_not_called()

    def fixture(self, root):
        stage, built, output = root / "stage", root / "built", root / "evidence"
        (stage / "OwnedGameHost.xcodeproj").mkdir(parents=True)
        (built / "DerivedData/Build/Products/Debug-iphonesimulator/OwnedGameHost.app").mkdir(parents=True)
        return stage, built, output, "platform=iOS Simulator,id=12345678-1234-1234-1234-123456789abc"

    def test_three_original_groups_same_build_and_strict_false_scope(self):
        features = runner.load_helpers(HOST)
        images = dict(attachment_export_available=1, attachment_exported=1, exported_png_count=2)
        with tempfile.TemporaryDirectory() as temp:
            stage, built, output, destination = self.fixture(Path(temp).resolve(strict=True))
            def mobile_main(argv):
                self.assertEqual(argv[argv.index("--built-output") + 1], str(built))
                self.assertEqual(argv[argv.index("--destination") + 1], destination)
                path = Path(argv[argv.index("--output") + 1]); path.mkdir()
                (path / "public-mobile-controls-result.json").write_text(json.dumps(features._mobile.success_report(images)))
                return 0
            with mock.patch.object(features._mobile, "main", side_effect=mobile_main) as mobile, \
                 mock.patch.object(features, "run_group", return_value=images) as settings:
                report = runner.run_groups(features, stage, built, output, destination)
            self.assertEqual(mobile.call_count, 1)
            self.assertEqual(settings.call_args_list, [
                mock.call("settings", stage, built / "DerivedData", output, destination),
                mock.call("graphics", stage, built / "DerivedData", output, destination)])
            self.assertEqual(report["exported_png_count"], 4)
            self.assertEqual(report["graphics_cases_passed"], 3)
            self.assertIs(report["public_offscreen_graphics_test_passed"], True)
            for name in ("audio_sdk_tests_executed", "mandatory_audio_gate_satisfied", "audio_output_verified",
                         "full_integration_verified", "game_executed", "physical_iphone_verified", "visual_review_performed"):
                self.assertIs(report[name], False)
            self.assertTrue((output / "public-ui-scope.json").is_file())

    def test_group_failure_still_collects_other_group_but_never_success(self):
        features = runner.load_helpers(HOST)
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp).resolve(strict=True))
            with mock.patch.object(features._mobile, "main", side_effect=RuntimeError("authored failure")), \
                 mock.patch.object(features, "run_group", return_value=dict(
                     attachment_export_available=1, attachment_exported=1, exported_png_count=2)) as settings:
                with self.assertRaisesRegex(RuntimeError, "incomplete:mobile"):
                    runner.run_groups(features, *args)
                self.assertEqual([call.args[0] for call in settings.call_args_list], ["settings", "graphics"])
            self.assertFalse((args[2] / "public-ui-scope.json").exists())

    def test_graphics_failure_cannot_publish_scoped_success(self):
        features = runner.load_helpers(HOST)
        images = dict(attachment_export_available=1, attachment_exported=1, exported_png_count=2)
        with tempfile.TemporaryDirectory() as temp:
            stage, built, output, destination = self.fixture(Path(temp).resolve(strict=True))
            def mobile_main(argv):
                path = Path(argv[argv.index("--output") + 1]); path.mkdir()
                (path / "public-mobile-controls-result.json").write_text(json.dumps(features._mobile.success_report(images)))
                return 0
            with mock.patch.object(features._mobile, "main", side_effect=mobile_main), \
                 mock.patch.object(features, "run_group", side_effect=[images, RuntimeError("authored graphics refusal")]) as groups:
                with self.assertRaisesRegex(RuntimeError, "incomplete:graphics"):
                    runner.run_groups(features, stage, built, output, destination)
            self.assertEqual([call.args[0] for call in groups.call_args_list], ["settings", "graphics"])
            self.assertFalse((output / "public-ui-scope.json").exists())

    def test_missing_unavailable_or_wrongly_typed_images_refuse(self):
        images = dict(attachment_export_available=1, attachment_exported=1, exported_png_count=2)
        runner.require_images(images)
        for key in images:
            for value in (0, True, "1", None):
                changed = dict(images, **{key: value})
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    runner.require_images(changed)

    def test_circle_config_has_one_false_manual_combined_job(self):
        import yaml
        before = yaml.safe_load((HERE / "testdata/prior-circleci-config.yml").read_text())
        after = yaml.safe_load((CLOUD / ".circleci/config.yml").read_text())
        self.assertEqual(set(after["jobs"]), {"public_build_and_ui"})
        self.assertEqual(set(after["workflows"]), {"manually_requested_public_build_and_ui"})
        self.assertIs(after["parameters"]["run_public_build_and_ui"]["default"], False)
        self.assertEqual(after["workflows"]["manually_requested_public_build_and_ui"]["when"],
                         "<< pipeline.parameters.run_public_build_and_ui >>")
        job = after["jobs"]["public_build_and_ui"]
        self.assertEqual(job["macos"], before["jobs"]["public_device_build"]["macos"])
        self.assertEqual(job["macos"]["xcode"], "26.6.0")
        self.assertEqual(job["resource_class"], "m4pro.medium")
        self.assertEqual(job["steps"][-2]["store_artifacts"]["path"], "out/public-evidence/owned-host")
        self.assertEqual(job["steps"][-1]["store_artifacts"],
                         dict(path="out/public-review", destination="public-review-download"))
        command = job["steps"][1]["run"]["command"]
        self.assertEqual(command.count("prepare_ui_route.py prepare"), 1)
        self.assertEqual(command.count("prepare_ui_route.py recheck"), 1)
        self.assertEqual(command.count("shutil.make_archive"), 1)
        self.assertIn("ci/run-bounded-command.py 120 python3", command)
        self.assertIn('"out/public-review"', command)
        self.assertLess(command.index("prepare_ui_route.py recheck"), command.index("shutil.make_archive"))
        self.assertIn('bash "$route/preflight.sh"', command)
        self.assertNotIn('<<',command,'CircleCI treats unescaped shell/Python << as template syntax')

    def test_failure_archive_never_hides_test_or_source_recheck_failure(self):
        import yaml
        config = yaml.safe_load((CLOUD / ".circleci/config.yml").read_text())
        command = config['jobs']['public_build_and_ui']['steps'][1]['run']['command']
        stubs = r'''
function bash() { return "$TEST_PREFLIGHT_STATUS"; }
function python3() {
  if [[ "$*" == *" recheck "* ]]; then
    printf checked > "$TMPDIR/rechecked"
    return "$TEST_RECHECK_STATUS"
  fi
  if [[ "$*" == *"ci/run-bounded-command.py"* ]]; then
    printf archived > "$TMPDIR/archive-attempted"
    return "$TEST_ARCHIVE_STATUS"
  fi
  return 0
}
'''
        for preflight,recheck,archive,expected in [(0,0,0,0),(17,0,0,17),(0,18,0,18),(17,18,0,17),(0,0,19,19)]:
            with self.subTest(statuses=(preflight,recheck,archive)), tempfile.TemporaryDirectory() as tmp:
                env=dict(os.environ,TMPDIR=tmp,REVIEWED_SOURCE_COMMIT='a'*40,
                    TEST_PREFLIGHT_STATUS=str(preflight),TEST_RECHECK_STATUS=str(recheck),TEST_ARCHIVE_STATUS=str(archive))
                result=subprocess.run(['bash','-c',stubs+'\n'+command],cwd=tmp,env=env,capture_output=True,timeout=10)
                self.assertEqual(result.returncode,expected,result.stderr.decode())
                self.assertEqual((Path(tmp)/'rechecked').read_text(),'checked')
                self.assertEqual((Path(tmp)/'archive-attempted').read_text(),'archived')

    def test_compact_artifacts_keep_originals_logs_results_and_images_within_free_storage(self):
        import yaml, zipfile
        config=yaml.safe_load((CLOUD/'.circleci/config.yml').read_text())
        command=config['jobs']['public_build_and_ui']['steps'][1]['run']['command']
        code=command.split("python3 -B ci/run-bounded-command.py 120 python3 -B -c '\n",1)[1].split("\n'\n",1)[0]
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'out/owned-host'
            required=['OwnedGameHost-0.1.0-unsigned.ipa','public-source-manifest.json','xctest/public-fullchain-result.json',
                'xctest-ui/graphics/xcodebuild.stdout.log',
                'xctest-ui/mobile/screenshots/a.png','xctest-ui/mobile/screenshots/b.png',
                'xctest-ui/settings/screenshots/a.png','xctest-ui/settings/screenshots/b.png']
            caches=['gpu-framework/device/cache.bin','xctest/DerivedData/Build/object.o','xctest-ui/graphics/GraphicsPresentationTests.xcresult/Info.plist']
            for name in required+caches:
                p=source/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(name.encode())
            before={name:(source/name).read_bytes() for name in required+caches}
            result=subprocess.run([sys.executable,'-c',code],cwd=tmp,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            self.assertIn(b'raw_xcresult_omitted=True',result.stdout)
            for name,data in before.items():self.assertEqual((source/name).read_bytes(),data)
            with zipfile.ZipFile(root/'out/public-review/SkyrimLab-build17-metal-target-review.zip') as archive:
                self.assertEqual(set(archive.namelist())-{n for n in archive.namelist() if n.endswith('/')},{'owned-host/'+n for n in required})
                for name in required:self.assertEqual(archive.read('owned-host/'+name),before[name])


if __name__ == "__main__":
    unittest.main()
