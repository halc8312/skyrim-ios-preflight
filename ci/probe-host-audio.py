#!/usr/bin/env python3
"""Read-only macOS HAL inventory; metadata is not audio playback verification.

phase: pre_gui=1, post_gui=2. stage: 0=complete, 1=not Darwin,
2=compile timeout, 3=compile failure, 4=query timeout, 5=query failure,
6=invalid child output, 7=local tool/I/O failure. HAL stages 10/20/30/40/41
identify default output/alive/rate/config-size/config-data calls.
No input devices, names, setters, drivers, or production audio code are used.
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
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int available, present, alive;
    uint32_t rate, channels, stage;
    int32_t status;
} Result;

static void before(uint32_t stage) {
    printf("%u\n", stage);
    fflush(stdout);
}

static void inspect(Result *r) {
    AudioDeviceID device = kAudioObjectUnknown;
    AudioObjectPropertyAddress p = {kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 bytes = sizeof(device);
    r->stage = 10; before(r->stage);
    r->status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &p, 0, NULL, &bytes, &device);
    if (r->status != noErr) return;
    if (bytes != sizeof(device)) { r->status = -1001; return; }
    r->present = device != kAudioObjectUnknown;
    if (!r->present) { r->stage = 0; return; }

    UInt32 alive = 0;
    p.mSelector = kAudioDevicePropertyDeviceIsAlive;
    bytes = sizeof(alive); r->stage = 20; before(r->stage);
    r->status = AudioObjectGetPropertyData(device, &p, 0, NULL, &bytes, &alive);
    if (r->status != noErr) return;
    if (bytes != sizeof(alive)) { r->status = -1001; return; }
    r->alive = alive != 0;

    Float64 rate = 0;
    p.mSelector = kAudioDevicePropertyNominalSampleRate;
    bytes = sizeof(rate); r->stage = 30; before(r->stage);
    r->status = AudioObjectGetPropertyData(device, &p, 0, NULL, &bytes, &rate);
    if (r->status != noErr) return;
    if (bytes != sizeof(rate) || !isfinite(rate) || rate <= 0 || rate > 1000000) {
        r->status = -1002; return;
    }
    r->rate = (uint32_t)(rate + 0.5);

    p.mSelector = kAudioDevicePropertyStreamConfiguration;
    p.mScope = kAudioObjectPropertyScopeOutput;
    bytes = 0; r->stage = 40; before(r->stage);
    r->status = AudioObjectGetPropertyDataSize(device, &p, 0, NULL, &bytes);
    if (r->status != noErr) return;
    const size_t header = offsetof(AudioBufferList, mBuffers);
    if (bytes < header || bytes > 65536) { r->status = -1003; return; }
    const UInt32 capacity = bytes;
    AudioBufferList *buffers = calloc(1, capacity);
    if (!buffers) { r->status = -1004; return; }
    r->stage = 41; before(r->stage);
    r->status = AudioObjectGetPropertyData(device, &p, 0, NULL, &bytes, buffers);
    if (r->status == noErr) {
        if (bytes < header || bytes > capacity || buffers->mNumberBuffers > 256 ||
            buffers->mNumberBuffers > (bytes - header) / sizeof(AudioBuffer)) {
            r->status = -1003;
        } else {
            uint64_t channels = 0;
            for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
                channels += buffers->mBuffers[i].mNumberChannels;
            if (channels > 65536) r->status = -1003;
            else r->channels = (uint32_t)channels;
        }
    }
    free(buffers);
    if (r->status != noErr) return;
    r->available = r->alive && r->rate != 0 && r->channels != 0;
    r->stage = 0;
}

int main(void) {
    Result r = {0};
    inspect(&r);
    printf("{\"available\":%s,\"default_output_present\":%s,\"default_alive\":%s,"
           "\"nominal_rate_hz\":%u,\"output_channels\":%u,\"stage\":%u,\"status\":%d}\n",
           r.available ? "true" : "false", r.present ? "true" : "false",
           r.alive ? "true" : "false", r.rate, r.channels, r.stage, r.status);
    return 0;
}
'''

LIMIT = 4096
HAL_STAGES = {10, 20, 30, 40, 41}


def owned_run(command: list[str], cwd: Path, seconds: int, *,
              environment: dict[str, str] | None = None) -> tuple[int, bool, bytes]:
    # Each process owns a fresh process group. Never signal a pre-existing tool
    # or CoreAudio service, and never relay arbitrary compiler/framework stderr.
    with tempfile.TemporaryFile() as output:
        child = subprocess.Popen(command, cwd=cwd, stdin=subprocess.DEVNULL,
                                 stdout=output, stderr=subprocess.DEVNULL, start_new_session=True,
                                 env=environment)
        timed_out = False
        try:
            code = child.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            code = child.wait(timeout=1)
        output.seek(0)
        return code, timed_out, output.read(LIMIT + 1)


def parse_child(raw: bytes) -> dict:
    if len(raw) > LIMIT:
        raise ValueError("child output exceeds bound")
    lines = raw.decode("ascii", "strict").splitlines()
    if not lines or len(lines) > 6:
        raise ValueError("unexpected child output shape")
    stages = [int(line) for line in lines[:-1]]
    if stages not in ([10], [10, 20], [10, 20, 30], [10, 20, 30, 40], [10, 20, 30, 40, 41]):
        raise ValueError("unexpected HAL query sequence")

    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate child key")
            result[key] = value
        return result

    value = json.loads(lines[-1], object_pairs_hook=pairs,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonstandard number")))
    expected = {"available": bool, "default_output_present": bool, "default_alive": bool,
                "nominal_rate_hz": int, "output_channels": int, "stage": int, "status": int}
    if type(value) is not dict or set(value) != set(expected):
        raise ValueError("unexpected child report fields")
    if any(type(value[key]) is not kind for key, kind in expected.items()):
        raise ValueError("unexpected child report type")
    if (not 0 <= value["nominal_rate_hz"] <= 1000000 or not 0 <= value["output_channels"] <= 65536
            or not -(1 << 31) <= value["status"] < (1 << 31)
            or value["stage"] not in HAL_STAGES | {0}
            or (value["stage"] != 0 and value["stage"] != stages[-1])):
        raise ValueError("unexpected child numeric range")
    stage, status = value["stage"], value["status"]
    present, alive = value["default_output_present"], value["default_alive"]
    rate, channels = value["nominal_rate_hz"], value["output_channels"]
    if stage == 0:
        if status != 0 or (stages != ([10, 20, 30, 40, 41] if present else [10])):
            raise ValueError("inconsistent completed query")
    elif status == 0 or present != (stage != 10):
        raise ValueError("inconsistent failed query")
    if ((not present and (alive or rate or channels))
            or (stage in (10, 20) and (alive or rate or channels))
            or (stage == 30 and (rate or channels))
            or (stage in (40, 41) and channels != 0)):
        raise ValueError("fields precede their successful query")
    ready = (value["stage"] == 0 and value["status"] == 0 and value["default_output_present"]
             and value["default_alive"] and value["nominal_rate_hz"] > 0 and value["output_channels"] > 0)
    if value["available"] != ready:
        raise ValueError("inconsistent availability")
    return value


def probe(phase: str) -> dict:
    report = dict(schema=1, phase={"pre_gui": 1, "post_gui": 2}[phase], available=False,
                  stage=1, status=0, hal_stage=0, default_output_present=False, default_alive=False,
                  nominal_rate_hz=0, output_channels=0, query_deadline_seconds=5,
                  audio_playback_verified=False, simulator_audio_verified=False)
    if sys.platform != "darwin":
        return report
    try:
        with tempfile.TemporaryDirectory(prefix="public-host-audio-") as directory:
            work = Path(directory)
            source, executable = work / "probe.c", work / "probe"
            source.write_text(SOURCE, encoding="ascii")
            # The surrounding workflow builds iOS. Do not let its SDK/target
            # environment override this explicitly macOS-only diagnostic.
            environment = os.environ.copy()
            for name in ("SDKROOT", "IPHONEOS_DEPLOYMENT_TARGET"):
                environment.pop(name, None)
            code, timeout, _ = owned_run(["xcrun", "--sdk", "macosx", "clang", "-std=c11", "-Wall", "-Wextra",
                                         "-Werror", str(source), "-framework", "CoreAudio", "-framework",
                                         "CoreFoundation", "-o", str(executable)], work, 15,
                                        environment=environment)
            if timeout or code:
                report.update(stage=2 if timeout else 3, status=code)
                return report
            code, timeout, raw = owned_run([str(executable)], work, 5)
            if timeout or code:
                report.update(stage=4 if timeout else 5, status=code)
                if len(raw) <= LIMIT:
                    stages = raw.splitlines()
                    if stages and stages[-1] in (b"10", b"20", b"30", b"40", b"41"):
                        report["hal_stage"] = int(stages[-1])
                return report
            try:
                value = parse_child(raw)
            except (ValueError, UnicodeError, RecursionError):
                report["stage"] = 6
                return report
            report.update(value)
            report["hal_stage"] = value["stage"]
            report["stage"] = 0
    except (OSError, subprocess.SubprocessError):
        report["stage"] = 7
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", required=True, choices=("pre_gui", "post_gui"))
    args = parser.parse_args(argv)
    print(json.dumps(probe(args.phase), separators=(",", ":")))
    # Successful diagnostic collection is distinct from available=true. Never
    # turn an absent endpoint into playback success or skip the actual SDK tests.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
