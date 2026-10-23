#!/usr/bin/env python3
"""Optional public XCTest diagnostic; never modifies or passes an SDK test.

Start before the fresh device-features driver, then SIGTERM and join from its CI
EXIT trap. Wait at most4s for output/watch-ready.json before launching tests;
if unavailable, continue the mandatory SDK tests with diagnostics unavailable.
Reads only audio/xcodebuild.{stdout,stderr}.log. One same-user app PID
on the selected Simulator may be sampled; no process-name sampling, sudo, env
dump, system log, private input, or target signal is used. Installed Apple's
/usr/share/man sample(1) must confirm the command syntax before sampling.

The marker may have at most512 ASCII prefix bytes from Xcode forwarding; this
prefix is not attestation. A separate exact process identity check is required.
The 0.5s delay measures *observed* phase persistence, not time inside AVFAudio.
xcodebuild can buffer output. PID rechecks narrow, but cannot atomically remove,
the race between validating a PID and sample attaching to it. No capture is
claimed if post-sample identity/report checks fail. Sampling perturbs timing.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import selectors
import signal
import stat
import subprocess
import sys
import time

TAIL_LIMIT = 64 * 1024
TOOL_LIMIT = 64 * 1024
# Leave room for the retained manual, child console and small numeric report.
SAMPLE_LIMIT = 2 * 1024 * 1024 - 3 * TOOL_LIMIT
MAX_WATCH_SECONDS = 120
PHASE_DELAY = 0.5
UUID = r"[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}"
PHASE = re.compile(rb"[\t\x20-\x7e]{0,512}PUBLIC_AUDIO_PHASE ([0-9]{1,20}) ([0-9]{1,3})\r?\n")
KNOWN_PHASES = {1, 10, 11, 12, 13, 14, 20, 21, 22, 23, 24, 25, 30, 31, 40, 70, 71, 72, 73, 100, 101}
STOP = False


class Refusal(Exception):
    """Only fixed public categories are emitted, never exception/path contents."""


def on_signal(_number, _frame):
    global STOP
    STOP = True


def no_symlink_parents(path: Path) -> None:
    for parent in (path, *path.parents):
        try:
            if stat.S_ISLNK(parent.lstat().st_mode):
                raise Refusal("symlink_path")
        except FileNotFoundError:
            continue


def regular_open(path: Path):
    no_symlink_parents(path)
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    fd = os.open(path, flags)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
        os.close(fd)
        raise Refusal("unsafe_log_or_report")
    return os.fdopen(fd, "rb"), info


@dataclass
class Cursor:
    identity: tuple[int, int] | None = None
    offset: int = 0


def read_phases(path: Path, cursor: Cursor) -> list[tuple[int, int]]:
    """At most64KiB per exact file/poll; retain incomplete trailing lines."""
    try:
        stream, info = regular_open(path)
    except FileNotFoundError:
        if cursor.identity is not None:
            raise Refusal("log_disappeared")
        return []
    with stream:
        identity = (info.st_dev, info.st_ino)
        if cursor.identity not in (None, identity) or info.st_size < cursor.offset:
            raise Refusal("log_replaced_or_truncated")
        cursor.identity = identity
        start = max(cursor.offset, info.st_size - TAIL_LIMIT)
        stream.seek(start)
        raw = stream.read(min(TAIL_LIMIT, info.st_size - start))
        if start > cursor.offset:
            # A gap can hide completion; never trigger from a truncated window.
            cursor.offset = info.st_size
            raise Refusal("log_tail_gap")
        end = raw.rfind(b"\n") + 1
        cursor.offset = start + end
        result = []
        for line in raw[:end].splitlines(keepends=True):
            if b"PUBLIC_AUDIO_PHASE" not in line:
                continue
            match = PHASE.fullmatch(line)
            if (len(line) > 1024 or line.count(b"PUBLIC_AUDIO_PHASE") != 1 or match is None
                    or int(match[1]) >= 2**64 or int(match[2]) not in KNOWN_PHASES):
                raise Refusal("malformed_public_phase")
            result.append((int(match[1]), int(match[2])))
        return result


@dataclass
class PhaseWait:
    value: tuple[int, int] | None = None
    since: float = 0.0

    def observe(self, streams: list[list[tuple[int, int]]], now: float) -> None:
        active = [rows for rows in streams if rows]
        if len(active) > 1 and active[0] != active[1]:
            raise Refusal("ambiguous_phase_stream_order")
        for value in (active[0] if active else []):
            self.value = value if value[1] in (21, 25) else None
            self.since = now

    def due(self, now: float) -> bool:
        return self.value is not None and now - self.since >= PHASE_DELAY


def sample_manual_valid(raw: bytes) -> bool:
    # man may use overstriking for bold/underline when a pager is disabled.
    text = raw.decode("utf-8", "replace")
    while "\b" in text:
        updated = re.sub(r".?\x08", "", text)
        if updated == text:
            return False
        text = updated
    text = " ".join(text.lower().split())
    return ("sample" in text[:300]
            and re.search(r"\[duration\s+\[samplinginterval\]\]", text) is not None
            and "seconds" in text and "milliseconds" in text
            and "-file" in text and "filename" in text)


def child_file_limit():
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (SAMPLE_LIMIT, SAMPLE_LIMIT))


def owned_run(command: list[str], deadline: float, allowance: float, *, env=None) -> tuple[int, bytes]:
    """Bound memory/time; kill only this helper's newly created child group."""
    end = min(deadline, time.monotonic() + allowance)
    if STOP or time.monotonic() >= end:
        raise Refusal("cancelled_or_deadline")
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True,
                               env=env, preexec_fn=child_file_limit)
    output = bytearray()
    completed = False
    try:
        assert process.stdout is not None
        os.set_blocking(process.stdout.fileno(), False)
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while selector.get_map():
                if STOP or time.monotonic() >= end:
                    raise Refusal("cancelled_or_deadline")
                for key, _ in selector.select(min(0.05, max(0.0, end - time.monotonic()))):
                    chunk = os.read(key.fd, 8192)
                    if not chunk:
                        selector.unregister(key.fileobj)
                    else:
                        if len(output) + len(chunk) > TOOL_LIMIT:
                            raise Refusal("tool_output_limit")
                        output.extend(chunk)
            code = process.wait(timeout=max(0.01, end - time.monotonic()))
            completed = True
            return code, bytes(output)
    finally:
        if not completed:
            # The unreaped leader keeps its PID reserved even if it already
            # exited. Its descendant may still hold the pipe: kill our group
            # BEFORE poll/wait, not only while the leader itself is running.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=0.5)
        if process.stdout is not None:
            process.stdout.close()


def parse_pids(raw: bytes) -> list[int]:
    lines = raw.splitlines()
    if len(lines) > 16 or any(re.fullmatch(rb"[1-9][0-9]{0,9}", line) is None for line in lines):
        raise Refusal("invalid_process_listing")
    result = [int(line) for line in lines]
    if len(set(result)) != len(result) or any(pid > 2**31 - 1 for pid in result):
        raise Refusal("invalid_process_listing")
    return result


@dataclass(frozen=True)
class Identity:
    pid: int
    uid: int
    started: str
    executable: str


def parse_identity(raw: bytes, pid: int, uid: int, udid: str) -> Identity | None:
    text = raw.decode("utf-8", "strict")
    row = re.fullmatch(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+"
                       r"([A-Za-z]{3}\s+[A-Za-z]{3}\s+\d{1,2}\s+\d{2}:\d{2}:\d{2}\s+\d{4})\s+"
                       r"(/[^\r\n\x00]+)\s*", text)
    if row is None or tuple(map(int, row.group(1, 2, 3))) != (pid, uid, uid):
        return None
    executable = row[5].rstrip()
    if (any(ord(c) < 32 or ord(c) == 127 for c in executable)
            or any(x in executable for x in ("/../", "/./", "//"))
            or executable.count("/CoreSimulator/Devices/") != 1):
        return None
    pattern = (r"/[^\r\n]+/CoreSimulator/Devices/(?i:" + re.escape(udid)
               + r")/data/Containers/Bundle/Application/" + UUID
               + r"/OwnedGameHost\.app/OwnedGameHost")
    if re.fullmatch(pattern, executable) is None:
        return None
    return Identity(pid, uid, " ".join(row[4].split()), executable)


def process_identity(pid: int, uid: int, udid: str, deadline: float) -> Identity | None:
    environment = dict(os.environ, LC_ALL="C", LANG="C")
    code, raw = owned_run(["/bin/ps", "-ww", "-p", str(pid), "-o", "pid=,uid=,ruid=,lstart=,comm="],
                          deadline, 0.75, env=environment)
    return parse_identity(raw, pid, uid, udid) if code == 0 else None


def find_target(udid: str, deadline: float) -> Identity | None:
    uid = os.getuid()
    code, raw = owned_run(["/usr/bin/pgrep", "-U", str(uid), "-x", "OwnedGameHost"], deadline, 0.75)
    if code == 1 and not raw:
        return None
    if code != 0:
        raise Refusal("process_query_failed")
    matches = [identity for pid in parse_pids(raw)
               if (identity := process_identity(pid, uid, udid, deadline)) is not None]
    if len(matches) > 1:
        raise Refusal("ambiguous_target")
    return matches[0] if matches else None


def sample_command(target: Identity, output: Path) -> list[str]:
    return ["/usr/bin/sample", str(target.pid), "1", "10", "-file", str(output)]


SYMBOLS = {
    "output_node": rb"AVAudioOutputNode|\[AVAudioEngine outputNode\]|GetOutputNode",
    "main_mixer": rb"\[AVAudioEngine mainMixerNode\]|GetMainMixerNode",
    "audio_initialize": rb"AudioUnitInitialize|AUAudioUnit.*allocateRenderResources|AVAudioEngineImpl.*Initialize",
    "simulator_audio": rb"SimulatorAudio|SimAudio|Simulator.*AudioUnit",
    "mach_message_wait": rb"mach_msg|semaphore_wait|__ulock_wait",
    "dispatch_wait": rb"__DISPATCH_WAIT_FOR_QUEUE__|_dispatch_sync|dispatch_sync",
    "run_loop": rb"CFRunLoop|NSRunLoop",
}


def summarize_sample(raw: bytes, target: Identity) -> dict[str, int]:
    if len(raw) > SAMPLE_LIMIT:
        raise Refusal("sample_output_limit")
    process = rb"(?m)^Process:\s+OwnedGameHost \[" + str(target.pid).encode() + rb"\]\s*$"
    path = rb"(?m)^Path:\s+" + re.escape(target.executable.encode()) + rb"\s*$"
    if re.search(process, raw) is None or re.search(path, raw) is None or b"Call graph:" not in raw:
        raise Refusal("sample_identity_or_format_mismatch")
    graph = raw.split(b"Call graph:", 1)[1].split(b"Binary Images:", 1)[0]
    return {name + "_stack_lines": sum(bool(re.search(pattern, line)) for line in graph.splitlines())
            for name, pattern in SYMBOLS.items()}


def watch(audio_dir: Path, output: Path, udid: str, seconds: int, report: dict) -> None:
    started = time.monotonic()
    deadline = started + seconds - 0.75  # child reap/public report reserve inside120s.
    if sys.platform != "darwin" or os.getuid() == 0 or os.getuid() != os.geteuid():
        raise Refusal("unsupported_platform_or_user")
    if not 1 <= seconds <= MAX_WATCH_SECONDS or re.fullmatch(UUID, udid) is None:
        raise Refusal("invalid_arguments")
    if audio_dir.name != "audio" or audio_dir.parent.name != "xctest-device-features":
        raise Refusal("wrong_audio_log_directory")
    no_symlink_parents(audio_dir)
    logs = [audio_dir / ("xcodebuild." + name + ".log") for name in ("stdout", "stderr")]
    if any(path.exists() or path.is_symlink() for path in logs):
        raise Refusal("logs_must_be_fresh")
    no_symlink_parents(output)
    if output.exists() or not output.parent.is_dir() or output == audio_dir or output in audio_dir.parents:
        raise Refusal("output_must_be_fresh")
    output.mkdir(mode=0o700)
    environment = dict(os.environ, LC_ALL="C", LANG="C", MANPAGER="/bin/cat", PAGER="/bin/cat",
                       MANPATH="/usr/share/man", MANWIDTH="100")
    code, manual = owned_run(["/usr/bin/man", "-M", "/usr/share/man", "-P", "/bin/cat", "1", "sample"],
                             deadline, 3, env=environment)
    if code != 0 or not sample_manual_valid(manual):
        raise Refusal("installed_sample_manual_unrecognized")
    (output / "sample-man.txt").write_bytes(manual)
    report["installed_man_confirmed"] = 1
    (output / "watch-ready.json").write_text('{"schema":1,"ready":1}\n', encoding="ascii")
    cursors = [Cursor(), Cursor()]
    phase = PhaseWait()
    while not STOP and time.monotonic() < deadline:
        phase.observe([read_phases(path, cursor) for path, cursor in zip(logs, cursors)], time.monotonic())
        now = time.monotonic()
        if phase.due(now):
            value, since = phase.value, phase.since
            target = find_target(udid, min(deadline, since + 2.5))
            if target is None:
                raise Refusal("no_exact_target")
            if process_identity(target.pid, target.uid, udid, min(deadline, since + 2.5)) != target:
                raise Refusal("target_changed_before_sample")
            phase.observe([read_phases(path, cursor) for path, cursor in zip(logs, cursors)], time.monotonic())
            if phase.value != value or phase.since != since:
                continue
            report.update(phase=value[1], token=value[0], pid=target.pid,
                          observed_phase_age_ms=int((time.monotonic() - since) * 1000), sample_attempted=1)
            sample = output / "sample.txt"
            code, raw = owned_run(sample_command(target, sample), deadline, 5)
            (output / "sample-command.log").write_bytes(raw)
            report["sample_exit_code"] = code
            if code != 0:
                raise Refusal("sample_command_failed")
            if process_identity(target.pid, target.uid, udid, deadline) != target:
                raise Refusal("target_changed_after_sample")
            stream, info = regular_open(sample)
            with stream:
                if not 0 < info.st_size <= SAMPLE_LIMIT:
                    raise Refusal("sample_output_limit")
                raw = stream.read(SAMPLE_LIMIT + 1)
            report.update(summarize_sample(raw, target), sample_collected=1, sample_bytes=len(raw))
            report["reason"] = "sample_collected"
            return
        time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
    raise Refusal("cancelled" if STOP else "watch_deadline")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audio-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--udid", required=True)
    parser.add_argument("--seconds", type=int, default=MAX_WATCH_SECONDS, choices=range(1, MAX_WATCH_SECONDS + 1))
    args = parser.parse_args(argv)
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    report = dict(schema=1, diagnostic_only=1, public_test_only=1, sdk_test_success_claimed=0,
                  physical_iphone_verified=0, apple_audio_verified=0, installed_man_confirmed=0,
                  sample_attempted=0, sample_collected=0, phase=0, token=0, pid=0,
                  watch_deadline_seconds=args.seconds, sample_seconds=1, sample_interval_ms=10,
                  sample_child_deadline_seconds=5, sample_and_child_output_limit_bytes=2 * 1024 * 1024)
    started = time.monotonic()
    try:
        watch(Path(os.path.abspath(args.audio_dir)), Path(os.path.abspath(args.output)), args.udid, args.seconds, report)
    except Refusal as error:
        report["reason"] = str(error)
    except Exception:
        report["reason"] = "diagnostic_tool_or_io_failure"
    report["elapsed_ms"] = int((time.monotonic() - started) * 1000)
    print("PUBLIC_AUDIO_HANG_DIAGNOSTIC " + json.dumps(report, sort_keys=True), flush=True)
    # The diagnostic never reports the SDK outcome. CI must keep its own status.
    return 0 if report["sample_collected"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
