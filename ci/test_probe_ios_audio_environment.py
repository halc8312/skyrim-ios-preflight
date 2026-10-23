"""Authored portable control/parser tests; never runs Apple or real audio."""
import contextlib
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ios_audio_environment", HERE / "probe-ios-audio-environment.py")
m = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = m
spec.loader.exec_module(m)
UDID = "11111111-2222-3333-4444-555555555555"
EXE = ("/Users/authored/Library/Developer/CoreSimulator/Devices/" + UDID
       + "/data/Containers/Bundle/Application/AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE/OwnedGameHost.app/OwnedGameHost")


def device(version="26.5"):
    return {"devices": {"com.apple.CoreSimulator.SimRuntime.iOS-" + version.replace(".", "-"): [dict(
        udid=UDID, state="Booted", isAvailable=True,
        deviceTypeIdentifier="com.apple.CoreSimulator.SimDeviceType.iPhone-17-Pro-Max")]}}


def value(mode=0):
    return dict(schema=1, mode=mode, available=True, completed=True, started=True, stopped=True,
                format_error=False, within_budget=True, phase=73, status=0, sample_rate_hz=48000,
                output_channels=2, render_callbacks=19, render_frames=9728, main_runloop_ms=240, stream_elapsed_ms=200)


def native(mode=0, result=None):
    phases = [1] + ([10, 11, 12, 13, 14] if mode else []) + m.GRAPH_PATH
    return ("".join(f"PUBLIC_AUDIO_PHASE {mode + 1} {phase}\n" for phase in phases)
            + "PUBLIC_IOS_AUDIO_RESULT " + json.dumps(value(mode) if result is None else result) + "\n").encode()


def sample_report():
    return (f"Process: OwnedGameHost [1234]\nPath: {EXE}\nUser ID: {os.getuid()}\nCall graph:\n"
            "  20 Thread_123\n  + 20 -[AVAudioEngine outputNode] (in AVFAudio) + 2 [0x1234]\n"
            "  +   20 OHNativeAudioEnvironmentGraph (in OwnedGameHost) + 4 [0x4321]\nBinary Images:\n").encode()


class ParserTests(unittest.TestCase):
    def setUp(self): m.STOP = False
    def tearDown(self): m.STOP = False

    def test_selected_device_requires_exact_runtime_type_udid_booted_and_unique(self):
        m.validate_device(json.dumps(device()), UDID)
        for version in ("26.3", "26.4", "26.5"):
            m.validate_device(json.dumps(device(version)), UDID, version)
        for key, replacement in (("udid", "other"), ("state", "Shutdown"), ("isAvailable", 1),
                                 ("deviceTypeIdentifier", "other")):
            broken = device(); next(iter(broken["devices"].values()))[0][key] = replacement
            with self.assertRaises(ValueError): m.validate_device(json.dumps(broken), UDID)
        broken = device(); rows = next(iter(broken["devices"].values())); rows.append(rows[0])
        with self.assertRaises(ValueError): m.validate_device(json.dumps(broken), UDID)
        with self.assertRaises(ValueError):
            m.validate_device(json.dumps(device()).replace("iOS-26-5", "iOS-26-4"), UDID)

    def test_version_cli_default_explicit_and_unknown_fail_before_probe(self):
        for arguments, expected in (([], "26.5"), (["--simulator-version", "26.4"], "26.4")):
            with mock.patch.object(m, "probe", return_value=dict(available=False)) as probe, \
                 mock.patch.object(m.signal, "signal"), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.main(["--udid", UDID, "--output", "/authored/out", *arguments]), 2)
            self.assertEqual(probe.call_args.args[2], expected)
        for version in ("26.2", "26.6", "latest", "26.4.0", ""):
            with mock.patch.object(m, "probe") as probe, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as refused:
                    m.main(["--udid", UDID, "--output", "/authored/out", "--simulator-version", version])
            self.assertEqual(refused.exception.code, 2); probe.assert_not_called()
            with mock.patch.object(m, "run") as run:
                with self.assertRaises(ValueError): m.probe(UDID, Path("/authored/out"), version)
            run.assert_not_called()

    def test_explicit_versions_reach_compile_with_matching_target_plist_and_unchanged_native_source(self):
        helps = {"launch": b"launch --console device bundle", "install": b"install device path",
                 "list": b"list devices --json search", "terminate": b"terminate device bundle",
                 "get_app_container": b"get_app_container device bundle app"}
        for version in ("26.3", "26.4"):
            captured = []
            def fake_run(command, deadline, seconds, **kwargs):
                if command[:3] == ["xcrun", "simctl", "help"]: raw = helps[command[3]]
                elif command[:4] == ["xcrun", "simctl", "list", "devices"]: raw = json.dumps(device(version)).encode()
                elif "--show-sdk-path" in command: raw = b"/authored/Simulator.sdk\n"
                elif "clang++" in command:
                    app = Path(command[command.index("-o") + 1]).parent
                    source = Path(next(x for x in command if x.endswith(".mm"))).read_text()
                    captured.append((command, seconds, plistlib.loads((app / "Info.plist").read_bytes()), source))
                    return m.Result(1, "completed", b"authored compiler refusal")
                else: raw = b""
                return m.Result(0, "completed", raw)
            with self.subTest(version=version), tempfile.TemporaryDirectory() as root, \
                 mock.patch.object(m.sys, "platform", "darwin"), mock.patch.object(m.os, "getuid", return_value=501), \
                 mock.patch.object(m.os, "geteuid", return_value=501), mock.patch.object(m, "run", side_effect=fake_run), \
                 mock.patch.object(m, "run_mode") as mode:
                result = m.probe(UDID, Path(root).resolve(strict=True) / "out", version)
            self.assertEqual(len(captured), 1)
            command, seconds, plist, source = captured[0]
            self.assertIn("arm64-apple-ios" + version + "-simulator", command)
            self.assertEqual(plist["MinimumOSVersion"], version)
            self.assertEqual(source, m.SOURCE); self.assertEqual(seconds, 15)
            self.assertFalse(result["available"]); self.assertEqual(result["reason"], "compile_failed")
            mode.assert_not_called()

    def test_help_requires_each_used_installed_option(self):
        cases = {"launch": "launch --console device bundle", "install": "install device path",
                 "list": "list devices --json search", "terminate": "terminate device bundle",
                 "get_app_container": "get_app_container device bundle app"}
        for kind, text in cases.items():
            self.assertTrue(m.help_valid(kind, text.encode()))
            self.assertFalse(m.help_valid(kind, b"unrecognized"))
        self.assertFalse(m.help_valid("launch", b"launch device bundle"))
        self.assertFalse(m.help_valid("list", b"list devices search"))

    def test_native_plain_and_playback_require_their_complete_paths(self):
        for mode in (0, 1):
            self.assertEqual(m.parse_native(native(mode), mode), value(mode))
            with self.assertRaises(ValueError): m.parse_native(native(mode).replace(b" 25\n", b" 21\n"), mode)
        with self.assertRaises(ValueError): m.parse_native(native(1), 0)

    def test_native_rejects_types_duplicates_bounds_and_false_success(self):
        for key, broken in (("schema", True), ("available", 1), ("render_callbacks", 0),
                            ("render_frames", 0), ("sample_rate_hz", 0), ("main_runloop_ms", 25001),
                            ("stream_elapsed_ms", 1001), ("phase", 21), ("status", -14)):
            v = value(); v[key] = broken
            with self.assertRaises(ValueError): m.parse_native(native(result=v), 0)
        for broken in (native() + native(), native().replace(b'"schema": 1', b'"schema": 1, "schema": 1'),
                       native().replace(b'"status": 0', b'"status": NaN'),
                       native().replace(b'"schema": 1', b'"schema": 1, "unknown": 0')):
            with self.assertRaises(ValueError): m.parse_native(broken, 0)

    def test_native_hang_is_honest_and_partial_record_is_retained(self):
        hang = value(); hang.update(available=False, completed=False, started=False, stopped=False,
                                   within_budget=False, phase=21, main_runloop_ms=25000,
                                   render_callbacks=0, render_frames=0, sample_rate_hz=0, output_channels=0)
        raw = b"PUBLIC_AUDIO_PHASE 1 1\nPUBLIC_AUDIO_PHASE 1 20\nPUBLIC_AUDIO_PHASE 1 21\nPUBLIC_IOS_AUDIO_RESULT "
        raw += json.dumps(hang).encode() + b"\n"
        self.assertFalse(m.parse_native(raw, 0)["available"])
        self.assertIsNone(m.parse_native(raw[:-5], 0))
        hang["within_budget"] = True
        with self.assertRaises(ValueError):
            m.parse_native(raw[:raw.index(b"PUBLIC_IOS_AUDIO_RESULT")] + b"PUBLIC_IOS_AUDIO_RESULT " + json.dumps(hang).encode() + b"\n", 0)

    def test_native_marker_prefix_and_size_are_bounded(self):
        self.assertEqual(m.parse_native(b"authored prefix " + native(), 0), value())
        for raw in (b"x" * (2 * m.LOG_LIMIT + 1), b"x" * 513 + native(),
                    native().replace(b"PUBLIC_IOS_AUDIO_RESULT ", b"PUBLIC_IOS_AUDIO_RESULT PUBLIC_IOS_AUDIO_RESULT ")):
            with self.assertRaises(ValueError): m.parse_native(raw, 0)

    def test_sanitized_progress_has_no_raw_paths_addresses_or_controls(self):
        text = m.sanitized_progress(b"Sampling /Users/authored/test [0xabc]\n\x1b\x00" * 1000, Path("/Users/authored"))
        encoded = json.dumps(text, ensure_ascii=True)
        self.assertLessEqual(len(encoded.encode("ascii")), 3500)
        self.assertNotIn("/Users", text); self.assertNotIn("0xabc", text)
        self.assertNotIn("\n", encoded); self.assertNotIn("\x1b", encoded)

    def test_sample_timeout_retains_progress_without_claiming_identity(self):
        helper = m.helper_module()
        target = helper.Identity(1234, os.getuid(), "date", EXE)
        with tempfile.TemporaryDirectory() as root:
            output = Path(root).resolve(strict=True)
            (output / "sample.txt").write_bytes(b"authored partial")
            with mock.patch.object(m, "run", return_value=m.Result(-9, "cancelled_or_deadline", b"Sampling completed; processing symbols\n")) as run, \
                 mock.patch.object(helper, "process_identity") as identity:
                report = {}; m.sample_once(helper, target, output, UDID, 123.0, report)
            self.assertEqual(run.call_args.args[1:3], (123.0, 15))
            self.assertEqual(run.call_args.kwargs["file_limit"], m.SAMPLE_LIMIT)
            identity.assert_not_called()
            self.assertIn("processing symbols", report["sample_tool_progress"])
            self.assertNotIn("sample_collected", report)
            self.assertEqual((output / "sample-command.log").read_bytes(), b"Sampling completed; processing symbols\n")

    def test_sample_requires_same_live_post_identity_and_exact_report(self):
        helper = m.helper_module()
        target = helper.Identity(1234, os.getuid(), "date", EXE)
        for after in (None, helper.Identity(1234, os.getuid(), "replacement", EXE), target):
            with self.subTest(after=after), tempfile.TemporaryDirectory() as root:
                output = Path(root).resolve(strict=True); (output / "sample.txt").write_bytes(sample_report())
                with mock.patch.object(m, "run", return_value=m.Result(0, "completed", b"done")), \
                     mock.patch.object(helper, "process_identity", return_value=after):
                    report = {}; m.sample_once(helper, target, output, UDID, time.monotonic() + 2, report)
                self.assertEqual(report.get("sample_collected", False), after == target)
                if after == target:
                    self.assertEqual(report["selected_stack_line_count"], 2)
                    self.assertNotIn("0x", json.dumps(report["selected_stack_lines"]))
                else: self.assertNotIn("selected_stack_lines", report)

    def test_non_apple_refuses_without_compilation_or_sdk_claim(self):
        with mock.patch.object(m.sys, "platform", "linux"), mock.patch.object(m, "run") as run:
            result = m.probe(UDID, Path("/authored/unused"))
        run.assert_not_called()
        self.assertFalse(result["available"]); self.assertFalse(result["sdk_tests_passed_claimed"])

    def test_exit_status_requires_both_modes_available_not_merely_completed(self):
        for available, expected in ((True, 0), (False, 2), (None, 2), (1, 2)):
            with mock.patch.object(m, "probe", return_value=dict(available=available, reason="completed", sdk_tests_passed_claimed=False)), \
                 mock.patch.object(m.signal, "signal"), contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(m.main(["--udid", UDID, "--output", "/authored/out"]), expected)
            self.assertFalse(json.loads(out.getvalue().split(" ", 1)[1])["sdk_tests_passed_claimed"])

    def test_compile_and_two_mode_orchestration_keep_target_env_signing_contract(self):
        commands, compiled_metadata = [], []
        helps = {"launch": b"launch --console device bundle", "install": b"install device path",
                 "list": b"list devices --json search", "terminate": b"terminate device bundle",
                 "get_app_container": b"get_app_container device bundle app"}
        def fake_run(command, deadline, seconds, **kwargs):
            commands.append((command, seconds, kwargs))
            if command[:3] == ["xcrun", "simctl", "help"]: raw = helps[command[3]]
            elif command[:4] == ["xcrun", "simctl", "list", "devices"]: raw = json.dumps(device()).encode()
            elif "--show-sdk-path" in command: raw = b"/authored/Simulator.sdk\n"
            elif "get_app_container" in command: raw = EXE.rsplit("/", 1)[0].encode() + b"\n"
            elif "clang++" in command:
                app = Path(command[command.index("-o") + 1]).parent
                source = Path(next(part for part in command if part.endswith(".mm"))).read_text()
                compiled_metadata.append((plistlib.loads((app / "Info.plist").read_bytes()), source))
                raw = b""
            else: raw = b""
            return m.Result(0, "completed", raw)
        modes = []
        def fake_mode(helper, udid, executable, mode, output, deadline, sampling):
            self.assertEqual((udid, executable), (UDID, EXE)); modes.append(mode)
            return dict(mode=mode, available=False)
        with tempfile.TemporaryDirectory() as root, mock.patch.object(m.sys, "platform", "darwin"), \
             mock.patch.object(m.os, "getuid", return_value=501), mock.patch.object(m.os, "geteuid", return_value=501), \
             mock.patch.dict(os.environ, {"SDKROOT": "forbidden", "IPHONEOS_DEPLOYMENT_TARGET": "26"}), \
             mock.patch.object(m, "run", side_effect=fake_run), mock.patch.object(m, "run_mode", side_effect=fake_mode), \
             contextlib.redirect_stdout(io.StringIO()):
            result = m.probe(UDID, Path(root).resolve(strict=True) / "out")
        self.assertEqual(modes, ["plain", "playback"])
        compile_call = next(call for call in commands if "clang++" in call[0])
        self.assertEqual(compile_call[1], 15)
        self.assertNotIn("SDKROOT", compile_call[2]["env"]); self.assertNotIn("IPHONEOS_DEPLOYMENT_TARGET", compile_call[2]["env"])
        self.assertIn("arm64-apple-ios26.5-simulator", compile_call[0])
        self.assertIn("-Werror", compile_call[0])
        self.assertEqual(len(compiled_metadata), 1)
        plist, source = compiled_metadata[0]
        self.assertEqual(plist["MinimumOSVersion"], "26.5")
        self.assertEqual(source, m.SOURCE)
        manifest = plist["UIApplicationSceneManifest"]
        self.assertIs(manifest["UIApplicationSupportsMultipleScenes"], False)
        self.assertEqual(set(manifest["UISceneConfigurations"]), {"UIWindowSceneSessionRoleApplication"})
        configurations = manifest["UISceneConfigurations"]["UIWindowSceneSessionRoleApplication"]
        self.assertEqual(len(configurations), 1)
        configuration = configurations[0]
        # Inspect the actual generated files at the compilation boundary: the
        # plist-selected classes/name must exist in the native configuration.
        delegate = re.search(r"@interface (\w+) : UIResponder <UIWindowSceneDelegate>", source).group(1)
        self.assertEqual(configuration["UISceneDelegateClassName"], delegate)
        self.assertIn("configuration.delegateClass = " + delegate + ".class;", source)
        self.assertEqual(configuration["UISceneConfigurationName"], re.search(r'initWithName:@"([^"]+)"', source).group(1))
        self.assertEqual(configuration["UISceneClassName"], "UIWindowScene")
        self.assertIn("configuration.sceneClass = " + configuration["UISceneClassName"] + ".class;", source)
        sign = next(call[0] for call in commands if call[0][0] == "/usr/bin/codesign")
        self.assertEqual(sign[1:5], ["--force", "--sign", "-", "--timestamp=none"])
        self.assertFalse(any("entitlement" in part for part in sign))
        self.assertFalse(result["available"]); self.assertEqual(result["reason"], "completed")

    def test_install_refusal_preserves_bounded_sanitized_failure_without_launching_modes(self):
        helps = {"launch": b"launch --console device bundle", "install": b"install device path",
                 "list": b"list devices --json search", "terminate": b"terminate device bundle",
                 "get_app_container": b"get_app_container device bundle app"}
        for code, reason in ((-9, "cancelled_or_deadline"), (70, "completed"), (0, "tool_output_limit")):
            with self.subTest(code=code, reason=reason), tempfile.TemporaryDirectory() as root:
                output = Path(root).resolve(strict=True) / "out"
                commands, progress = [], []
                started = time.monotonic()
                def fake_run(command, deadline, seconds, **kwargs):
                    commands.append(command)
                    if command[:3] == ["xcrun", "simctl", "help"]: raw = helps[command[3]]
                    elif command[:4] == ["xcrun", "simctl", "list", "devices"]: raw = json.dumps(device()).encode()
                    elif "--show-sdk-path" in command: raw = b"/authored/Simulator.sdk\n"
                    elif command[:3] == ["xcrun", "simctl", "install"]:
                        self.assertEqual(seconds, 20)
                        self.assertLessEqual(deadline, started + 120)
                        raw = (b"Install refused " + command[-1].encode() +
                               b" /Users/authored-private/input [0xfeed]\n\x1b\x00") * 100
                        progress.append(raw)
                        return m.Result(code, reason, raw)
                    else: raw = b""
                    return m.Result(0, "completed", raw)
                with mock.patch.object(m.sys, "platform", "darwin"), \
                     mock.patch.object(m.os, "getuid", return_value=501), mock.patch.object(m.os, "geteuid", return_value=501), \
                     mock.patch.object(m, "run", side_effect=fake_run), mock.patch.object(m, "run_mode") as mode, \
                     mock.patch.object(m.signal, "signal"), contextlib.redirect_stdout(io.StringIO()) as console:
                    self.assertEqual(m.main(["--udid", UDID, "--output", str(output)]), 2)
                mode.assert_not_called()
                result = json.loads(console.getvalue().split(" ", 1)[1])
                self.assertEqual(result["stage"], "install")
                self.assertEqual(result["reason"], "diagnostic_tool_or_validation_failure")
                self.assertEqual(result["modes"], [])
                for flag in ("available", "simulator_output_stream_verified", "physical_iphone_verified",
                             "audible_output_verified", "sdk_tests_passed_claimed"):
                    self.assertIs(result[flag], False)
                failure = result["tool_failure"]
                self.assertEqual(set(failure), {"stage", "reason", "exit_code", "progress"})
                self.assertEqual((failure["stage"], failure["reason"], failure["exit_code"]), ("install", reason, code))
                self.assertIs(type(failure["exit_code"]), int)
                self.assertIn("Install refused", failure["progress"])
                self.assertLessEqual(len(json.dumps(failure["progress"], ensure_ascii=True).encode("ascii")), 3500)
                self.assertLessEqual(len(json.dumps(failure, ensure_ascii=True).encode("ascii")), 4096)
                for private in (commands[-1][-1], "/Users", "authored-private", "0xfeed", "\x1b", "\x00"):
                    self.assertNotIn(private, console.getvalue())
                self.assertEqual(commands[-1][:3], ["xcrun", "simctl", "install"])
                self.assertEqual((output / "install.log").read_bytes(), progress[0])


@unittest.skipUnless(os.name == "posix", "POSIX owned-process fixture")
class SupervisionTests(unittest.TestCase):
    def setUp(self): m.STOP = False
    def tearDown(self): m.STOP = False

    def test_timeout_retains_flushed_progress_and_reaps_owned_child(self):
        original, children = subprocess.Popen, []
        def launch(*args, **kwargs):
            child = original(*args, **kwargs); children.append(child); return child
        started = time.monotonic()
        with mock.patch.object(m.subprocess, "Popen", side_effect=launch):
            result = m.run([sys.executable, "-c", "import time; print('authored progress',flush=True); time.sleep(2)"], started + 2, .25)
        self.assertEqual(result.reason, "cancelled_or_deadline")
        self.assertEqual(result.output, b"authored progress\n")
        self.assertEqual(children[0].returncode, -signal.SIGKILL)
        self.assertLess(time.monotonic() - started, 1.5)

    def test_exited_group_eperm_requires_zero_wait_proof_and_never_resignals_reaped_pid(self):
        child = subprocess.Popen([sys.executable, "-c", "pass"], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            # Observe the authored child as a zombie without reaping it. ps
            # queries this one owned PID; no target/system process is touched.
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                state = subprocess.check_output(["/bin/ps", "-p", str(child.pid), "-o", "stat="], timeout=0.25)
                if state.strip().startswith(b"Z"):
                    break
                time.sleep(0.01)
            else:
                self.fail("authored child did not exit within the fixture deadline")
            self.assertIsNone(child.returncode)
            real_wait = child.wait
            with mock.patch.object(m.os, "killpg", side_effect=PermissionError(errno.EPERM, "authored empty group")) as kill, \
                 mock.patch.object(child, "wait", wraps=real_wait) as wait:
                self.assertEqual(m.reap_owned_group(child), 0)
                kill.assert_called_once_with(child.pid, signal.SIGKILL)
                wait.assert_called_once_with(timeout=0)
                self.assertEqual(m.reap_owned_group(child), 0)
                self.assertEqual(kill.call_count, 1)
                self.assertEqual(wait.call_count, 1)
            self.assertEqual(child.returncode, 0)
        finally:
            if child.returncode is None:
                m.reap_owned_group(child)

    def test_running_group_eperm_propagates_and_fixture_cleanup_remains_bounded(self):
        unexpected = PermissionError(errno.EACCES, "authored different denial")
        fake_child = mock.Mock(returncode=None, pid=123)
        with mock.patch.object(m.os, "killpg", side_effect=unexpected):
            with self.assertRaises(PermissionError) as raised:
                m.reap_owned_group(fake_child)
            self.assertIs(raised.exception, unexpected)
            fake_child.wait.assert_not_called()
        original, children = subprocess.Popen, []
        denied = PermissionError(errno.EPERM, "authored live group denial")
        def launch(*args, **kwargs):
            child = original(*args, **kwargs); children.append(child); return child
        started = time.monotonic()
        try:
            with mock.patch.object(m.subprocess, "Popen", side_effect=launch), \
                 mock.patch.object(m.os, "killpg", side_effect=denied) as kill:
                with self.assertRaises(PermissionError) as raised:
                    m.run([sys.executable, "-c", "import time; time.sleep(2)"], started + 2, .15)
                self.assertIs(raised.exception, denied)
                self.assertEqual(len(children), 1)
                kill.assert_called_once_with(children[0].pid, signal.SIGKILL)
                self.assertIsNone(children[0].returncode)
                self.assertTrue(children[0].stdout.closed)
            self.assertLess(time.monotonic() - started, 1.5)
        finally:
            for child in children:
                m.reap_owned_group(child)
        self.assertEqual(children[0].returncode, -signal.SIGKILL)

    def test_output_limit_preserves_bounded_prefix(self):
        result = m.run([sys.executable, "-c", "import os,time; os.write(1,b'x'*100000); time.sleep(2)"], time.monotonic() + 2, 1)
        self.assertEqual(result.reason, "tool_output_limit")
        self.assertEqual(result.output, b"x" * m.TOOL_LIMIT)

    @unittest.skipUnless(hasattr(os, "fork"), "authored pipe-held descendant")
    def test_exited_leader_descendant_is_killed_before_reap(self):
        original, killed = os.killpg, []
        def kill(group, sig):
            result = original(group, sig); killed.append((group, sig)); return result
        with mock.patch.object(m.os, "killpg", side_effect=kill):
            result = m.run([sys.executable, "-c", "import os,time; c=os.fork(); time.sleep(2) if c==0 else os._exit(0)"],
                           time.monotonic() + 2, .25)
        self.assertEqual(result.reason, "cancelled_or_deadline"); self.assertEqual(result.code, 0)
        self.assertEqual(len(killed), 1); self.assertEqual(killed[0][1], signal.SIGKILL)

    def test_file_output_limit_and_parent_deadline_are_effective(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root).resolve(strict=True) / "out"
            result = m.run([sys.executable, "-c", "import sys; open(sys.argv[1],'wb').write(b'x'*100000)", str(path)],
                           time.monotonic() + 2, 1, file_limit=m.LOG_LIMIT)
            self.assertNotEqual(result.code, 0); self.assertLessEqual(path.stat().st_size, m.LOG_LIMIT)
        started = time.monotonic()
        result = m.run([sys.executable, "-c", "import time; time.sleep(2)"], started + .1, 15)
        self.assertEqual(result.reason, "cancelled_or_deadline"); self.assertLess(time.monotonic() - started, 1)

    def test_mode_supervises_authored_console_and_exact_bundle_cleanup(self):
        helper, original, commands = m.helper_module(), subprocess.Popen, []
        def launch(command, **kwargs):
            commands.append(command)
            return original([sys.executable, "-c", "import sys; sys.stdout.buffer.write(" + repr(native()) + "); sys.stdout.flush()"], **kwargs)
        with tempfile.TemporaryDirectory() as root, mock.patch.object(m.subprocess, "Popen", side_effect=launch), \
             mock.patch.object(m, "run", return_value=m.Result(1, "completed", b"already exited")) as cleanup, \
             mock.patch.object(helper, "find_target", return_value=None):
            output = Path(root).resolve(strict=True)
            result = m.run_mode(helper, UDID, EXE, "plain", output, time.monotonic() + 5, False)
        self.assertEqual(commands, [["xcrun", "simctl", "launch", "--console", UDID, m.BUNDLE, "plain"]])
        self.assertEqual(cleanup.call_args.args[0], ["xcrun", "simctl", "terminate", UDID, m.BUNDLE])
        self.assertTrue(result["available"]); self.assertFalse(result["sample_attempted"])

    def test_expired_mode_never_launches_and_cancelled_tool_never_launches(self):
        helper = m.helper_module()
        with mock.patch.object(m.subprocess, "Popen") as launch:
            with self.assertRaises(ValueError): m.run_mode(helper, UDID, EXE, "plain", Path("/authored"), time.monotonic() - 1, False)
            m.STOP = True
            self.assertEqual(m.run(["authored"], time.monotonic() + 1, 1).reason, "cancelled_or_deadline")
            launch.assert_not_called()


if __name__ == "__main__": unittest.main()
