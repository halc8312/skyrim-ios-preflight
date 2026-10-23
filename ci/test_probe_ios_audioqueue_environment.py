"""Authored portable validation/supervision checks, never Apple audio execution."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ios_audioqueue_environment", HERE / "probe-ios-audioqueue-environment.py")
m = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = m
spec.loader.exec_module(m)
UDID = "11111111-2222-3333-4444-555555555555"


def value():
    return dict(schema=1, available=True, completed=True, within_budget=True, session_ready=True,
                phase=123, probe_error=0, session_error=0, started=True, stopped=True, disposed=True,
                buffer_error=False, guard_checks=3, callbacks=22, reenqueue_attempts=22, reenqueue_successes=22,
                callback_status=0, sample_rate_hz=48000, channels=2, frames_per_buffer=512, buffer_count=3,
                time_flags=[1, 1], sample_times=[2048.0, 11776.0], time_advancing=True, observation_ms=200,
                stream_elapsed_ms=251, main_runloop_ms=350, statuses=[0] * len(m.STAGES))


def native(v=None, phases=None):
    return ("".join(f"PUBLIC_AUDIOQUEUE_PHASE {p}\n" for p in (m.SUCCESS_PHASES if phases is None else phases))
            + "PUBLIC_IOS_AUDIOQUEUE_RESULT " + json.dumps(value() if v is None else v) + "\n").encode()


def device(version="26.5"):
    return {"devices": {"com.apple.CoreSimulator.SimRuntime.iOS-" + version.replace(".", "-"): [dict(
        udid=UDID, state="Booted", isAvailable=True,
        deviceTypeIdentifier="com.apple.CoreSimulator.SimDeviceType.iPhone-17-Pro-Max")]}}


HELPS = {"launch": b"launch --console device bundle", "install": b"install device path",
         "list": b"list devices --json search", "terminate": b"terminate device bundle"}


class ParserTests(unittest.TestCase):
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
            with mock.patch.object(m.engine, "run") as run:
                with self.assertRaises(ValueError): m.probe(UDID, Path("/authored/out"), version)
            run.assert_not_called()

    def test_explicit_versions_reach_compile_with_matching_target_plist_and_unchanged_native_source(self):
        for version in ("26.3", "26.4"):
            captured = []
            def fake_run(command, deadline, seconds, **kwargs):
                if command[:3] == ["xcrun", "simctl", "help"]: raw = HELPS[command[3]]
                elif command[:4] == ["xcrun", "simctl", "list", "devices"]: raw = json.dumps(device(version)).encode()
                elif "--show-sdk-path" in command: raw = b"/authored/Simulator.sdk\n"
                elif "clang++" in command:
                    app = Path(command[command.index("-o") + 1]).parent
                    source = Path(next(x for x in command if x.endswith(".mm"))).read_text()
                    captured.append((command, seconds, plistlib.loads((app / "Info.plist").read_bytes()), source))
                    return m.engine.Result(1, "completed", b"authored compiler refusal")
                else: raw = b""
                return m.engine.Result(0, "completed", raw)
            with self.subTest(version=version), tempfile.TemporaryDirectory() as root, \
                 mock.patch.object(m.sys, "platform", "darwin"), mock.patch.object(m.os, "getuid", return_value=501), \
                 mock.patch.object(m.os, "geteuid", return_value=501), mock.patch.object(m.engine, "run", side_effect=fake_run), \
                 mock.patch.object(m, "run_queue") as queue:
                result = m.probe(UDID, Path(root).resolve(strict=True) / "out", version)
            self.assertEqual(len(captured), 1)
            command, seconds, plist, source = captured[0]
            self.assertIn("arm64-apple-ios" + version + "-simulator", command)
            self.assertEqual(plist["MinimumOSVersion"], version)
            self.assertEqual(source, m.SOURCE); self.assertEqual(seconds, 15)
            self.assertFalse(result["available"]); self.assertEqual(result["reason"], "compile_failed")
            queue.assert_not_called()

    def test_full_typed_positive_requires_advancing_time_and_complete_api_path(self):
        self.assertEqual(m.parse_native(native()), value())
        self.assertEqual(m.parse_native(b"authored prefix " + native()), value())
        self.assertEqual(len(m.STAGES), 12)

    def test_callbacks_alone_or_static_invalid_timestamps_cannot_pass(self):
        for samples, flags in (([None, None], [0, 0]), ([2048, 2048], [1, 1]),
                               ([2048, 1024], [1, 1]), ([2048, 11776], [0, 1]),
                               ([2048, 11776], [1, 0]), ([0, 96001], [1, 1]),
                               ([0, float("nan")], [1, 1]), ([0, float("inf")], [1, 1]),
                               ([-1, 1], [1, 1]), ([0, 2**48], [1, 1])):
            with self.subTest(samples=samples, flags=flags):
                v = value(); v.update(sample_times=samples, time_flags=flags)
                with self.assertRaises(ValueError): m.parse_native(native(v))

    def test_every_actual_stage_status_must_return_success(self):
        for index, name in enumerate(m.STAGES):
            for status in (None, -50, 1954115647):
                with self.subTest(stage=name, status=status):
                    v = value(); v["statuses"][index] = status
                    with self.assertRaises(ValueError): m.parse_native(native(v))
        v = value(); v["statuses"][8] = None; v["available"] = False
        with self.assertRaises(ValueError): m.parse_native(native(v))

    def test_integrity_completion_and_bounds_cannot_be_faked(self):
        for key, wrong in (("buffer_error", True), ("guard_checks", 2), ("callback_status", -1),
                           ("callbacks", 0), ("reenqueue_successes", 0), ("reenqueue_attempts", 23),
                           ("started", False), ("stopped", False), ("disposed", False),
                           ("session_ready", False), ("session_error", 4), ("probe_error", 8),
                           ("observation_ms", 189), ("observation_ms", 1001), ("stream_elapsed_ms", 2001),
                           ("main_runloop_ms", 25001), ("completed", False), ("sample_rate_hz", 44100)):
            with self.subTest(key=key, wrong=wrong):
                v = value(); v[key] = wrong
                with self.assertRaises(ValueError): m.parse_native(native(v))

    def test_types_unknown_fields_duplicate_keys_and_bad_markers_refuse(self):
        for key, wrong in (("schema", True), ("available", 1), ("guard_checks", 3.0),
                           ("statuses", [False] * 12), ("time_flags", [True, 1]),
                           ("sample_times", [False, 100]), ("statuses", [0] * 13),
                           ("statuses", [2**31] * 12), ("time_flags", [2**32, 1]),
                           ("sample_times", [None]), ("phase", 99)):
            with self.subTest(key=key):
                v = value(); v[key] = wrong
                with self.assertRaises(ValueError): m.parse_native(native(v))
        for raw in (native() + native(), native().replace(b'"schema": 1', b'"schema": 1, "schema": 1'),
                    native().replace(b'"schema": 1', b'"schema": 1, "extra": 0'),
                    b"x" * 513 + native(), b"x" * (2 * m.LOG_LIMIT + 1),
                    native().replace(b"PUBLIC_IOS_AUDIOQUEUE_RESULT ", b"PUBLIC_IOS_AUDIOQUEUE_RESULT PUBLIC_IOS_AUDIOQUEUE_RESULT ")):
            with self.assertRaises(ValueError): m.parse_native(raw)

    def test_complete_phase_order_is_required_for_success(self):
        for phases in (m.SUCCESS_PHASES[:-1], m.SUCCESS_PHASES + [123],
                       m.SUCCESS_PHASES[:6] + [102, 101] + m.SUCCESS_PHASES[8:]):
            with self.assertRaises(ValueError): m.parse_native(native(phases=phases))

    def test_hung_creation_has_null_unreturned_statuses_and_no_success(self):
        v = value(); v.update(available=False, completed=False, within_budget=False, phase=100,
            started=False, stopped=False, disposed=False, guard_checks=0, callbacks=0, reenqueue_attempts=0,
            reenqueue_successes=0, time_flags=[0, 0], sample_times=[None, None], time_advancing=False,
            observation_ms=0, stream_elapsed_ms=0, main_runloop_ms=25000, statuses=[None] * 12)
        raw = native(v, m.SUCCESS_PHASES[:7])
        self.assertEqual(m.parse_native(raw), v)
        self.assertIsNone(m.parse_native(raw[:-1]))
        v["within_budget"] = True
        with self.assertRaises(ValueError): m.parse_native(native(v, m.SUCCESS_PHASES[:7]))

    def test_completed_api_failure_keeps_real_osstatus_and_attempts(self):
        v = value(); v.update(available=False, started=False, statuses=[0] * 7 + [-50, None, None, 0, 0],
            time_flags=[0, 0], sample_times=[None, None], time_advancing=False, callbacks=0,
            reenqueue_attempts=0, reenqueue_successes=0, observation_ms=0, stream_elapsed_ms=0)
        phases = [p for p in m.SUCCESS_PHASES if p not in range(116, 120)]
        self.assertEqual(m.parse_native(native(v, phases))["statuses"][7], -50)


class OrchestrationTests(unittest.TestCase):
    def test_actual_shared_gate_preserves_delayed_closed_and_admitted_counts(self):
        # Compile the exact helper inserted into the native source. This tests
        # its deterministic callback interleavings, not a Python replica or a
        # string-only claim. Apple APIs are intentionally outside this fixture.
        compiler = shutil.which("clang++") or shutil.which("c++")
        self.assertIsNotNone(compiler, "A C++17 compiler is required for the authored gate fixture")
        source = "#include <atomic>\n#include <cstdint>\n#include <cassert>\n" + m.GATE_SOURCE + r'''
int main() {
    CallbackGate gate;
    // Callback A observed closed but has not yet executed its end.
    assert(!gate.begin());
    const uint32_t closed_active = gate.word.load();
    assert(closed_active == CallbackGate::Closed + 1);
    assert(!gate.try_open());
    assert(gate.word.load() == closed_active); // CAS never discards A's count.
    gate.end();
    assert(gate.try_open());
    assert(gate.begin()); // B admitted with immutable queue/buffers published.
    gate.close();
    assert(!gate.idle());
    assert(!gate.begin()); // C arrives after closure; atomics only.
    gate.end();
    assert(!gate.idle()); // C cannot erase B's still-live ownership.
    gate.end();
    assert(gate.idle());
    assert(gate.word.load() == CallbackGate::Closed);
    // A second delayed closed callback also prevents a subsequent open.
    assert(!gate.begin());
    assert(!gate.try_open());
    gate.end();
    assert(gate.try_open());
    assert(gate.begin());
    gate.end();
    gate.close();
    assert(gate.idle());
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as root:
            path = Path(root); src = path / "gate.cpp"; executable = path / "gate"
            src.write_text(source, encoding="ascii")
            compiled = subprocess.run([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(executable)],
                                      stdin=subprocess.DEVNULL, capture_output=True, timeout=15, env=m.compile_environment())
            self.assertEqual(compiled.returncode, 0, compiled.stderr.decode("utf-8", "replace")[:2048])
            ran = subprocess.run([str(executable)], stdin=subprocess.DEVNULL, capture_output=True, timeout=2)
            self.assertEqual(ran.returncode, 0, ran.stderr.decode("utf-8", "replace")[:2048])

    def test_non_apple_and_cli_fail_closed_without_engine_or_sdk_claim(self):
        with mock.patch.object(m.sys, "platform", "linux"), mock.patch.object(m.engine, "run") as run:
            result = m.probe(UDID, Path("/authored/unused"))
        run.assert_not_called()
        for key in ("available", "avaudioengine_verified", "physical_iphone_verified", "audible_output_verified", "game_audio_verified", "sdk_tests_passed_claimed"):
            self.assertIs(result[key], False)
        for available, expected in ((True, 0), (False, 2), (None, 2), (1, 2)):
            with mock.patch.object(m, "probe", return_value=dict(available=available, sdk_tests_passed_claimed=False)), \
                 mock.patch.object(m.signal, "signal"), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.main(["--udid", UDID, "--output", "/authored/out"]), expected)

    def test_fresh_output_and_exact_device_admission(self):
        with tempfile.TemporaryDirectory() as root, mock.patch.object(m.sys, "platform", "darwin"), \
             mock.patch.object(m.os, "getuid", return_value=501), mock.patch.object(m.os, "geteuid", return_value=501), \
             mock.patch.object(m.engine, "run") as run:
            path = Path(root).resolve(strict=True)
            with self.assertRaises(ValueError): m.probe(UDID, path)
            with self.assertRaises(ValueError): m.probe("booted", path / "fresh")
            run.assert_not_called()

    def test_actual_compile_boundary_uses_distinct_scene_app_and_interleaved_native_source(self):
        commands, metadata = [], []
        def fake_run(command, deadline, seconds, **kwargs):
            commands.append((command, deadline, seconds, kwargs))
            if command[:3] == ["xcrun", "simctl", "help"]: raw = HELPS[command[3]]
            elif command[:4] == ["xcrun", "simctl", "list", "devices"]: raw = json.dumps(device()).encode()
            elif "--show-sdk-path" in command: raw = b"/authored/Simulator.sdk\n"
            elif "clang++" in command:
                app = Path(command[command.index("-o") + 1]).parent
                metadata.append((plistlib.loads((app / "Info.plist").read_bytes()),
                                 Path(next(x for x in command if x.endswith(".mm"))).read_text()))
                raw = b""
            else: raw = b""
            return m.engine.Result(0, "completed", raw)
        with tempfile.TemporaryDirectory() as root, mock.patch.object(m.sys, "platform", "darwin"), \
             mock.patch.object(m.os, "getuid", return_value=501), mock.patch.object(m.os, "geteuid", return_value=501), \
             mock.patch.dict(os.environ, {k: "forbidden" for k in ("SDKROOT", "IPHONEOS_DEPLOYMENT_TARGET", "MACOSX_DEPLOYMENT_TARGET")}), \
             mock.patch.object(m.engine, "run", side_effect=fake_run), \
             mock.patch.object(m, "run_queue", return_value=dict(available=False)) as queue:
            started = time.monotonic(); result = m.probe(UDID, Path(root).resolve(strict=True) / "out")
        queue.assert_called_once()
        self.assertEqual(result["reason"], "completed"); self.assertFalse(result["available"])
        self.assertEqual(len(metadata), 1)
        plist, source = metadata[0]
        self.assertEqual(plist["MinimumOSVersion"], "26.5")
        self.assertEqual(source, m.SOURCE)
        self.assertEqual(plist["CFBundleIdentifier"], m.BUNDLE); self.assertNotEqual(m.BUNDLE, m.engine.BUNDLE)
        self.assertEqual(plist["CFBundleExecutable"], m.EXECUTABLE)
        scene = plist["UIApplicationSceneManifest"]["UISceneConfigurations"]["UIWindowSceneSessionRoleApplication"][0]
        self.assertIn("@interface " + scene["UISceneDelegateClassName"], source)
        self.assertIn('initWithName:@"' + scene["UISceneConfigurationName"] + '"', source)
        compile_call = next(c for c in commands if "clang++" in c[0])
        for flag in ("-Werror", "-fobjc-arc", "-fblocks", "arm64-apple-ios26.5-simulator", "AudioToolbox"):
            self.assertIn(flag, compile_call[0])
        for key in ("SDKROOT", "IPHONEOS_DEPLOYMENT_TARGET", "MACOSX_DEPLOYMENT_TARGET"):
            self.assertNotIn(key, compile_call[3]["env"])
        self.assertTrue(all(c[1] <= started + 120 for c in commands))
        sign = next(c[0] for c in commands if c[0][0] == "/usr/bin/codesign")
        self.assertEqual(sign[1:5], ["--force", "--sign", "-", "--timestamp=none"])
        self.assertFalse(any("entitlement" in x for x in sign))
        self.assertIn("AudioQueueNewOutput(&format, output_callback, s, nullptr, nullptr, 0, &s->queue)", source)
        self.assertIn("format.mBytesPerFrame = 8", source)
        self.assertNotIn("kAudioFormatFlagIsNonInterleaved", source)
        self.assertNotIn("AudioQueueOfflineRender", source)
        self.assertNotIn("AVAudioEngine", source)
        callback = source.split("static void output_callback", 1)[1].split("static bool close_callbacks", 1)[0]
        for forbidden in ("printf(", "new ", "malloc(", "dispatch_", "sleep", "flockfile", "mutex", "fopen"):
            self.assertNotIn(forbidden, callback)
        self.assertIn("AudioQueueEnqueueBuffer", callback)

    def test_failed_install_retains_sanitized_progress_and_never_launches(self):
        for code, reason in ((-9, "cancelled_or_deadline"), (70, "completed"), (0, "tool_output_limit")):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as root:
                def fake_run(command, deadline, seconds, **kwargs):
                    if command[:3] == ["xcrun", "simctl", "help"]: raw = HELPS[command[3]]
                    elif command[:4] == ["xcrun", "simctl", "list", "devices"]: raw = json.dumps(device()).encode()
                    elif "--show-sdk-path" in command: raw = b"/authored/Simulator.sdk\n"
                    elif command[:3] == ["xcrun", "simctl", "install"]:
                        self.assertEqual(seconds, 20)
                        return m.engine.Result(code, reason, b"refused /Users/authored/location [0xfeed]\n")
                    else: raw = b""
                    return m.engine.Result(0, "completed", raw)
                with mock.patch.object(m.sys, "platform", "darwin"), mock.patch.object(m.os, "getuid", return_value=501), \
                     mock.patch.object(m.os, "geteuid", return_value=501), mock.patch.object(m.engine, "run", side_effect=fake_run), \
                     mock.patch.object(m, "run_queue") as queue:
                    result = m.probe(UDID, Path(root).resolve(strict=True) / "out")
                queue.assert_not_called(); self.assertFalse(result["available"])
                self.assertEqual(result["tool_failure"]["exit_code"], code)
                self.assertEqual(result["tool_failure"]["reason"], reason)
                self.assertNotIn("/Users", result["tool_failure"]["progress"])
                self.assertNotIn("0xfeed", result["tool_failure"]["progress"])


@unittest.skipUnless(os.name == "posix", "POSIX owned-process fixture")
class SupervisionTests(unittest.TestCase):
    def authored_queue(self, code, *, parse=None):
        original = subprocess.Popen
        children, commands = [], []
        def launch(command, **kwargs):
            commands.append(command)
            child = original([sys.executable, "-c", code], **kwargs); children.append(child)
            return child
        with tempfile.TemporaryDirectory() as root, mock.patch.object(m.subprocess, "Popen", side_effect=launch), \
             mock.patch.object(m.engine, "run", return_value=m.engine.Result(0, "completed", b"")) as terminate:
            output = Path(root).resolve(strict=True)
            if parse is None: result = m.run_queue(UDID, output, time.monotonic() + 5)
            else:
                with mock.patch.object(m, "parse_native", side_effect=parse):
                    result = m.run_queue(UDID, output, time.monotonic() + 5)
            self.assertEqual(terminate.call_args.args[0], ["xcrun", "simctl", "terminate", UDID, m.BUNDLE])
            self.assertTrue(terminate.call_args.kwargs["allow_cancel"])
            self.assertEqual(commands[0], ["xcrun", "simctl", "launch", "--console", UDID, m.BUNDLE])
            return result, children[0]

    def test_actual_child_success_requires_native_report_and_zero_console_exit(self):
        result, child = self.authored_queue("import sys; sys.stdout.buffer.write(" + repr(native()) + "); sys.stdout.flush()")
        self.assertTrue(result["available"]); self.assertEqual(result["console_exit_code"], 0)
        self.assertEqual(child.returncode, 0)

    def test_nonzero_or_missing_report_child_cannot_pass(self):
        for code in ("raise SystemExit(3)", "print('authored no report')",
                     "import sys; sys.stdout.buffer.write(" + repr(native()) + "); sys.stdout.flush(); raise SystemExit(3)"):
            result, child = self.authored_queue(code)
            self.assertFalse(result["available"]); self.assertIsNotNone(child.returncode)

    def test_report_followed_by_hanging_console_fails_and_reaps(self):
        started = time.monotonic()
        result, child = self.authored_queue("import sys,time; sys.stdout.buffer.write(" + repr(native()) + "); sys.stdout.flush(); time.sleep(10)")
        self.assertFalse(result["available"]); self.assertEqual(result["reason"], "console_exit_timeout")
        self.assertEqual(child.returncode, -signal.SIGKILL)
        self.assertLess(time.monotonic() - started, 2.5)

    def test_parse_failure_still_reaps_and_terminates_exact_app(self):
        original = subprocess.Popen; children = []
        def launch(command, **kwargs):
            child = original([sys.executable, "-c", "import time; time.sleep(10)"], **kwargs); children.append(child); return child
        with tempfile.TemporaryDirectory() as root, mock.patch.object(m.subprocess, "Popen", side_effect=launch), \
             mock.patch.object(m.engine, "run", return_value=m.engine.Result(0, "completed", b"")) as terminate, \
             mock.patch.object(m, "parse_native", side_effect=ValueError("authored refusal")):
            with self.assertRaises(ValueError): m.run_queue(UDID, Path(root).resolve(strict=True), time.monotonic() + 5)
        self.assertEqual(children[0].returncode, -signal.SIGKILL)
        self.assertEqual(terminate.call_args.args[0], ["xcrun", "simctl", "terminate", UDID, m.BUNDLE])


if __name__ == "__main__":
    unittest.main()
