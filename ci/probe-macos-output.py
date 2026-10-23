#!/usr/bin/env python3
"""Optional macOS real-output diagnostic; never substitutes for SDK audio tests.

Fresh AVAudioEngine graph on a worker, serviced Main runloop <=4s, silent
48kHz/stereo source for 200ms, whole child deadline 5s. No input/session,
offline rendering, device settings, names, or audible-output assertion.
stage: 0=collected, 1=not Darwin, 2=compile timeout, 3=compile failure,
4=query timeout, 5=query failure, 6=invalid output, 7=local tool/I/O failure.
phase: 10=engine alloc, 11=output getter, 12=hardware format, 20=mixer getter,
30=source alloc, 31=attach, 32=connect, 40=prepare, 41=start, 42=running,
50=stop, 51=stop returned. Actual macOS execution is a separate CI result.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


SOURCE = r'''
#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <time.h>
#include <unistd.h>

struct State {
    std::atomic<uint32_t> phase{0}, rate{0}, channels{0}, stream_ms{0};
    std::atomic<uint64_t> callbacks{0}, frames{0};
    std::atomic<int32_t> status{0};
    std::atomic<bool> done{false}, started{false}, stopped{false}, bad_format{false};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

static double now() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
}
static void phase(State *s, uint32_t value) {
    flockfile(stdout);
    s->phase.store(value, std::memory_order_relaxed);
    std::printf("%u\n", value);
    std::fflush(stdout);
    funlockfile(stdout);
}
static void graph(State *s) {
    @autoreleasepool {
        AVAudioEngine *engine = nil;
        double stream_start = 0;
        @try {
            phase(s, 10);
            engine = [[AVAudioEngine alloc] init];
            if (!engine) { s->status.store(-1); return; }
            phase(s, 11);
            AVAudioOutputNode *output = engine.outputNode;
            if (!output) { s->status.store(-2); return; }
            phase(s, 12);
            AVAudioFormat *hardware = [output outputFormatForBus:0];
            const double rate = hardware.sampleRate;
            const uint32_t channels = hardware.channelCount;
            if (!hardware || !std::isfinite(rate) || rate < 1 || rate > 1000000 ||
                channels == 0 || channels > 256) { s->status.store(-3); return; }
            s->rate.store(uint32_t(rate + 0.5));
            s->channels.store(channels);
            AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
            if (!format || format.isInterleaved || format.commonFormat != AVAudioPCMFormatFloat32) {
                s->status.store(-4); return;
            }
            phase(s, 20);
            AVAudioMixerNode *mixer = engine.mainMixerNode;
            if (!mixer) { s->status.store(-5); return; }
            phase(s, 30);
            AVAudioSourceNode *source = [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:
                ^OSStatus(BOOL *silence, const AudioTimeStamp *timestamp,
                          AVAudioFrameCount frames, AudioBufferList *buffers) {
                    (void)timestamp;
                    if (!silence || !buffers || frames > 65536 || buffers->mNumberBuffers != 2) {
                        s->bad_format.store(true, std::memory_order_relaxed); return -50;
                    }
                    for (uint32_t i = 0; i < 2; ++i) {
                        const AudioBuffer &b = buffers->mBuffers[i];
                        if (!b.mData || b.mNumberChannels != 1 || b.mDataByteSize < frames * sizeof(float)) {
                            s->bad_format.store(true, std::memory_order_relaxed); return -50;
                        }
                    }
                    for (uint32_t i = 0; i < 2; ++i)
                        std::memset(buffers->mBuffers[i].mData, 0, frames * sizeof(float));
                    *silence = YES;
                    s->frames.fetch_add(frames, std::memory_order_relaxed);
                    s->callbacks.fetch_add(1, std::memory_order_relaxed);
                    return 0;
                }];
            if (!source) { s->status.store(-6); return; }
            phase(s, 31); [engine attachNode:source];
            phase(s, 32); [engine connect:source to:mixer format:format];
            mixer.outputVolume = 1.0f;
            phase(s, 40); [engine prepare];
            NSError *error = nil;
            phase(s, 41);
            if (![engine startAndReturnError:&error]) { s->status.store(-7); return; }
            stream_start = now();
            s->started.store(true);
            phase(s, 42);
            [NSThread sleepForTimeInterval:0.2];
        } @catch (NSException *exception) {
            (void)exception;
            s->status.store(-8);
        } @finally {
            if (engine) {
                @try {
                    phase(s, 50); [engine stop];
                    if (stream_start) s->stream_ms.store(uint32_t((now() - stream_start) * 1000));
                    s->stopped.store(true); phase(s, 51);
                } @catch (NSException *exception) {
                    (void)exception;
                    s->status.store(-9);
                }
            }
            s->done.store(true, std::memory_order_release);
        }
    }
}

int main() {
    @autoreleasepool {
        // Intentionally process-retained: an incomplete Apple call or an
        // in-flight render block must never outlive its callback state.
        State *s = new State;
        const double start = now(), deadline = start + 4.0;
        NSPort *keepAlive = [NSMachPort port];
        [[NSRunLoop currentRunLoop] addPort:keepAlive forMode:NSDefaultRunLoopMode];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ graph(s); });
        while (!s->done.load(std::memory_order_acquire) && now() < deadline) {
            @autoreleasepool {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                       beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
            }
        }
        // Serialize the last phase and final line. Keep this lock until _exit:
        // a still-running worker cannot append after the final report.
        flockfile(stdout);
        const double elapsed = now() - start;
        const bool done = s->done.load(std::memory_order_acquire), within_budget = elapsed <= 4.0;
        const uint64_t callbacks = s->callbacks.load(), frames = s->frames.load();
        const bool available = done && within_budget && s->stream_ms.load() <= 1000 &&
            s->started.load() && s->stopped.load() &&
            !s->bad_format.load() && s->status.load() == 0 && callbacks > 0 && frames > 0;
        std::printf("{\"available\":%s,\"completed\":%s,\"started\":%s,\"stopped\":%s,"
                    "\"format_error\":%s,\"within_budget\":%s,\"phase\":%u,\"status\":%d,\"sample_rate_hz\":%u,"
                    "\"output_channels\":%u,\"render_callbacks\":%llu,\"render_frames\":%llu,"
                    "\"main_runloop_ms\":%u,\"stream_elapsed_ms\":%u}\n",
                    available ? "true" : "false", done ? "true" : "false",
                    s->started.load() ? "true" : "false", s->stopped.load() ? "true" : "false",
                    s->bad_format.load() ? "true" : "false", within_budget ? "true" : "false",
                    s->phase.load(), s->status.load(),
                    s->rate.load(), s->channels.load(), (unsigned long long)callbacks,
                    (unsigned long long)frames, uint32_t(elapsed * 1000), s->stream_ms.load());
        std::fflush(stdout);
        // Never block on autorelease/destructors after a timed-out Apple call.
        _exit(0);
    }
}
'''

LIMIT = 4096
PHASES = {10, 11, 12, 20, 30, 31, 32, 40, 41, 42, 50, 51}


def query_limits():
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (LIMIT, LIMIT))


def owned_run(command, cwd, seconds, *, environment=None, bounded_query=False, compiler_output=False):
    # Same fresh-process-group/deadline mechanism as probe-host-audio.py.
    with tempfile.TemporaryFile() as output:
        child = subprocess.Popen(command, cwd=cwd, stdin=subprocess.DEVNULL, stdout=output,
                                 stderr=subprocess.STDOUT if compiler_output else subprocess.DEVNULL,
                                 start_new_session=True, env=environment,
                                 preexec_fn=query_limits if bounded_query else None)
        timeout = False
        try:
            code = child.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            timeout = True
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            code = child.wait(timeout=1)
        output.seek(0)
        return code, timeout, output.read(LIMIT + 1)


def compiler_diagnostic(raw, work):
    text = raw[:LIMIT].decode("utf-8", "replace")
    for path in sorted({str(work), str(work.resolve())}, key=len, reverse=True):
        text = text.replace(path, "<probe-temp>")
    prefix = "PUBLIC_MACOS_OUTPUT_COMPILE_FAILURE "
    encoded = json.dumps(text, ensure_ascii=True)
    while len(prefix) + len(encoded) + 1 > LIMIT:
        text = text[:len(text) // 2]
        encoded = json.dumps(text, ensure_ascii=True)
    return prefix + encoded


def parse_child(raw):
    if len(raw) > LIMIT:
        raise ValueError("output bound")
    lines = raw.decode("ascii", "strict").splitlines()
    if not 1 <= len(lines) <= 14 or any(line not in {str(x) for x in PHASES} for line in lines[:-1]):
        raise ValueError("phase output")
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate key")
            result[key] = value
        return result
    value = json.loads(lines[-1], object_pairs_hook=pairs,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonstandard number")))
    expected = dict(available=bool, completed=bool, started=bool, stopped=bool, format_error=bool, within_budget=bool,
                    phase=int, status=int, sample_rate_hz=int, output_channels=int,
                    render_callbacks=int, render_frames=int, main_runloop_ms=int, stream_elapsed_ms=int)
    if type(value) is not dict or set(value) != set(expected) or any(type(value[k]) is not t for k, t in expected.items()):
        raise ValueError("fields or types")
    if (value["phase"] not in PHASES | {0} or not -9 <= value["status"] <= 0
            or not 0 <= value["sample_rate_hz"] <= 1000000 or not 0 <= value["output_channels"] <= 256
            or not 0 <= value["render_callbacks"] <= 2**64-1 or not 0 <= value["render_frames"] <= 2**64-1
            or not 0 <= value["main_runloop_ms"] <= 5000 or not 0 <= value["stream_elapsed_ms"] <= 5000):
        raise ValueError("numeric bounds")
    ready = (value["completed"] and value["within_budget"] and value["stream_elapsed_ms"] <= 1000
             and value["started"] and value["stopped"] and not value["format_error"]
             and value["status"] == 0 and value["render_callbacks"] > 0 and value["render_frames"] > 0
             and value["sample_rate_hz"] > 0 and value["output_channels"] > 0 and value["phase"] == 51)
    if value["available"] != ready or (lines[:-1] and int(lines[-2]) != value["phase"]):
        raise ValueError("inconsistent result")
    if value["available"] and lines[:-1] != [str(x) for x in (10, 11, 12, 20, 30, 31, 32, 40, 41, 42, 50, 51)]:
        raise ValueError("incomplete successful graph path")
    if not value["completed"] and value["within_budget"]:
        raise ValueError("inconsistent graph deadline")
    if value["within_budget"] and value["main_runloop_ms"] > 4000:
        raise ValueError("inconsistent elapsed budget")
    return value


def probe():
    report = dict(schema=1, diagnostic_only=True, available=False, stage=1, status=0, phase=0,
                  query_deadline_seconds=5, graph_deadline_seconds=4, stream_window_ms=200,
                  native_output_stream_verified=False, audible_output_verified=False,
                  simulator_audio_verified=False, physical_iphone_verified=False)
    if sys.platform != "darwin":
        return report
    try:
        with tempfile.TemporaryDirectory(prefix="public-macos-output-") as directory:
            work = Path(directory)
            source, executable = work / "probe.mm", work / "probe"
            source.write_text(SOURCE, encoding="ascii")
            environment = os.environ.copy()
            for name in ("SDKROOT", "IPHONEOS_DEPLOYMENT_TARGET"):
                environment.pop(name, None)
            command = ["xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-fobjc-arc", "-fblocks",
                       "-Wall", "-Wextra", "-Werror", str(source), "-framework", "AVFAudio",
                       "-framework", "Foundation", "-framework", "CoreAudio", "-o", str(executable)]
            code, timeout, raw = owned_run(command, work, 15, environment=environment, compiler_output=True)
            if timeout or code:
                print(compiler_diagnostic(raw, work), file=sys.stderr, flush=True)
                report.update(stage=2 if timeout else 3, status=code)
                return report
            code, timeout, raw = owned_run([str(executable)], work, 5, bounded_query=True)
            if timeout or code:
                report.update(stage=4 if timeout else 5, status=code)
                if len(raw) <= LIMIT and raw.splitlines() and raw.splitlines()[-1] in {str(x).encode() for x in PHASES}:
                    report["phase"] = int(raw.splitlines()[-1])
                return report
            try:
                value = parse_child(raw)
            except (ValueError, UnicodeError, RecursionError):
                report["stage"] = 6
                return report
            report.update(value, stage=0, native_output_stream_verified=value["available"])
    except (OSError, subprocess.SubprocessError):
        report["stage"] = 7
    return report


def main(argv=None):
    argparse.ArgumentParser(description=__doc__).parse_args(argv)
    print("PUBLIC_MACOS_OUTPUT " + json.dumps(probe(), separators=(",", ":")))
    return 0  # Diagnostic availability never replaces the six SDK audio gates.


if __name__ == "__main__":
    raise SystemExit(main())
