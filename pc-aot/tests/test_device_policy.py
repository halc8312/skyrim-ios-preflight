"""Production C policy and pacing with authored clocks; no UIKit claims."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DevicePolicyTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "darwin", "Actual Apple run loop executes in the single combined Mac build")
    def test_actual_apple_wait_handles_input_and_worker_deadline(self):
        platform=(ROOT / "ios/runtime-port/device_platform.m").read_text()
        role=platform[platform.index("int skyrim_device_platform_work_role("):platform.index("unsigned skyrim_device_platform_wait(")]
        wait=platform[platform.index("unsigned skyrim_device_platform_wait("):platform.index("void skyrim_device_platform_log(")]
        with tempfile.TemporaryDirectory(prefix="device-apple-wait-") as tmp:
            source=Path(tmp)/"wait.m"
            source.write_text('''#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include "device_policy.h"
''' + role + wait + '''
static CFRunLoopSourceRef input;
static CFRunLoopRef main_loop;
static int handled;
static uint64_t now_ns(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
static void perform(void *unused) { (void)unused; handled=1; }
static void *signal_input(void *unused) {
    (void)unused; struct timespec t={0,20000000}; nanosleep(&t,NULL);
    CFRunLoopSourceSignal(input); CFRunLoopWakeUp(main_loop); return NULL;
}
static void *worker(void *unused) {
    (void)unused; uint64_t start=now_ns(); errno=E2BIG;
    assert(skyrim_device_platform_wait(10000000)==1);
    assert(errno==E2BIG && now_ns()-start>=10000000);
    SkyrimDeviceState state={6,0,0,1}; skyrim_device_update(&state);
    assert(skyrim_device_platform_work_role(SKYRIM_WORK_COMPILER)==0);
    qos_class_t qos; int relative;
    assert(!pthread_get_qos_class_np(pthread_self(),&qos,&relative));
    assert(qos==QOS_CLASS_UTILITY && relative==0);
    state.low_power=1; skyrim_device_update(&state);
    assert(skyrim_device_platform_work_role(SKYRIM_WORK_COMPILER)==0);
    assert(!pthread_get_qos_class_np(pthread_self(),&qos,&relative));
    assert(qos==QOS_CLASS_BACKGROUND);
    state.low_power=0; skyrim_device_update(&state);
    assert(skyrim_device_platform_work_role(SKYRIM_WORK_COMPILER)==0);
    assert(!pthread_get_qos_class_np(pthread_self(),&qos,&relative));
    assert(qos==QOS_CLASS_UTILITY);
    assert(skyrim_device_platform_work_role(SKYRIM_WORK_CACHE_WRITER)==0);
    assert(!pthread_get_qos_class_np(pthread_self(),&qos,&relative));
    assert(qos==QOS_CLASS_BACKGROUND && errno==E2BIG);
    return NULL;
}
int main(void) { @autoreleasepool {
    main_loop=CFRunLoopGetCurrent();
    CFRunLoopSourceContext context={0}; context.perform=perform;
    input=CFRunLoopSourceCreate(NULL,0,&context);
    CFRunLoopAddSource(main_loop,input,kCFRunLoopDefaultMode);
    pthread_t producer; assert(!pthread_create(&producer,NULL,signal_input,NULL));
    errno=ERANGE;
    assert(skyrim_device_platform_wait(2000000000)==0);
    assert(errno==ERANGE && handled==1);
    assert(!pthread_join(producer,NULL));
    CFRunLoopRemoveSource(main_loop,input,kCFRunLoopDefaultMode); CFRelease(input);
    assert(!pthread_create(&producer,NULL,worker,NULL));
    assert(!pthread_join(producer,NULL));
} }
''')
            binary=Path(tmp)/"wait"
            policy=Path(tmp)/"policy.o"
            subprocess.run(["clang","-std=c11","-O2","-c",str(ROOT/"ios/runtime-port/device_policy.c"),
                "-o",str(policy)],check=True,timeout=60)
            subprocess.run(["clang","-std=gnu11","-O2","-Wall","-Wextra","-Werror","-fobjc-arc",
                "-I",str(ROOT/"ios/runtime-port"),str(source),str(policy),"-framework","Foundation","-framework","CoreFoundation",
                "-o",str(binary)],check=True,timeout=60)
            subprocess.run([str(binary)],check=True,timeout=10)

    def test_bounded_policy_with_concurrent_public_states(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required; run on WSL/Mac")
        with tempfile.TemporaryDirectory(prefix="device-policy-") as tmp:
            binary = Path(tmp) / "policy"
            flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([cc, *flags, "-I", str(ROOT / "ios/runtime-port"),
                str(ROOT / "ios/runtime-port/device_policy.c"),
                str(ROOT / "tests/native/test_device_policy.c"), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=15)
            self.assertIn("concurrent_snapshot=PASS", result.stdout)

    def test_real_pacer_rechecks_interruption_and_keeps_cap_and_epoch(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required; run on WSL/Mac")
        runtime = (ROOT / "ios/runtime-port/ios_runtime.c").read_text()
        pacer = runtime[runtime.index("void skyrim_ios_pace_frame(void) {"):runtime.index("void skyrim_ios_watchdog(void) {")]
        with tempfile.TemporaryDirectory(prefix="device-pacer-") as tmp:
            source = Path(tmp) / "pacer.c"
            source.write_text('''#include <stdint.h>
#include <assert.h>
typedef struct { unsigned unused; } SkyrimFrameClock;
#include "performance_metrics.h"
static uint64_t metrics_epoch, now, target, slept, measured, remaining_seen;
static uint64_t interval_start, interval_end;
static unsigned waits, pumps, recorded, mode;
static uint64_t monotonic_ns(void) { return now; }
static unsigned skyrim_mobile_fps_limit(void) { return 60; }
static uint64_t skyrim_mobile_frame_deadline(SkyrimFrameClock *c, uint64_t start, unsigned fps) {
    (void)c; assert(start==now && fps==60); return target;
}
static unsigned skyrim_device_platform_wait(uint64_t remaining) {
    assert(remaining == target-now); remaining_seen=remaining; ++waits;
    if (mode==1 && waits==1) { now+=1; return 0; } /* input source wakes early */
    if (mode==2 && waits==1) { ++metrics_epoch; now+=2; return 1; } /* interruption/resume */
    now+=remaining; return mode ? 0 : 1;
}
static void skyrim_ios_pump_events(void) { ++pumps; }
void skyrim_perf_interval(unsigned kind, uint64_t started, uint64_t ended, int failed, uint64_t sleeps) {
    assert(kind==SKYRIM_PERF_PACER && !failed && ended>=started);
    ++recorded; measured=ended-started; slept=sleeps;
    interval_start=started; interval_end=ended;
}
''' + pacer + '''
int main(void) {
    target=1000000000; skyrim_ios_pace_frame();
    assert(waits==1 && pumps==1 && recorded==1 && slept==1 && measured==target);
    assert(interval_start==0 && interval_end==target);
    mode=1; now=0; waits=pumps=recorded=0; skyrim_ios_pace_frame();
    assert(waits==2 && pumps==2 && recorded==1 && slept==0 && measured==target);
    assert(interval_start==0 && interval_end==target);
    assert(remaining_seen==target-1);
    mode=2; now=0; waits=pumps=recorded=0; skyrim_ios_pace_frame();
    assert(waits==2 && pumps==2 && recorded==0);
    now=target+1; waits=recorded=0; skyrim_ios_pace_frame();
    assert(waits==0 && recorded==1 && measured==0);
    assert(interval_start==target+1 && interval_end==target+1);
}
''')
            binary = Path(tmp) / "pacer"
            subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT/"ios/runtime-port"),
                            str(source), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
