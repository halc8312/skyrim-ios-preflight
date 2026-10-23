"""Authored parser/ownership/bounds checks; never samples an Apple process."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("public_audio_hang", HERE / "watch-public-audio-hang.py")
m = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = m
spec.loader.exec_module(m)
UDID = "11111111-2222-3333-4444-555555555555"
EXE = ("/Users/public/Library/Developer/CoreSimulator/Devices/" + UDID
       + "/data/Containers/Bundle/Application/AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE/OwnedGameHost.app/OwnedGameHost")
ROW = f" 1234 501 501 Fri Oct 23 12:34:56 2026 {EXE}\n".encode()
# Authored syntax excerpt, deliberately not evidence of an installed Apple tool.
MAN = (b"sample(1) sample pid [duration [samplingInterval]] [-file filename] [-mayDie]\n"
       b"Duration in seconds; sampling rate in milliseconds. -mayDie collects symbol information early.")


def authored_report():
    return (f"Process: OwnedGameHost [1234]\nPath: {EXE}\nUser ID: 501\nCall graph:\n"
            "  99 Thread_123  DispatchQueue_1: authored.queue\n"
            "  + 99 -[AVAudioEngine outputNode]  (in AVFAudio) + 42 [0x123abc]\n"
            "  +   99 mach_msg_trap  (in libsystem_kernel.dylib) + 8 [0x123abc]\n"
            "  +     99 __DISPATCH_WAIT_FOR_QUEUE__  (in libdispatch.dylib) + 1 [0x123abc]\n"
            "Binary Images:\nAVAudioOutputNode\n").encode()


class ParserTests(unittest.TestCase):
    def setUp(self):
        m.STOP = m.FINISH_REQUESTED = False

    def tearDown(self):
        m.STOP = m.FINISH_REQUESTED = False

    def test_exact_process_identity_accepts_only_selected_user_app_and_device(self):
        expected = m.Identity(1234, 501, "Fri Oct 23 12:34:56 2026", EXE)
        self.assertEqual(m.parse_identity(ROW, 1234, 501, UDID), expected)
        for bad in (ROW.replace(b"1234", b"1235", 1), ROW.replace(b"501 501", b"0 501"),
                    ROW.replace(b"501 501", b"501 502"), ROW.replace(UDID.encode(), b"99999999-2222-3333-4444-555555555555"),
                    ROW.replace(b"OwnedGameHost.app/OwnedGameHost", b"Other.app/OwnedGameHost"),
                    ROW.replace(b"OwnedGameHost\n", b"OwnedGameHost --argument\n"),
                    ROW.replace(b"/Users/public/", b"/Users/../public/"), ROW + ROW):
            with self.subTest(bad=bad):
                self.assertIsNone(m.parse_identity(bad, 1234, 501, UDID))

    def test_pid_listing_rejects_duplicates_options_and_excess(self):
        self.assertEqual(m.parse_pids(b"123\n456\n"), [123, 456])
        for bad in (b"123\n123\n", b"-1\n", b"0\n", b"123 abc\n", b"2147483648\n", b"1\n" * 17):
            with self.assertRaises(m.Refusal):
                m.parse_pids(bad)

    def test_matching_two_targets_refuses_without_sampling(self):
        one = m.Identity(123, 501, "date", EXE)
        two = m.Identity(124, 501, "date", EXE)
        with mock.patch.object(m, "owned_run", return_value=(0, b"123\n124\n")), \
             mock.patch.object(m, "process_identity", side_effect=[one, two]):
            with self.assertRaisesRegex(m.Refusal, "ambiguous_target"):
                m.find_target(UDID, time.monotonic() + 1)

    def test_manual_requires_order_units_and_explicit_file(self):
        self.assertTrue(m.sample_manual_valid(MAN))
        self.assertTrue(m.sample_manual_valid(MAN.replace(b"sample(1)", b"s\bsample(1)")))
        for old, new in ((b"seconds", b"ticks"), (b"milliseconds", b"ticks"),
                         (b"[duration [samplingInterval]]", b"[samplingInterval [duration]]"),
                         (b"-file", b"-other"), (b"filename", b"unknown"),
                         (b"-mayDie", b"-mayDieLater"), (b"symbol", b"unknown")):
            self.assertFalse(m.sample_manual_valid(MAN.replace(old, new)))

    def test_phase_delay_advance_and_repeated_generation(self):
        phase = m.PhaseWait()
        phase.observe([[(3, 21)], []], 10.0)
        self.assertFalse(phase.due(10.499))
        self.assertTrue(phase.due(10.5))
        phase.observe([[(3, 25)], []], 10.6)
        self.assertFalse(phase.due(10.9))
        phase.observe([[(3, 22)], []], 10.95)
        self.assertFalse(phase.due(20))
        phase.observe([[(4, 21)], [(4, 21)]], 20)
        self.assertTrue(phase.due(20.5))
        with self.assertRaisesRegex(m.Refusal, "ambiguous_phase"):
            phase.observe([[(4, 21)], [(4, 22)]], 21)

    def test_complete_incremental_lines_only_and_truncation_refuses(self):
        with tempfile.TemporaryDirectory() as root:
            root = str(Path(root).resolve(strict=True))
            path = Path(root) / "xcodebuild.stdout.log"
            cursor = m.Cursor()
            self.assertEqual(m.read_phases(path, cursor), [])
            path.write_bytes(b"noise\nPUBLIC_AUDIO_PHASE 2 21")
            self.assertEqual(m.read_phases(path, cursor), [])
            with path.open("ab") as out:
                out.write(b"\nPUBLIC_AUDIO_PHASE 2 25\n")
            self.assertEqual(m.read_phases(path, cursor), [(2, 21), (2, 25)])
            self.assertEqual(m.read_phases(path, cursor), [])
            path.write_bytes(b"")
            with self.assertRaisesRegex(m.Refusal, "truncated"):
                m.read_phases(path, cursor)

    def test_symlink_and_tail_gap_refuse(self):
        with tempfile.TemporaryDirectory() as root:
            root = str(Path(root).resolve(strict=True))
            path = Path(root) / "log"
            target = Path(root) / "target"
            target.write_bytes(b"PUBLIC_AUDIO_PHASE 2 21\n")
            path.symlink_to(target)
            with self.assertRaisesRegex(m.Refusal, "symlink"):
                m.read_phases(path, m.Cursor())
            target.write_bytes(b"x" * (m.TAIL_LIMIT + 1))
            with self.assertRaisesRegex(m.Refusal, "tail_gap"):
                m.read_phases(target, m.Cursor())

    def test_bounded_ascii_prefix_and_malformed_markers(self):
        with tempfile.TemporaryDirectory() as root:
            root = str(Path(root).resolve(strict=True))
            path = Path(root) / "log"
            for prefix in (b"", b"2026-10-23 12:34:56 OwnedGameHost[1234:5] ", b"x" * 512):
                path.write_bytes(prefix + b"PUBLIC_AUDIO_PHASE 2 21\n")
                self.assertEqual(m.read_phases(path, m.Cursor()), [(2, 21)])
            for bad in (b"x" * 513 + b"PUBLIC_AUDIO_PHASE 2 21\n",
                        b"\x1b[0mPUBLIC_AUDIO_PHASE 2 21\n", b"PUBLIC_AUDIO_PHASE 2 999\n",
                        b"PUBLIC_AUDIO_PHASE 18446744073709551616 21\n",
                        b"PUBLIC_AUDIO_PHASE 2 21 extra\n", b"PUBLIC_AUDIO_PHASE bad 21\n",
                        b"PUBLIC_AUDIO_PHASE 2 21 PUBLIC_AUDIO_PHASE 2 25\n"):
                path.write_bytes(bad)
                with self.assertRaisesRegex(m.Refusal, "malformed_public_phase"):
                    m.read_phases(path, m.Cursor())

    def test_sample_uses_one_exact_numeric_pid_and_fixed_duration(self):
        target = m.Identity(1234, 501, "date", EXE)
        self.assertEqual(m.sample_command(target, Path("/authored/sample.txt")),
                         ["/usr/bin/sample", "1234", "1", "10", "-mayDie", "-file", "/authored/sample.txt"])

    def test_sample_report_identity_and_fixed_symbol_summary(self):
        target = m.Identity(1234, 501, "date", EXE)
        raw = authored_report()
        summary = m.summarize_sample(raw, target)
        self.assertEqual(summary["output_node_stack_lines"], 1)
        self.assertEqual(summary["mach_message_wait_stack_lines"], 1)
        self.assertEqual(summary["dispatch_wait_stack_lines"], 1)
        self.assertEqual(summary["selected_stack_line_count"], 3)
        self.assertIn("thread=1 depth=4 samples=99 -[AVAudioEngine outputNode] (in AVFAudio)",
                      summary["selected_stack_lines"])
        excerpt = "\n".join(summary["selected_stack_lines"])
        for forbidden in ("0x123abc", EXE, "Thread_123", "authored.queue", "+ 42"):
            self.assertNotIn(forbidden, excerpt)
        for bad in (raw.replace(b"[1234]", b"[1235]"), raw.replace(UDID.encode(), b"other"),
                    raw.replace(b"Call graph:", b"Other:"), b"x" * (m.SAMPLE_LIMIT + 1),
                    raw + raw, raw.replace(b"Process:", b"Process: wrong\nProcess:"),
                    raw.replace(b"Path:", b"Path: /wrong\nPath:"),
                    raw.replace(b"User ID: 501", b"User ID: 502"),
                    raw.replace(b"Call graph:\n", b"Call graph: injected\n")):
            with self.assertRaises(m.Refusal):
                m.summarize_sample(bad, target)

    def test_stack_excerpt_prioritizes_audio_and_strips_unapproved_frames(self):
        main = b"  99 Thread_1\n" + b"  + 99 CFRunLoopRun (in CoreFoundation) + 4 [0xabc]\n" * 70
        audio = (b"  50 Thread_2\n  + 50 -[AVAudioEngine outputNode] (in AVFAudio) + 8 [0xaaa]\n"
                 b"  + 50 -[OHOwnedAudioController beginEngine] (in OwnedGameHost) + 1 (Private.swift:1)\n"
                 b"  + 50 -[UnknownAudioController beginEngine] (in OwnedGameHost) + 1\n"
                 b"  + 50 secretMethod (in OwnedGameHost) + 1\n"
                 b"  + 50 /private/path (in AVFAudio)\n"
                 b"  + 50 method0x1234 (in AVFAudio)\n"
                 b"  + 50 unknown (in PrivateLibrary)\n")
        lines = m.selected_stack_lines(main + audio)
        self.assertEqual(len(lines), m.STACK_LINE_LIMIT)
        self.assertTrue(lines[0].startswith("thread=2 "))
        self.assertIn("OHOwnedAudioController beginEngine", lines[1])
        excerpt = "\n".join(lines)
        for forbidden in ("secretMethod", "Private", "/private", "0x", "unknown", "UnknownAudioController"):
            self.assertNotIn(forbidden, excerpt)
        self.assertLessEqual(sum(len(line) for line in lines), m.STACK_TEXT_LIMIT)

    def test_signal_handlers_are_installed_before_watch(self):
        with mock.patch.object(m.signal, "signal") as register, \
             mock.patch.object(m, "watch") as watch, contextlib.redirect_stdout(io.StringIO()):
            def observe(*_args):
                installed = [call.args[0] for call in register.call_args_list]
                self.assertEqual(installed, [signal.SIGTERM, signal.SIGINT, signal.SIGUSR1])
                raise m.Refusal("authored_stop")
            watch.side_effect = observe
            self.assertEqual(m.main(["--audio-dir", "/authored/xctest-device-features/audio",
                                     "--output", "/authored/out", "--udid", UDID]), 2)

    def test_graceful_finish_stops_idle_watch_without_process_query(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(m.sys, "platform", "darwin"), \
             mock.patch.object(m.os, "getuid", return_value=501), \
             mock.patch.object(m.os, "geteuid", return_value=501), \
             mock.patch.object(m, "owned_run") as run, mock.patch.object(m, "find_target") as target:
            root = str(Path(root).resolve(strict=True))
            def finish(*_args, **_kwargs):
                m.on_signal(signal.SIGUSR1, None)
                return 0, MAN
            run.side_effect = finish
            with self.assertRaisesRegex(m.Refusal, "finish_requested"):
                m.watch(Path(root) / "xctest-device-features/audio", Path(root) / "out", UDID, 120, {})
            target.assert_not_called()
            self.assertFalse(m.STOP)

    def test_capture_grace_keeps_mandatory_post_identity_and_absolute_budget(self):
        target = m.Identity(1234, 501, "date", EXE)
        with tempfile.TemporaryDirectory() as root:
            output = Path(root).resolve(strict=True)
            report = {}
            def sample(command, deadline, allowance, **kwargs):
                self.assertEqual(deadline, 116.0)
                self.assertEqual(allowance, 15)
                self.assertTrue(kwargs["finish_current"])
                (output / "sample.txt").write_bytes(authored_report())
                m.on_signal(signal.SIGUSR1, None)
                return 0, b"authored tool output"
            with mock.patch.object(m.time, "monotonic", return_value=100.0), \
                 mock.patch.object(m, "owned_run", side_effect=sample), \
                 mock.patch.object(m, "process_identity", return_value=target) as identity:
                m.capture_sample(target, output, UDID, 200, report)
            identity.assert_called_once_with(1234, 501, UDID, 116.0, finish_current=True)
            self.assertEqual(report["sample_collected"], 1)
            self.assertEqual(report["post_sample_identity_verified"], 1)
            self.assertEqual(report["selected_stack_line_count"], 3)

    def test_capture_refuses_new_admission_after_finish_and_hard_stop(self):
        target = m.Identity(1234, 501, "date", EXE)
        for signal_number in (signal.SIGUSR1, signal.SIGTERM, signal.SIGINT):
            m.STOP = m.FINISH_REQUESTED = False
            m.on_signal(signal_number, None)
            with mock.patch.object(m, "owned_run") as run:
                report = {}
                with self.assertRaises(m.Refusal):
                    m.capture_sample(target, Path("/authored"), UDID, time.monotonic() + 2, report)
                run.assert_not_called()
                self.assertNotIn("sample_attempted", report)

    def test_exited_or_replaced_target_retains_raw_but_never_emits_excerpt(self):
        target = m.Identity(1234, 501, "date", EXE)
        for after in (None, m.Identity(1234, 501, "new launch", EXE)):
            with self.subTest(after=after), tempfile.TemporaryDirectory() as root:
                output = Path(root).resolve(strict=True)
                (output / "sample.txt").write_bytes(authored_report())
                with mock.patch.object(m, "owned_run", return_value=(0, b"")), \
                     mock.patch.object(m, "process_identity", return_value=after):
                    report = {}
                    with self.assertRaisesRegex(m.Refusal, "target_changed_after_sample"):
                        m.capture_sample(target, output, UDID, time.monotonic() + 2, report)
                self.assertEqual((output / "sample.txt").read_bytes(), authored_report())
                self.assertNotIn("selected_stack_lines", report)
                self.assertNotIn("sample_collected", report)

    def test_capture_deadline_is_clipped_to_remaining_watch_budget(self):
        target = m.Identity(1234, 501, "date", EXE)
        with tempfile.TemporaryDirectory() as root:
            output = Path(root).resolve(strict=True)
            (output / "sample.txt").write_bytes(authored_report())
            with mock.patch.object(m.time, "monotonic", return_value=100.0), \
                 mock.patch.object(m, "owned_run", return_value=(0, b"")) as run, \
                 mock.patch.object(m, "process_identity", return_value=target) as identity:
                m.capture_sample(target, output, UDID, 100.25, {})
            self.assertEqual(run.call_args.args[1:3], (100.25, 15))
            self.assertEqual(identity.call_args.args[-1], 100.25)

    def test_non_apple_execution_is_diagnostic_refusal_not_test_success(self):
        with tempfile.TemporaryDirectory() as root, mock.patch.object(m.sys, "platform", "linux"):
            root = str(Path(root).resolve(strict=True))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = m.main(["--audio-dir", root + "/xctest-device-features/audio",
                               "--output", root + "/diagnostic", "--udid", UDID])
            self.assertEqual(code, 2)
            report = json.loads(out.getvalue().split(" ", 1)[1])
            self.assertEqual(report["sample_attempted"], 0)
            self.assertEqual(report["sdk_test_success_claimed"], 0)
            self.assertFalse((Path(root) / "diagnostic").exists())

    def test_installed_manual_gate_precedes_readiness_or_process_query(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(m.sys, "platform", "darwin"), \
             mock.patch.object(m.os, "getuid", return_value=501), \
             mock.patch.object(m.os, "geteuid", return_value=501), \
             mock.patch.object(m, "owned_run", return_value=(0, b"unrecognized")) as run, \
             mock.patch.object(m, "find_target") as target:
            root = str(Path(root).resolve(strict=True))
            output = Path(root) / "diagnostic"
            with self.assertRaisesRegex(m.Refusal, "installed_sample_manual_unrecognized"):
                m.watch(Path(root) / "xctest-device-features/audio", output, UDID, 120, {})
            self.assertEqual(run.call_args.args[0],
                             ["/usr/bin/man", "-M", "/usr/share/man", "-P", "/bin/cat", "1", "sample"])
            self.assertEqual(run.call_count, 1)
            target.assert_not_called()
            self.assertFalse((output / "watch-ready.json").exists())


@unittest.skipUnless(os.name == "posix", "POSIX owned-child bounds")
class ChildBoundsTests(unittest.TestCase):
    def setUp(self):
        m.STOP = m.FINISH_REQUESTED = False

    def tearDown(self):
        m.STOP = m.FINISH_REQUESTED = False

    def test_admitted_child_finishes_after_graceful_signal(self):
        timer = threading.Timer(0.03, m.on_signal, (signal.SIGUSR1, None))
        timer.start()
        try:
            code, raw = m.owned_run([sys.executable, "-c", "import time; time.sleep(.1); print('finished')"],
                                    time.monotonic() + 2, 1, finish_current=True)
            self.assertEqual((code, raw), (0, b"finished\n"))
            self.assertTrue(m.FINISH_REQUESTED)
        finally:
            timer.cancel(); timer.join()

    def test_grace_does_not_extend_parent_deadline(self):
        started = time.monotonic()
        m.FINISH_REQUESTED = True
        with self.assertRaisesRegex(m.Refusal, "deadline"):
            m.owned_run([sys.executable, "-c", "import time; time.sleep(2)"],
                        started + 0.15, 15, finish_current=True)
        self.assertLess(time.monotonic() - started, 1.5)

    def test_hard_stop_cancels_admitted_child_even_after_stdout_closed(self):
        timer = threading.Timer(0.08, m.on_signal, (signal.SIGTERM, None))
        timer.start()
        started = time.monotonic()
        try:
            with self.assertRaisesRegex(m.Refusal, "cancelled"):
                m.owned_run([sys.executable, "-c", "import os,time; os.close(1); os.close(2); time.sleep(2)"],
                            started + 2, 15, finish_current=True)
            self.assertLess(time.monotonic() - started, 1.5)
        finally:
            timer.cancel(); timer.join()

    def test_authored_child_timeout_reaps_only_owned_process(self):
        original = subprocess.Popen
        children = []
        def launch(*args, **kwargs):
            child = original(*args, **kwargs)
            children.append(child)
            return child
        started = time.monotonic()
        with mock.patch.object(m.subprocess, "Popen", side_effect=launch):
            with self.assertRaisesRegex(m.Refusal, "deadline"):
                m.owned_run([sys.executable, "-c", "import time; time.sleep(30)"], started + 2, 0.15)
        self.assertLess(time.monotonic() - started, 2)
        self.assertEqual(len(children), 1)
        self.assertEqual(children[0].returncode, -signal.SIGKILL)
        with self.assertRaises(ProcessLookupError):
            os.kill(children[0].pid, 0)

    def test_authored_child_pipe_and_regular_file_output_are_bounded(self):
        with self.assertRaisesRegex(m.Refusal, "output_limit"):
            m.owned_run([sys.executable, "-c", "import os; os.write(1,b'x'*200000)"],
                        time.monotonic() + 2, 1)
        with tempfile.TemporaryDirectory() as root:
            root = str(Path(root).resolve(strict=True))
            path = Path(root) / "authored.bin"
            code, _ = m.owned_run([sys.executable, "-c", "import sys; open(sys.argv[1],'wb').write(b'x'*3000000)", str(path)],
                                  time.monotonic() + 2, 1)
            self.assertNotEqual(code, 0)
            self.assertLessEqual(path.stat().st_size, m.SAMPLE_LIMIT)
            self.assertLessEqual(m.SAMPLE_LIMIT + 3 * m.TOOL_LIMIT, 2 * 1024 * 1024)

    @unittest.skipUnless(hasattr(os, "fork"), "authored fork descendant")
    def test_exited_leader_pipe_holding_descendant_still_has_owned_group_killed(self):
        original_popen, original_kill = subprocess.Popen, os.killpg
        children, killed = [], []
        def launch(*args, **kwargs):
            child = original_popen(*args, **kwargs)
            children.append(child)
            return child
        def kill(group, sig):
            result = original_kill(group, sig)
            killed.append((group, sig))
            return result
        script = "import os,time; child=os.fork(); time.sleep(2) if child==0 else os._exit(0)"
        try:
            with mock.patch.object(m.subprocess, "Popen", side_effect=launch), \
                 mock.patch.object(m.os, "killpg", side_effect=kill):
                with self.assertRaisesRegex(m.Refusal, "deadline"):
                    m.owned_run([sys.executable, "-c", script], time.monotonic() + 2, 0.25)
            self.assertEqual(len(children), 1)
            self.assertEqual(children[0].returncode, 0)
            self.assertEqual(killed, [(children[0].pid, signal.SIGKILL)])
        finally:
            for child in children:
                # Do not poll/reap before this guard: only an unreaped leader
                # reserves the PID, and a successful group kill needs no retry.
                if child.returncode is None:
                    if (child.pid, signal.SIGKILL) not in killed:
                        try:
                            original_kill(child.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    child.wait(timeout=2.5)


if __name__ == "__main__":
    unittest.main()
