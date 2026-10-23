#!/usr/bin/env python3
"""Optional authored UIKit/AVAudioEngine Simulator control, without XCTest.

Root supplies one fresh, owned, booted iPhone17ProMax UDID on the explicit iOS
version (default26.5; allowlist26.3/26.4/26.5). This script
installs only its isolated ad-hoc-signed app and runs plain/playback in separate
processes. No entitlements, microphone, system logs, guest code or SDK-test
changes. Actual silent source callbacks prove a Simulator output stream, never
audibility or physical-iPhone behavior. Total budget120s; each mode35s maximum.
The app keeps Main alive25s on an Apple-call hang. Sampling remains optional,
requires installed manual confirmation, and uses the exact live identity gates
from watch-public-audio-hang.py. Partial sample-tool output is retained on timeout
and a bounded sanitized excerpt is printed; it is not a validated stack.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import errno
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import selectors
import signal
import subprocess
import sys
import tempfile
import time

from audio_simulator_selection import DEFAULT_VERSION, SUPPORTED_VERSIONS, runtime_identifier

SOURCE = r'''
#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#include <dispatch/dispatch.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <time.h>
#include <unistd.h>

struct State {
    uint32_t mode = 0;
    double begin = 0;
    std::atomic<uint32_t> phase{0}, rate{0}, channels{0}, stream_ms{0};
    std::atomic<uint64_t> callbacks{0}, frames{0};
    std::atomic<int32_t> status{0};
    std::atomic<bool> done{false}, started{false}, stopped{false}, bad{false}, finished{false}, activated{false};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static State *state;  // Process-retained even if an Apple call never returns.
static double now() {
    timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
}
static void phase(State *s, uint32_t value) {
    flockfile(stdout);
    s->phase.store(value);
    std::printf("PUBLIC_AUDIO_PHASE %u %u\n", s->mode + 1, value);
    std::fflush(stdout); funlockfile(stdout);
}
static void finish(State *s) {
    if (s->finished.exchange(true)) return;
    // Hold through _exit so a late worker cannot append after the final line.
    flockfile(stdout);
    const double elapsed = now() - s->begin;
    const bool done = s->done.load(std::memory_order_acquire), budget = elapsed <= 25.0;
    const uint64_t callbacks = s->callbacks.load(), frames = s->frames.load();
    const bool available = done && budget && s->started.load() && s->stopped.load() &&
        !s->bad.load() && s->status.load() == 0 && s->stream_ms.load() <= 1000 &&
        s->rate.load() > 0 && s->channels.load() > 0 && callbacks > 0 && frames > 0;
    std::printf("PUBLIC_IOS_AUDIO_RESULT {\"schema\":1,\"mode\":%u,\"available\":%s,"
        "\"completed\":%s,\"started\":%s,\"stopped\":%s,\"format_error\":%s,\"within_budget\":%s,"
        "\"phase\":%u,\"status\":%d,\"sample_rate_hz\":%u,\"output_channels\":%u,"
        "\"render_callbacks\":%llu,\"render_frames\":%llu,\"main_runloop_ms\":%u,\"stream_elapsed_ms\":%u}\n",
        s->mode, available ? "true" : "false", done ? "true" : "false",
        s->started.load() ? "true" : "false", s->stopped.load() ? "true" : "false",
        s->bad.load() ? "true" : "false", budget ? "true" : "false", s->phase.load(),
        s->status.load(), s->rate.load(), s->channels.load(), (unsigned long long)callbacks,
        (unsigned long long)frames, uint32_t(elapsed * 1000), s->stream_ms.load());
    std::fflush(stdout);
    _exit(0);  // Never wait for an incomplete Apple call or graph destructor.
}
extern "C" void OHNativeAudioEnvironmentGraph(State *s) {
    @autoreleasepool {
        AVAudioEngine *engine = nil;
        double stream_start = 0;
        @try {
            if (s->mode == 1) {
                phase(s, 10);
                AVAudioSession *session = AVAudioSession.sharedInstance;
                NSError *error = nil;
                phase(s, 11);
                if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&error]) {
                    s->status.store(-1); return;
                }
                phase(s, 12);
                if (![session setPreferredSampleRate:48000 error:&error]) { s->status.store(-2); return; }
                phase(s, 13);
                if (![session setPreferredIOBufferDuration:512.0/48000.0 error:&error]) { s->status.store(-3); return; }
                phase(s, 14);
                if (![session setActive:YES error:&error]) { s->status.store(-4); return; }
            }
            phase(s, 20); engine = [[AVAudioEngine alloc] init];
            if (!engine) { s->status.store(-5); return; }
            phase(s, 21); AVAudioOutputNode *output = engine.outputNode; phase(s, 25);
            if (!output) { s->status.store(-6); return; }
            phase(s, 22); AVAudioFormat *hardware = [output outputFormatForBus:0];
            const double rate = hardware.sampleRate;
            const uint32_t channels = hardware.channelCount;
            if (!hardware || !std::isfinite(rate) || rate < 1 || rate > 1000000 || channels == 0 || channels > 256) {
                s->status.store(-7); return;
            }
            s->rate.store(uint32_t(rate + 0.5)); s->channels.store(channels);
            AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
            if (!format || format.isInterleaved || format.commonFormat != AVAudioPCMFormatFloat32) {
                s->status.store(-8); return;
            }
            phase(s, 23); AVAudioMixerNode *mixer = engine.mainMixerNode; phase(s, 24);
            if (!mixer) { s->status.store(-9); return; }
            phase(s, 30);
            AVAudioSourceNode *source = [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:
                ^OSStatus(BOOL *silence, const AudioTimeStamp *timestamp, AVAudioFrameCount frames, AudioBufferList *buffers) {
                    (void)timestamp;
                    if (!silence || !buffers || frames > 65536 || buffers->mNumberBuffers != 2) {
                        s->bad.store(true); return -50;
                    }
                    for (uint32_t i = 0; i < 2; ++i) {
                        const AudioBuffer &b = buffers->mBuffers[i];
                        if (!b.mData || b.mNumberChannels != 1 || b.mDataByteSize < frames * sizeof(float)) {
                            s->bad.store(true); return -50;
                        }
                    }
                    for (uint32_t i = 0; i < 2; ++i) std::memset(buffers->mBuffers[i].mData, 0, frames * sizeof(float));
                    *silence = YES;
                    s->frames.fetch_add(frames, std::memory_order_relaxed);
                    s->callbacks.fetch_add(1, std::memory_order_relaxed);
                    return 0;
                }];
            if (!source) { s->status.store(-10); return; }
            phase(s, 31); [engine attachNode:source]; [engine connect:source to:mixer format:format];
            mixer.outputVolume = 1.0f;
            phase(s, 40); [engine prepare];
            NSError *error = nil;
            phase(s, 70);
            if (![engine startAndReturnError:&error]) { s->status.store(-11); return; }
            stream_start = now(); s->started.store(true); phase(s, 71);
            [NSThread sleepForTimeInterval:0.2];
        } @catch (NSException *exception) {
            (void)exception; s->status.store(-12);
        } @finally {
            if (engine) {
                @try {
                    phase(s, 72); [engine stop];
                    if (stream_start) s->stream_ms.store(uint32_t((now() - stream_start) * 1000));
                    s->stopped.store(true); phase(s, 73);
                } @catch (NSException *exception) { (void)exception; s->status.store(-13); }
            }
            s->done.store(true, std::memory_order_release);
            dispatch_async(dispatch_get_main_queue(), ^{ finish(s); });
        }
    }
}
@interface PublicAudioSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation PublicAudioSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *controller = [[UIViewController alloc] init];
    controller.view.backgroundColor = UIColor.systemBlueColor;
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    if (![scene isKindOfClass:UIWindowScene.class] || self.window.windowScene != scene) return;
    if (state->activated.exchange(true)) return;
    state->begin = now(); phase(state, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 25LL * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ finish(state); });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ OHNativeAudioEnvironmentGraph(state); });
}
@end
@interface PublicAudioAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation PublicAudioAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)application
        configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *configuration = [[UISceneConfiguration alloc] initWithName:@"PublicAudioScene" sessionRole:session.role];
    configuration.sceneClass = UIWindowScene.class;
    configuration.delegateClass = PublicAudioSceneDelegate.class;
    return configuration;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool {
        state = new State;
        if (argc != 2 || (std::strcmp(argv[1], "plain") && std::strcmp(argv[1], "playback"))) return 2;
        state->mode = std::strcmp(argv[1], "playback") == 0 ? 1 : 0;
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(PublicAudioAppDelegate.class));
    }
}
'''

BUNDLE = "org.openai.publicprobe.ios-audio-environment"
TOOL_LIMIT = 32 * 1024
LOG_LIMIT = 32 * 1024
SAMPLE_LIMIT = 512 * 1024
TOTAL_SECONDS = 120
MODE_SECONDS = 35
STOP = False
GRAPH_PATH = [20, 21, 25, 22, 23, 24, 30, 31, 40, 70, 71, 72, 73]
PHASES = {1, 10, 11, 12, 13, 14, *GRAPH_PATH}


def helper_module():
    spec = importlib.util.spec_from_file_location("ios_audio_watch_helpers", Path(__file__).with_name("watch-public-audio-hang.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def on_signal(_number, _frame):
    global STOP
    STOP = True


def limits(size):
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (size, size))


@dataclass
class Result:
    code: int
    reason: str
    output: bytes


def reap_owned_group(child):
    if child.returncode is not None:
        return child.returncode  # Already reaped: its old PID is no longer ours.
    try:
        # Do not poll/reap first: an exited leader can still have descendants
        # holding its pipe, and its unreaped PID still reserves our PGID.
        os.killpg(child.pid, signal.SIGKILL)
    except PermissionError as denied:
        if denied.errno != errno.EPERM:
            raise
        # Darwin may return EPERM for the empty group of an exited leader.
        # Accept only that proven exit; never hide denial for a live child.
        try:
            return child.wait(timeout=0)
        except subprocess.TimeoutExpired:
            raise denied from None
    except ProcessLookupError:
        pass
    return child.wait(timeout=0.5)


def run(command, deadline, seconds, *, env=None, file_limit=2 * 1024 * 1024, allow_cancel=False):
    """Retain bounded progress on failure; signal only this unreaped child PGID."""
    end = min(deadline, time.monotonic() + seconds)
    if time.monotonic() >= end or STOP and not allow_cancel:
        return Result(-1, "cancelled_or_deadline", b"")
    child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             start_new_session=True, env=env, preexec_fn=lambda: limits(file_limit))
    output = bytearray()
    completed, reason, code = False, "completed", -1
    try:
        os.set_blocking(child.stdout.fileno(), False)
        with selectors.DefaultSelector() as selector:
            selector.register(child.stdout, selectors.EVENT_READ)
            while True:
                if time.monotonic() >= end or STOP and not allow_cancel:
                    reason = "cancelled_or_deadline"; break
                if not selector.get_map():
                    try:
                        code = child.wait(timeout=min(0.05, max(0.001, end - time.monotonic())))
                        completed = True; break
                    except subprocess.TimeoutExpired:
                        continue
                for key, _ in selector.select(min(0.05, max(0.0, end - time.monotonic()))):
                    chunk = os.read(key.fd, 4096)
                    if not chunk:
                        selector.unregister(key.fileobj)
                    else:
                        room = TOOL_LIMIT - len(output)
                        output.extend(chunk[:room])
                        if len(chunk) > room:
                            reason = "tool_output_limit"; break
                if reason != "completed":
                    break
    finally:
        try:
            if not completed:
                code = reap_owned_group(child)
        finally:
            child.stdout.close()
    return Result(code, reason, bytes(output))


def strict_json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result: raise ValueError("duplicate key")
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonstandard number")))


def validate_device(raw, udid, simulator_version=DEFAULT_VERSION):
    expected_runtime = runtime_identifier(simulator_version)
    value = strict_json(raw)
    matches = [(runtime, item) for runtime, rows in value["devices"].items() for item in rows]
    if len(matches) != 1:
        raise ValueError("selected device listing")
    runtime, device = matches[0]
    if (runtime != expected_runtime or device.get("udid", "").upper() != udid.upper()
            or device.get("state") != "Booted" or device.get("isAvailable") is not True
            or device.get("deviceTypeIdentifier") != "com.apple.CoreSimulator.SimDeviceType.iPhone-17-Pro-Max"):
        raise ValueError("wrong selected device")


def help_valid(kind, raw):
    text = " ".join(raw.decode("utf-8", "replace").lower().split())
    required = {"launch": ("launch", "--console", "device", "bundle"),
                "install": ("install", "device", "path"),
                "list": ("list", "devices", "--json", "search"),
                "terminate": ("terminate", "device", "bundle"),
                "get_app_container": ("get_app_container", "device", "bundle", "app")}
    return all(word in text for word in required[kind])


def parse_native(raw, mode):
    if len(raw) > 2 * LOG_LIMIT: raise ValueError("console limit")
    results, phases = [], []
    # simctl is still appending: incomplete final records are not failures yet.
    for line in raw[:raw.rfind(b"\n") + 1].splitlines():
        if b"PUBLIC_IOS_AUDIO_RESULT" in line:
            match = re.fullmatch(rb"[\t\x20-\x7e]{0,512}PUBLIC_IOS_AUDIO_RESULT (\{[^\r\n]+\})", line)
            if not match or line.count(b"PUBLIC_IOS_AUDIO_RESULT") != 1: raise ValueError("result marker")
            results.append(strict_json(match[1]))
        if b"PUBLIC_AUDIO_PHASE" in line:
            match = re.fullmatch(rb"[\t\x20-\x7e]{0,512}PUBLIC_AUDIO_PHASE ([12]) ([0-9]{1,3})", line)
            if not match or line.count(b"PUBLIC_AUDIO_PHASE") != 1: raise ValueError("phase marker")
            if int(match[1]) != mode + 1 or int(match[2]) not in PHASES: raise ValueError("phase value")
            phases.append(int(match[2]))
    if not results: return None
    if len(results) != 1: raise ValueError("duplicate result")
    value = results[0]
    expected = dict(schema=int, mode=int, available=bool, completed=bool, started=bool, stopped=bool,
                    format_error=bool, within_budget=bool, phase=int, status=int, sample_rate_hz=int,
                    output_channels=int, render_callbacks=int, render_frames=int, main_runloop_ms=int, stream_elapsed_ms=int)
    if (type(value) is not dict or set(value) != set(expected)
            or any(type(value[k]) is not t for k, t in expected.items())): raise ValueError("fields/types")
    if (value["schema"] != 1 or value["mode"] != mode or value["phase"] not in PHASES
            or not -13 <= value["status"] <= 0 or not 0 <= value["sample_rate_hz"] <= 1000000
            or not 0 <= value["output_channels"] <= 256 or not 0 <= value["main_runloop_ms"] <= 35000
            or not 0 <= value["stream_elapsed_ms"] <= 35000
            or any(not 0 <= value[k] < 2**64 for k in ("render_callbacks", "render_frames"))):
        raise ValueError("numeric bounds")
    ready = (value["completed"] and value["within_budget"] and value["started"] and value["stopped"]
             and not value["format_error"] and value["status"] == 0 and value["stream_elapsed_ms"] <= 1000
             and value["sample_rate_hz"] > 0 and value["output_channels"] > 0
             and value["render_callbacks"] > 0 and value["render_frames"] > 0 and value["phase"] == 73)
    if (value["available"] != ready or not phases or phases[-1] != value["phase"]
            or value["within_budget"] and value["main_runloop_ms"] > 25000
            or not value["completed"] and value["within_budget"]): raise ValueError("inconsistent result")
    if ready and phases != [1] + ([10, 11, 12, 13, 14] if mode else []) + GRAPH_PATH:
        raise ValueError("incomplete successful path")
    return value


def sanitized_progress(raw, work):
    text = raw[:4096].decode("ascii", "replace")
    for path in (str(work), str(work.resolve())): text = text.replace(path, "<probe-temp>")
    text = re.sub(r"/[^\s\"']+", "<path>", text)
    text = re.sub(r"0[xX][0-9a-fA-F]+", "<address>", text)
    encoded = json.dumps(text, ensure_ascii=True)
    while len(encoded.encode("ascii")) > 3500:
        text = text[:len(text) // 2]; encoded = json.dumps(text, ensure_ascii=True)
    return text


def bounded_read(path, maximum):
    with path.open("rb") as stream:
        raw = stream.read(maximum + 1)
    if len(raw) > maximum: raise ValueError("file output limit")
    return raw


def sample_once(helper, target, output, udid, deadline, report):
    sample = output / "sample.txt"
    outcome = run(helper.sample_command(target, sample), deadline, 15, file_limit=SAMPLE_LIMIT)
    (output / "sample-command.log").write_bytes(outcome.output)
    report.update(sample_attempted=True, sample_reason=outcome.reason, sample_exit_code=outcome.code,
                  sample_tool_progress=sanitized_progress(outcome.output, output))
    if outcome.reason != "completed" or outcome.code != 0: return
    if helper.process_identity(target.pid, target.uid, udid, deadline) != target:
        report["sample_reason"] = "target_changed_after_sample"; return
    stream, info = helper.regular_open(sample)
    with stream:
        if not 0 < info.st_size <= SAMPLE_LIMIT: raise ValueError("sample size")
        raw = stream.read(SAMPLE_LIMIT + 1)
    summary = helper.summarize_sample(raw, target)
    lines, size = [], 0
    for line in summary.pop("selected_stack_lines"):
        if size + len(line) > 3000: break
        lines.append(line); size += len(line)
    summary.update(selected_stack_lines=lines, selected_stack_line_count=len(lines))
    report.update(summary, sample_collected=True, sample_reason="sample_collected")


def run_mode(helper, udid, executable, mode, output, deadline, sampling):
    end = min(deadline, time.monotonic() + MODE_SECONDS)
    if STOP or time.monotonic() >= end: raise ValueError("mode budget exhausted")
    logdir = output / "xctest-device-features" / "audio"
    logdir.mkdir(parents=True)
    logs = [logdir / ("xcodebuild." + name + ".log") for name in ("stdout", "stderr")]
    result = dict(mode=mode, available=False, native_output_stream_verified=False, sample_attempted=False,
                  sample_collected=False, sample_reason="not_attempted", phase=0, reason="mode_deadline")
    cursors, phase = [helper.Cursor(), helper.Cursor()], helper.PhaseWait()
    child = None
    try:
        with logs[0].open("xb") as stdout, logs[1].open("xb") as stderr:
            child = subprocess.Popen(["xcrun", "simctl", "launch", "--console", udid, BUNDLE, mode],
                                     stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, start_new_session=True,
                                     preexec_fn=lambda: limits(LOG_LIMIT))
            while not STOP and time.monotonic() < end - 4.0:
                rows = [helper.read_phases(path, cursor) for path, cursor in zip(logs, cursors)]
                phase.observe(rows, time.monotonic())
                for stream in rows:
                    if stream: result["phase"] = stream[-1][1]
                raw = b"\n".join(bounded_read(path, LOG_LIMIT) for path in logs)
                native = parse_native(raw, 0 if mode == "plain" else 1)
                if native is not None:
                    result.update(native, mode=mode, reason="native_report", native_output_stream_verified=native["available"])
                    break
                if sampling and not result["sample_attempted"] and phase.due(time.monotonic()):
                    try:
                        before = phase.value, phase.since
                        target = helper.find_target(udid, min(end - 4, time.monotonic() + 2.0))
                        if target is None or target.executable != executable:
                            result.update(sample_attempted=True, sample_reason="no_exact_installed_target")
                        elif helper.process_identity(target.pid, target.uid, udid, end - 4) != target:
                            result.update(sample_attempted=True, sample_reason="target_changed_before_sample")
                        else:
                            phase.observe([helper.read_phases(path, cursor) for path, cursor in zip(logs, cursors)], time.monotonic())
                            if (phase.value, phase.since) == before:
                                sample_once(helper, target, output, udid, end - 4, result)
                    except (helper.Refusal, OSError, ValueError, UnicodeError):
                        result.update(sample_attempted=True, sample_reason="sample_tool_or_identity_refusal")
                if child.poll() is not None:
                    # Re-read once: the app flushes its report before exiting.
                    native = parse_native(b"\n".join(bounded_read(path, LOG_LIMIT) for path in logs), 0 if mode == "plain" else 1)
                    if native is not None:
                        result.update(native, mode=mode, reason="native_report", native_output_stream_verified=native["available"])
                    else: result["reason"] = "console_exited_without_native_report"
                    break
                time.sleep(0.05)
    finally:
        try:
            if child is not None:
                reap_owned_group(child)
        finally:
            # Still clean up the exact app if console cleanup refuses. An
            # unexpected live-child permission failure continues to propagate.
            stopped = run(["xcrun", "simctl", "terminate", udid, BUNDLE], end, 2, allow_cancel=True)
            result["terminate_exit_code"] = stopped.code
    if not STOP and helper.find_target(udid, end) is not None:
        raise ValueError("prior app process still present")
    (output / "result.json").write_text(json.dumps(result, sort_keys=True) + "\n", encoding="ascii")
    return result


def probe(udid, output, simulator_version=DEFAULT_VERSION):
    runtime_identifier(simulator_version)
    start, deadline = time.monotonic(), time.monotonic() + TOTAL_SECONDS - 1
    report = dict(schema=1, diagnostic_only=True, public_only=True, available=False,
                  simulator_output_stream_verified=False, physical_iphone_verified=False,
                  audible_output_verified=False, sdk_tests_passed_claimed=False, modes=[], reason="unsupported_platform")
    if sys.platform != "darwin" or os.getuid() == 0 or os.getuid() != os.geteuid(): return report
    helper = helper_module()
    helper.no_symlink_parents(output)
    if output.exists() or not output.parent.is_dir() or re.fullmatch(helper.UUID, udid) is None:
        raise ValueError("fresh output and exact UDID required")
    output.mkdir(mode=0o700)
    def checked(command, seconds, name, **kwargs):
        report["stage"] = name
        outcome = run(command, deadline, seconds, **kwargs)
        if outcome.reason != "completed" or outcome.code:
            report["tool_failure"] = dict(stage=name, reason=outcome.reason, exit_code=outcome.code,
                                          progress=sanitized_progress(outcome.output, output))
        (output / (name + ".log")).write_bytes(outcome.output)
        if outcome.reason != "completed" or outcome.code: raise ValueError(name + " failed")
        return outcome.output
    def helper_run(command, child_deadline, seconds, *, env=None, finish_current=False):
        outcome = run(command, child_deadline, seconds, env=env)
        if outcome.reason != "completed": raise helper.Refusal(outcome.reason)
        return outcome.code, outcome.output
    helper.owned_run = helper_run
    try:
        for kind in ("launch", "install", "list", "terminate", "get_app_container"):
            raw = checked(["xcrun", "simctl", "help", kind], 2, "help-" + kind)
            if not help_valid(kind, raw): raise ValueError("installed simctl help unrecognized")
        raw = checked(["xcrun", "simctl", "list", "devices", udid, "--json"], 3, "selected-device")
        validate_device(raw, udid, simulator_version)
        environment = dict(os.environ, LC_ALL="C", LANG="C", MANPAGER="/bin/cat", PAGER="/bin/cat",
                           MANPATH="/usr/share/man", MANWIDTH="100")
        manual = run(["/usr/bin/man", "-M", "/usr/share/man", "-P", "/bin/cat", "1", "sample"], deadline, 3, env=environment)
        (output / "sample-man.log").write_bytes(manual.output)
        sampling = manual.reason == "completed" and manual.code == 0 and helper.sample_manual_valid(manual.output)
        report["installed_sample_man_confirmed"] = sampling
        with tempfile.TemporaryDirectory(prefix="public-ios-audio-") as temporary:
            work = Path(temporary).resolve(strict=True)
            app = work / "OwnedGameHost.app"; app.mkdir()
            source = work / "probe.mm"; source.write_text(SOURCE, encoding="ascii")
            plist = dict(CFBundleExecutable="OwnedGameHost", CFBundleIdentifier=BUNDLE, CFBundleName="OwnedGameHost",
                         CFBundlePackageType="APPL", CFBundleVersion="1", CFBundleShortVersionString="1.0",
                         MinimumOSVersion=simulator_version, LSRequiresIPhoneOS=True, UIDeviceFamily=[1, 2],
                         CFBundleSupportedPlatforms=["iPhoneSimulator"], UILaunchScreen={},
                         UIApplicationSceneManifest=dict(UIApplicationSupportsMultipleScenes=False,
                             UISceneConfigurations={"UIWindowSceneSessionRoleApplication": [dict(
                                 UISceneConfigurationName="PublicAudioScene", UISceneClassName="UIWindowScene",
                                 UISceneDelegateClassName="PublicAudioSceneDelegate")]}))
            (app / "Info.plist").write_bytes(plistlib.dumps(plist))
            sdk = checked(["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], 2, "sdk-path").decode().strip()
            if not sdk.startswith("/") or "\n" in sdk or "\x00" in sdk: raise ValueError("SDK path")
            compile_env = os.environ.copy()
            for name in ("SDKROOT", "IPHONEOS_DEPLOYMENT_TARGET"): compile_env.pop(name, None)
            command = ["xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17", "-fobjc-arc", "-fblocks",
                       "-Wall", "-Wextra", "-Werror", "-target", f"arm64-apple-ios{simulator_version}-simulator", "-isysroot", sdk,
                       str(source), "-framework", "UIKit", "-framework", "AVFAudio", "-framework", "Foundation",
                       "-o", str(app / "OwnedGameHost")]
            report["stage"] = "compile"
            compiled = run(command, deadline, 15, env=compile_env)
            (output / "compile.log").write_bytes(compiled.output)
            if compiled.reason != "completed" or compiled.code:
                report.update(reason="compile_failed", compiler_progress=sanitized_progress(compiled.output, work)); return report
            checked(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(app)], 4, "adhoc-sign")
            checked(["xcrun", "simctl", "install", udid, str(app)], 20, "install")
            container = checked(["xcrun", "simctl", "get_app_container", udid, BUNDLE, "app"], 3, "installed-container").decode().strip()
            executable = container + "/OwnedGameHost"
            authored_ps = f"1 {os.getuid()} {os.getuid()} Fri Oct 23 12:34:56 2026 {executable}\n".encode()
            if helper.parse_identity(authored_ps, 1, os.getuid(), udid) is None: raise ValueError("installed app path")
            for mode in ("plain", "playback"):
                report["stage"] = mode
                mode_output = output / mode; mode_output.mkdir()
                result = run_mode(helper, udid, executable, mode, mode_output, deadline, sampling)
                report["modes"].append(result)
                print("PUBLIC_IOS_AUDIO_MODE " + json.dumps(result, sort_keys=True), flush=True)
                if STOP: break
        report.update(reason="completed", available=len(report["modes"]) == 2 and all(x["available"] for x in report["modes"]))
        report["simulator_output_stream_verified"] = report["available"]
    except Exception:
        report["reason"] = "diagnostic_tool_or_validation_failure"
    finally:
        report["elapsed_ms"] = int((time.monotonic() - start) * 1000)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--udid", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--simulator-version", choices=SUPPORTED_VERSIONS, default=DEFAULT_VERSION)
    args = parser.parse_args(argv)
    signal.signal(signal.SIGTERM, on_signal); signal.signal(signal.SIGINT, on_signal)
    try: report = probe(args.udid, Path(os.path.abspath(args.output)), args.simulator_version)
    except Exception:
        report = dict(schema=1, diagnostic_only=True, public_only=True, available=False,
                      sdk_tests_passed_claimed=False, reason="diagnostic_setup_failure")
    print("PUBLIC_IOS_AUDIO_ENVIRONMENT " + json.dumps(report, sort_keys=True), flush=True)
    return 0 if report.get("available") is True else 2


if __name__ == "__main__":
    raise SystemExit(main())
