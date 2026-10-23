#!/usr/bin/env python3
"""Optional public AudioQueue Simulator discriminator; never an engine/SDK pass.

Uses the same fresh, owned iOS26.5/iPhone17ProMax supplied to the separate engine
control. Its distinct UIKit/UIScene app configures playback, queues authored zero
48kHz stereo interleaved Float32, and requires valid advancing queue sample time
across 200ms, callbacks, intact buffer guards, and successful stop/disposal.
Callbacks alone do not prove playback (or audibility). No microphone, input,
offline rendering, sample capture, game data, or production audio changes.

Apple API contracts used:
https://developer.apple.com/documentation/audiotoolbox/audioqueuenewoutput(_:_:_:_:_:_:_:)
https://developer.apple.com/documentation/audiotoolbox/audioqueueoutputcallback
https://developer.apple.com/documentation/audiotoolbox/audioqueuegetcurrenttime(_:_:_:_:)
https://developer.apple.com/documentation/audiotoolbox/audioqueuestop(_:_:)
https://developer.apple.com/documentation/audiotoolbox/audioqueuedispose(_:_:)
The installed SDK compilation uses -Werror; portable tests cannot replace it.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import plistlib
import re
import signal
import subprocess
import sys
import tempfile
import time


def load_engine_helpers():
    # Explicit reuse of existing process/device/sanitizer APIs, with no function
    # replacement or hidden backend change in the two-mode engine probe.
    name = "public_audioqueue_engine_helpers"
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name("probe-ios-audio-environment.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


engine = load_engine_helpers()
BUNDLE = "org.openai.publicprobe.ios-audioqueue-environment"
EXECUTABLE = "PublicAudioQueueProbe"
TOTAL_SECONDS = 120
MODE_SECONDS = 35
LOG_LIMIT = 32 * 1024
STAGES = ("new_output", "allocate_0", "allocate_1", "allocate_2",
          "enqueue_0", "enqueue_1", "enqueue_2", "start", "time_0", "time_1", "stop", "dispose")
SUCCESS_PHASES = [1, 10, 11, 12, 13, 14, *range(100, 124)]
PHASES = set(SUCCESS_PHASES)

GATE_SOURCE = r'''
struct CallbackGate {
    static constexpr uint32_t Closed = 1u << 31;
    std::atomic<uint32_t> word{Closed};
    // Each begin, including a refused late callback, owns exactly one end.
    bool begin() { return !(word.fetch_add(1) & Closed); }
    void end() { word.fetch_sub(1); }
    bool try_open() {
        uint32_t expected = Closed;
        return word.compare_exchange_strong(expected, 0);
    }
    void close() { word.fetch_or(Closed); }
    bool idle() const { return (word.load() & ~Closed) == 0; }
};
'''

SOURCE = r'''
#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <time.h>
#include <unistd.h>
''' + GATE_SOURCE + r'''
constexpr uint32_t BufferCount = 3, Frames = 512, Payload = Frames * 2 * sizeof(float), Guard = 32;
enum Stage : uint32_t { NewOutput, Allocate0, Allocate1, Allocate2, Enqueue0, Enqueue1, Enqueue2,
                       Start, Time0, Time1, Stop, Dispose, StageCount };
struct CallResult { std::atomic<int32_t> status{0}; std::atomic<bool> returned{false}; };
struct State {
    double begin = 0;
    AudioQueueRef queue = nullptr;
    AudioQueueBufferRef buffers[BufferCount]{};
    void *data[BufferCount]{};
    CallResult calls[StageCount];
    CallbackGate gate;
    std::atomic<uint32_t> phase{0}, guard_checks{0}, observation_ms{0}, stream_ms{0};
    std::atomic<uint64_t> callbacks{0}, reenqueue_attempts{0}, reenqueue_successes{0};
    std::atomic<int32_t> callback_status{0}, error{0};
    std::atomic<int64_t> session_error{0};
    std::atomic<bool> activated{false}, finished{false}, done{false}, session_ready{false}, buffer_error{false};
    std::atomic<uint32_t> time_flags[2]{{0}, {0}};
    std::atomic<double> sample_time[2]{{0.0}, {0.0}};
    std::atomic<bool> time_valid[2]{{false}, {false}};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<int32_t>::is_always_lock_free);
static_assert(std::atomic<int64_t>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static State *state; // Process-retained if an Apple call stalls; never free under a callback.
static double now() {
    timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
}
static void phase(State *s, uint32_t value) {
    flockfile(stdout); s->phase.store(value);
    std::printf("PUBLIC_AUDIOQUEUE_PHASE %u\n", value);
    std::fflush(stdout); funlockfile(stdout);
}
static bool returned(State *s, Stage stage, OSStatus status) {
    s->calls[stage].status.store(status);
    s->calls[stage].returned.store(true, std::memory_order_release);
    phase(s, 101 + 2 * stage);
    return status == noErr;
}
static bool successful(State *s, Stage stage) {
    return s->calls[stage].returned.load(std::memory_order_acquire) && s->calls[stage].status.load() == noErr;
}
static bool buffer_ok(State *s, uint32_t i) {
    AudioQueueBufferRef b = s->buffers[i];
    if (!b || !s->data[i] || b->mAudioData != s->data[i] || b->mAudioDataBytesCapacity < Payload + Guard ||
        b->mAudioDataByteSize != Payload) return false;
    const auto *bytes = static_cast<const unsigned char *>(b->mAudioData);
    for (uint32_t n = 0; n < Guard; ++n) if (bytes[Payload + n] != 0xa5) return false;
    return true;
}
static bool open_callbacks(State *s) {
    const double deadline = now() + 0.5;
    while (!s->gate.try_open()) {
        if (now() >= deadline) return false;
        [NSThread sleepForTimeInterval:0.001];
    }
    return true;
}
static void output_callback(void *user, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    auto *s = static_cast<State *>(user);
    // A single SC admission word closes the enqueue/stop race. After closure,
    // late callbacks touch only process-retained atomics, never queue/buffers.
    if (!s->gate.begin()) { s->gate.end(); return; }
    s->callbacks.fetch_add(1, std::memory_order_relaxed);
    uint32_t index = BufferCount;
    for (uint32_t i = 0; i < BufferCount; ++i) if (s->buffers[i] == buffer) index = i;
    if (queue != s->queue || index == BufferCount || !buffer_ok(s, index)) {
        s->buffer_error.store(true);
    } else {
        // AudioQueue's prescribed reuse operation; no allocation, application
        // lock, logging, file/device I/O, guest calls, or Swift in this callback.
        s->reenqueue_attempts.fetch_add(1, std::memory_order_relaxed);
        const OSStatus status = AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
        if (status == noErr) s->reenqueue_successes.fetch_add(1, std::memory_order_relaxed);
        else { int32_t expected = 0; s->callback_status.compare_exchange_strong(expected, status); }
    }
    s->gate.end();
}
static bool close_callbacks(State *s) {
    s->gate.close();
    const double deadline = now() + 0.5;
    while (!s->gate.idle()) {
        if (now() >= deadline) return false;
        [NSThread sleepForTimeInterval:0.001];
    }
    return true;
}
static void print_status(State *s, Stage stage) {
    if (s->calls[stage].returned.load(std::memory_order_acquire)) std::printf("%d", s->calls[stage].status.load());
    else std::printf("null"); // An unreturned Apple call is not invented success.
}
static void finish(State *s) {
    if (s->finished.exchange(true)) return;
    flockfile(stdout); // Kept until _exit so no worker appends after this record.
    const double elapsed = now() - s->begin;
    const bool done = s->done.load(std::memory_order_acquire), budget = elapsed <= 25.0;
    bool all = true;
    for (uint32_t i = 0; i < StageCount; ++i) all = all && successful(s, static_cast<Stage>(i));
    const bool valid0 = s->time_valid[0].load(), valid1 = s->time_valid[1].load();
    const double first = s->sample_time[0].load(), last = s->sample_time[1].load();
    const bool advancing = valid0 && valid1 && last > first && last - first <= 96000.0;
    const bool available = done && budget && all && s->session_ready.load() && s->error.load() == 0 &&
        !s->buffer_error.load() && s->guard_checks.load() == BufferCount && s->callback_status.load() == 0 &&
        s->callbacks.load() > 0 && s->reenqueue_successes.load() > 0 && advancing &&
        s->observation_ms.load() >= 190 && s->observation_ms.load() <= 1000 && s->stream_ms.load() <= 2000;
    std::printf("PUBLIC_IOS_AUDIOQUEUE_RESULT {\"schema\":1,\"available\":%s,\"completed\":%s,"
        "\"within_budget\":%s,\"session_ready\":%s,\"phase\":%u,\"probe_error\":%d,\"session_error\":%lld,"
        "\"started\":%s,\"stopped\":%s,\"disposed\":%s,\"buffer_error\":%s,\"guard_checks\":%u,"
        "\"callbacks\":%llu,\"reenqueue_attempts\":%llu,\"reenqueue_successes\":%llu,\"callback_status\":%d,"
        "\"sample_rate_hz\":48000,\"channels\":2,\"frames_per_buffer\":512,\"buffer_count\":3,"
        "\"time_flags\":[%u,%u],\"sample_times\":[",
        available ? "true" : "false", done ? "true" : "false", budget ? "true" : "false",
        s->session_ready.load() ? "true" : "false", s->phase.load(), s->error.load(), (long long)s->session_error.load(),
        successful(s, Start) ? "true" : "false", successful(s, Stop) ? "true" : "false",
        successful(s, Dispose) ? "true" : "false", s->buffer_error.load() ? "true" : "false", s->guard_checks.load(),
        (unsigned long long)s->callbacks.load(), (unsigned long long)s->reenqueue_attempts.load(),
        (unsigned long long)s->reenqueue_successes.load(), s->callback_status.load(),
        s->time_flags[0].load(), s->time_flags[1].load());
    if (valid0) std::printf("%.17g", first); else std::printf("null");
    std::printf(",");
    if (valid1) std::printf("%.17g", last); else std::printf("null");
    std::printf("],\"time_advancing\":%s,\"observation_ms\":%u,\"stream_elapsed_ms\":%u,"
        "\"main_runloop_ms\":%u,\"statuses\":[", advancing ? "true" : "false", s->observation_ms.load(),
        s->stream_ms.load(), uint32_t(elapsed * 1000));
    for (uint32_t i = 0; i < StageCount; ++i) { if (i) std::printf(","); print_status(s, static_cast<Stage>(i)); }
    std::printf("]}\n"); std::fflush(stdout); _exit(0);
}
extern "C" void OHNativeAudioQueueEnvironmentGraph(State *s) {
    @autoreleasepool {
        double stream_begin = 0;
        @try {
            AVAudioSession *session = AVAudioSession.sharedInstance;
            NSError *error = nil;
            phase(s, 10);
            if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&error]) {
                s->error.store(1); s->session_error.store(error.code); return;
            }
            phase(s, 11);
            if (![session setPreferredSampleRate:48000 error:&error]) { s->error.store(2); s->session_error.store(error.code); return; }
            phase(s, 12);
            if (![session setPreferredIOBufferDuration:512.0 / 48000.0 error:&error]) { s->error.store(3); s->session_error.store(error.code); return; }
            phase(s, 13);
            if (![session setActive:YES error:&error]) { s->error.store(4); s->session_error.store(error.code); return; }
            s->session_ready.store(true); phase(s, 14);
            AudioStreamBasicDescription format{};
            format.mSampleRate = 48000; format.mFormatID = kAudioFormatLinearPCM;
            format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
            format.mBytesPerPacket = 8; format.mFramesPerPacket = 1; format.mBytesPerFrame = 8;
            format.mChannelsPerFrame = 2; format.mBitsPerChannel = 32;
            phase(s, 100);
            // NULL runloop: AudioQueue supplies an internal callback thread.
            if (!returned(s, NewOutput, AudioQueueNewOutput(&format, output_callback, s, nullptr, nullptr, 0, &s->queue))) return;
            if (!s->queue) { s->error.store(5); return; }
            for (uint32_t i = 0; i < BufferCount; ++i) {
                const Stage stage = static_cast<Stage>(Allocate0 + i); phase(s, 100 + 2 * stage);
                if (!returned(s, stage, AudioQueueAllocateBuffer(s->queue, Payload + Guard, &s->buffers[i]))) return;
                AudioQueueBufferRef buffer = s->buffers[i];
                if (!buffer || !buffer->mAudioData || buffer->mAudioDataBytesCapacity < Payload + Guard) { s->error.store(6); return; }
                s->data[i] = buffer->mAudioData;
                std::memset(buffer->mAudioData, 0, Payload);
                std::memset(static_cast<unsigned char *>(buffer->mAudioData) + Payload, 0xa5, Guard);
                buffer->mAudioDataByteSize = Payload;
            }
            for (uint32_t i = 0; i < BufferCount; ++i) {
                const Stage stage = static_cast<Stage>(Enqueue0 + i); phase(s, 100 + 2 * stage);
                if (!returned(s, stage, AudioQueueEnqueueBuffer(s->queue, s->buffers[i], 0, nullptr))) return;
            }
            if (!open_callbacks(s)) { s->error.store(8); return; }
            phase(s, 114);
            if (!returned(s, Start, AudioQueueStart(s->queue, nullptr))) return;
            stream_begin = now();
            [NSThread sleepForTimeInterval:0.05];
            double observation_begin = 0;
            for (uint32_t i = 0; i < 2; ++i) {
                if (i) [NSThread sleepForTimeInterval:0.2];
                AudioTimeStamp timestamp{};
                const Stage stage = static_cast<Stage>(Time0 + i); phase(s, 100 + 2 * stage);
                if (!returned(s, stage, AudioQueueGetCurrentTime(s->queue, nullptr, &timestamp, nullptr))) return;
                s->time_flags[i].store(timestamp.mFlags);
                const bool valid = (timestamp.mFlags & kAudioTimeStampSampleTimeValid) && std::isfinite(timestamp.mSampleTime) &&
                    timestamp.mSampleTime >= 0 && timestamp.mSampleTime < 281474976710656.0;
                if (valid) s->sample_time[i].store(timestamp.mSampleTime);
                s->time_valid[i].store(valid);
                if (!valid) { s->error.store(7); return; }
                if (!i) observation_begin = now();
                else s->observation_ms.store(uint32_t((now() - observation_begin) * 1000));
            }
        } @catch (NSException *exception) { (void)exception; s->error.store(9); }
        @finally {
            if (s->queue) {
                // Never race a callback enqueue against Stop/Dispose. An
                // undrained callback is an honest failure; retain until _exit.
                if (!close_callbacks(s)) s->error.store(8);
                else {
                    phase(s, 120);
                    const bool stopped = returned(s, Stop, AudioQueueStop(s->queue, true));
                    if (stream_begin) s->stream_ms.store(uint32_t((now() - stream_begin) * 1000));
                    if (stopped) {
                        for (uint32_t i = 0; i < BufferCount; ++i) if (s->data[i]) {
                            if (!buffer_ok(s, i)) s->buffer_error.store(true);
                            else s->guard_checks.fetch_add(1);
                        }
                    }
                    phase(s, 122); returned(s, Dispose, AudioQueueDispose(s->queue, true));
                    // All buffer pointers are invalid after Dispose; never inspect again.
                }
            }
            s->done.store(true, std::memory_order_release);
            dispatch_async(dispatch_get_main_queue(), ^{ finish(s); });
        }
    }
}
@interface PublicAudioQueueSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation PublicAudioQueueSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *controller = [[UIViewController alloc] init];
    controller.view.backgroundColor = UIColor.systemBlueColor;
    self.window.rootViewController = controller; [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    if (![scene isKindOfClass:UIWindowScene.class] || self.window.windowScene != scene || state->activated.exchange(true)) return;
    state->begin = now(); phase(state, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 25LL * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ finish(state); });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ OHNativeAudioQueueEnvironmentGraph(state); });
}
@end
@interface PublicAudioQueueAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation PublicAudioQueueAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)application
        configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *configuration = [[UISceneConfiguration alloc] initWithName:@"PublicAudioQueueScene" sessionRole:session.role];
    configuration.sceneClass = UIWindowScene.class; configuration.delegateClass = PublicAudioQueueSceneDelegate.class;
    return configuration;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 1) return 2;
        state = new State;
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(PublicAudioQueueAppDelegate.class));
    }
}
'''


def parse_native(raw):
    if len(raw) > 2 * LOG_LIMIT: raise ValueError("console limit")
    results, phases = [], []
    for line in raw[:raw.rfind(b"\n") + 1].splitlines():
        if b"PUBLIC_IOS_AUDIOQUEUE_RESULT" in line:
            match = re.fullmatch(rb"[\t\x20-\x7e]{0,512}PUBLIC_IOS_AUDIOQUEUE_RESULT (\{[^\r\n]+\})", line)
            if not match or line.count(b"PUBLIC_IOS_AUDIOQUEUE_RESULT") != 1: raise ValueError("result marker")
            results.append(engine.strict_json(match[1]))
        if b"PUBLIC_AUDIOQUEUE_PHASE" in line:
            match = re.fullmatch(rb"[\t\x20-\x7e]{0,512}PUBLIC_AUDIOQUEUE_PHASE ([0-9]{1,3})", line)
            if not match or line.count(b"PUBLIC_AUDIOQUEUE_PHASE") != 1 or int(match[1]) not in PHASES:
                raise ValueError("phase marker")
            phases.append(int(match[1]))
    if not results: return None
    if len(results) != 1: raise ValueError("duplicate result")
    value = results[0]
    expected = dict(schema=int, available=bool, completed=bool, within_budget=bool, session_ready=bool,
                    phase=int, probe_error=int, session_error=int, started=bool, stopped=bool, disposed=bool,
                    buffer_error=bool, guard_checks=int, callbacks=int, reenqueue_attempts=int, reenqueue_successes=int,
                    callback_status=int, sample_rate_hz=int, channels=int, frames_per_buffer=int, buffer_count=int,
                    time_flags=list, sample_times=list, time_advancing=bool, observation_ms=int,
                    stream_elapsed_ms=int, main_runloop_ms=int, statuses=list)
    if type(value) is not dict or set(value) != set(expected) or any(type(value[k]) is not t for k, t in expected.items()):
        raise ValueError("fields/types")
    if (value["schema"] != 1 or value["phase"] not in PHASES or not 0 <= value["probe_error"] <= 9
            or not -(2**63) <= value["session_error"] < 2**63 or not -(2**31) <= value["callback_status"] < 2**31
            or (value["sample_rate_hz"], value["channels"], value["frames_per_buffer"], value["buffer_count"]) != (48000, 2, 512, 3)
            or not 0 <= value["guard_checks"] <= 3
            or any(not 0 <= value[k] < 2**64 for k in ("callbacks", "reenqueue_attempts", "reenqueue_successes"))
            or any(not 0 <= value[k] <= 35000 for k in ("observation_ms", "stream_elapsed_ms", "main_runloop_ms"))):
        raise ValueError("numeric bounds")
    statuses, flags, samples = value["statuses"], value["time_flags"], value["sample_times"]
    if (len(statuses) != len(STAGES) or any(x is not None and (type(x) is not int or not -(2**31) <= x < 2**31) for x in statuses)
            or len(flags) != 2 or any(type(x) is not int or not 0 <= x < 2**32 for x in flags)
            or len(samples) != 2 or any(x is not None and (type(x) not in (int, float) or not math.isfinite(x) or not 0 <= x < 2**48) for x in samples)):
        raise ValueError("stage/time fields")
    for i in range(2):
        if samples[i] is not None and (statuses[8 + i] != 0 or not flags[i] & 1):
            raise ValueError("invalid sample timestamp")
    advancing = all(x is not None for x in samples) and 0 < samples[1] - samples[0] <= 96000
    ready = (value["completed"] and value["within_budget"] and value["session_ready"] and statuses == [0] * len(STAGES)
             and value["probe_error"] == 0 and value["session_error"] == 0 and not value["buffer_error"] and value["guard_checks"] == 3
             and value["callback_status"] == 0 and value["callbacks"] > 0 and value["reenqueue_successes"] > 0 and advancing
             and 190 <= value["observation_ms"] <= 1000 and value["stream_elapsed_ms"] <= 2000)
    if (value["available"] != ready or value["time_advancing"] != advancing
            or any(value[name] != (statuses[index] == 0) for name, index in (("started", 7), ("stopped", 10), ("disposed", 11)))
            or value["reenqueue_successes"] > value["reenqueue_attempts"] or value["reenqueue_attempts"] > value["callbacks"]
            or value["within_budget"] and value["main_runloop_ms"] > 25000
            or not value["completed"] and value["within_budget"]
            or not phases or phases[-1] != value["phase"]):
        raise ValueError("inconsistent result")
    if ready and phases != SUCCESS_PHASES: raise ValueError("incomplete successful path")
    return value


def compile_environment():
    environment = os.environ.copy()
    for name in ("SDKROOT", "IPHONEOS_DEPLOYMENT_TARGET", "MACOSX_DEPLOYMENT_TARGET", "TVOS_DEPLOYMENT_TARGET", "WATCHOS_DEPLOYMENT_TARGET"):
        environment.pop(name, None)
    return environment


def run_queue(udid, output, deadline):
    end = min(deadline, time.monotonic() + MODE_SECONDS)
    if engine.STOP or time.monotonic() >= end: raise ValueError("mode budget exhausted")
    logs = [output / ("console." + stream + ".log") for stream in ("stdout", "stderr")]
    result = dict(available=False, queue_output_stream_verified=False, phase=0, reason="mode_deadline")
    child = None
    try:
        with logs[0].open("xb") as stdout, logs[1].open("xb") as stderr:
            child = subprocess.Popen(["xcrun", "simctl", "launch", "--console", udid, BUNDLE],
                                     stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, start_new_session=True,
                                     preexec_fn=lambda: engine.limits(LOG_LIMIT))
            while not engine.STOP and time.monotonic() < end - 3.0:
                raw = b"\n".join(engine.bounded_read(path, LOG_LIMIT) for path in logs)
                native = parse_native(raw)
                if native is not None:
                    result.update(native, reason="native_report", queue_output_stream_verified=native["available"])
                    try:
                        code = child.wait(timeout=min(1.0, max(0.001, end - time.monotonic() - 2.5)))
                        result["console_exit_code"] = code
                        if code != 0:
                            result.update(available=False, queue_output_stream_verified=False, reason="console_nonzero_exit")
                    except subprocess.TimeoutExpired:
                        result.update(available=False, queue_output_stream_verified=False, reason="console_exit_timeout")
                    break
                if child.poll() is not None:
                    result["console_exit_code"] = child.returncode
                    native = parse_native(b"\n".join(engine.bounded_read(path, LOG_LIMIT) for path in logs))
                    if native is not None: result.update(native, reason="native_report", queue_output_stream_verified=native["available"])
                    else: result["reason"] = "console_exited_without_native_report"
                    if child.returncode != 0:
                        result.update(available=False, queue_output_stream_verified=False, reason="console_nonzero_exit")
                    break
                time.sleep(0.05)
            if engine.STOP:
                result.update(available=False, queue_output_stream_verified=False, reason="cancelled")
    finally:
        try:
            if child is not None: engine.reap_owned_group(child)
        finally:
            stopped = engine.run(["xcrun", "simctl", "terminate", udid, BUNDLE], end, 2, allow_cancel=True)
            result["terminate_exit_code"] = stopped.code
    (output / "queue-result.json").write_text(json.dumps(result, sort_keys=True) + "\n", encoding="ascii")
    return result


def probe(udid, output):
    start = time.monotonic(); deadline = start + TOTAL_SECONDS - 1
    report = dict(schema=1, backend="audioqueue", diagnostic_only=True, public_only=True, available=False,
                  queue_output_stream_verified=False, avaudioengine_verified=False, physical_iphone_verified=False,
                  audible_output_verified=False, game_audio_verified=False, sdk_tests_passed_claimed=False,
                  sample_attempted=False, reason="unsupported_platform")
    if sys.platform != "darwin" or os.getuid() == 0 or os.getuid() != os.geteuid(): return report
    helper = engine.helper_module()
    helper.no_symlink_parents(output)
    if output.exists() or not output.parent.is_dir() or re.fullmatch(helper.UUID, udid) is None:
        raise ValueError("fresh output and exact UDID required")
    output.mkdir(mode=0o700)
    def checked(command, seconds, name, **kwargs):
        report["stage"] = name
        outcome = engine.run(command, deadline, seconds, **kwargs)
        (output / (name + ".log")).write_bytes(outcome.output)
        if outcome.reason != "completed" or outcome.code:
            report["tool_failure"] = dict(stage=name, reason=outcome.reason, exit_code=outcome.code,
                                          progress=engine.sanitized_progress(outcome.output, output))
            raise ValueError(name + " failed")
        return outcome.output
    try:
        for kind in ("launch", "install", "list", "terminate"):
            if not engine.help_valid(kind, checked(["xcrun", "simctl", "help", kind], 2, "help-" + kind)):
                raise ValueError("installed simctl help unrecognized")
        engine.validate_device(checked(["xcrun", "simctl", "list", "devices", udid, "--json"], 3, "selected-device"), udid)
        with tempfile.TemporaryDirectory(prefix="public-ios-audioqueue-") as temporary:
            work = Path(temporary).resolve(strict=True)
            app = work / (EXECUTABLE + ".app"); app.mkdir()
            source = work / "probe.mm"; source.write_text(SOURCE, encoding="ascii")
            plist = dict(CFBundleExecutable=EXECUTABLE, CFBundleIdentifier=BUNDLE, CFBundleName=EXECUTABLE,
                         CFBundlePackageType="APPL", CFBundleVersion="1", CFBundleShortVersionString="1.0",
                         MinimumOSVersion="26.5", LSRequiresIPhoneOS=True, UIDeviceFamily=[1, 2],
                         CFBundleSupportedPlatforms=["iPhoneSimulator"], UILaunchScreen={},
                         UIApplicationSceneManifest=dict(UIApplicationSupportsMultipleScenes=False,
                             UISceneConfigurations={"UIWindowSceneSessionRoleApplication": [dict(
                                 UISceneConfigurationName="PublicAudioQueueScene", UISceneClassName="UIWindowScene",
                                 UISceneDelegateClassName="PublicAudioQueueSceneDelegate")]}))
            (app / "Info.plist").write_bytes(plistlib.dumps(plist))
            sdk = checked(["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"], 2, "sdk-path").decode().strip()
            if not sdk.startswith("/") or "\n" in sdk or "\x00" in sdk: raise ValueError("SDK path")
            command = ["xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++17", "-fobjc-arc", "-fblocks",
                       "-Wall", "-Wextra", "-Werror", "-target", "arm64-apple-ios26.5-simulator", "-isysroot", sdk,
                       str(source), "-framework", "UIKit", "-framework", "AVFAudio", "-framework", "AudioToolbox",
                       "-framework", "Foundation", "-o", str(app / EXECUTABLE)]
            report["stage"] = "compile"
            compiled = engine.run(command, deadline, 15, env=compile_environment())
            (output / "compile.log").write_bytes(compiled.output)
            if compiled.reason != "completed" or compiled.code:
                report.update(reason="compile_failed", compiler_progress=engine.sanitized_progress(compiled.output, work)); return report
            checked(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(app)], 4, "adhoc-sign")
            checked(["xcrun", "simctl", "install", udid, str(app)], 20, "install")
            report["stage"] = "queue"
            report["queue"] = run_queue(udid, output, deadline)
            report.update(reason="completed", available=report["queue"]["available"])
            report["queue_output_stream_verified"] = report["available"]
    except Exception:
        report["reason"] = "diagnostic_tool_or_validation_failure"
    finally:
        report["elapsed_ms"] = int((time.monotonic() - start) * 1000)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--udid", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    # The reused process API owns cancellation; call its explicit public handler.
    signal.signal(signal.SIGTERM, engine.on_signal); signal.signal(signal.SIGINT, engine.on_signal)
    try: report = probe(args.udid, Path(os.path.abspath(args.output)))
    except Exception:
        report = dict(schema=1, backend="audioqueue", diagnostic_only=True, public_only=True, available=False,
                      sdk_tests_passed_claimed=False, reason="diagnostic_setup_failure")
    print("PUBLIC_IOS_AUDIOQUEUE_ENVIRONMENT " + json.dumps(report, sort_keys=True), flush=True)
    return 0 if report.get("available") is True else 2


if __name__ == "__main__":
    raise SystemExit(main())
