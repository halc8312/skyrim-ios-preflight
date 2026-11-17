# Separate host wait trials

These are two independent, opt-in experiments. Production staging and the app
build remain unchanged. Neither trial establishes iPhone performance or gameplay.
Use a fresh copy made by `ios/dependencies/prepare_dxvk.py` for each trial:

```sh
python3 ios/dependencies/prepare_dxvk.py --source vendor/skydream/third_party/dxvk --output /private/stage-lock
python3 experiments/dxvk-host-wait/patch.py unfair-lock /private/stage-lock

python3 ios/dependencies/prepare_dxvk.py --source vendor/skydream/third_party/dxvk --output /private/stage-qos
python3 experiments/dxvk-host-wait/patch.py cs-qos /private/stage-qos
```

The patcher pins the public source text, checks unique anchors, validates every
write destination first, rejects source/vendor locations and refuses to combine
the two trials or the graphics-flight prototype. Reapplying the same trial is
idempotent. Use a fresh stage to switch trials.

`unfair-lock` changes only the generic `Spinlock` class on Apple. An uncontended
call tries once; a failed try calls the blocking Apple API directly. Successful
lock/try calls read no clocks. Only a failed try starts wall/thread-CPU observation,
ending immediately after blocking acquisition. Fixed-size thread-local counters
receive the sample; no allocation, I/O or retry loop enters this lock. The host
fixture drains and prints counters after unlocking. Instances cannot be copied or
moved. The shared `spin()` function, non-Apple implementation and copied recursive
D3D9 lock remain unchanged. The contract follows Apple's guidance for
[os_unfair_lock_trylock](https://developer.apple.com/documentation/os/os_unfair_lock_trylock).

`cs-qos` adds one hook immediately after the command thread is named `dxvk-cs`.
Only an exact `SKYRIM_DXVK_CS_QOS=1` requests `USER_INITIATED` at relative priority
zero. Otherwise no QoS API, counter or report runs, preserving original inheritance.
The startup record keeps `errno`, reports the API return codes and before/after
values, uses an anonymous sequential number, and prints at most 64 fixed-size
numeric lines. The report cannot change scheduling. Shader workers are untouched.

The `requested_*` fields come from `pthread_get_qos_class_np`; `current_*` comes
from `qos_class_self`. Neither proves the temporary effective scheduler priority
during a QoS override, as documented in
[Apple's pthread QoS header](https://github.com/apple-oss-distributions/libpthread/blob/main/include/pthread/qos.h).
`effective_observed=0` and `iphone_verified=0` are therefore always reported.

Run the isolated checks with:

```sh
python3 -m unittest discover -s tests -p test_dxvk_host_wait.py -v
```

Portable API stubs test call ordering, failure/errno contracts and mutual
exclusion; they are explicitly not Apple OS evidence. The host fixture prints
bounded lock-wait wall time and thread CPU time without a performance threshold
or iPhone inference. Apple backend verification requires running the same tests
on an actual Mac with the SDK. `SKYRIM_TEST_SANITIZERS=thread` enables race checks;
`SKYRIM_TSAN_DISABLE_ASLR=1` supports the existing Linux `setarch` workaround.
The existing runtime diagnostic/receiver schema is untouched; connecting these
isolated counters to a runtime report is pending a separate decision.

The manual `pc2011-host-wait-checks` workflow calls
`scripts/check-host-wait-mac.sh`: it fetches the pinned public source, runs normal
and targeted ThreadSanitizer checks with real Apple APIs, and preserves the log.
It does not build or sign an iPhone app. Linux normal/stage checks and both
native ThreadSanitizer cases passed.

Actual Apple validation passed on 2026-11-17 in host-check run 2 (public commit
`8e7c3b7`, M2, Xcode 26.6/macOS SDK 26.5): 12 normal tests and two targeted
ThreadSanitizer tests, no skips or failures. This includes actual os_unfair_lock,
QoS request/readback, the complete staged CS translation unit, and zero added
clock reads on uncontended locks. The first run's SDK-path and canonical
temporary-root fixture failures are preserved; production was not changed.

For three deliberately held locks, normal-run wall/CPU totals were
136.016/83.081416 ms for the original spinlock and 138.603/0.024416 ms for the
blocking trial. This demonstrates reduced busy CPU use in the authored
scenario, not reduced waiting latency, iPhone frame gains, fairness or heat.
QoS success confirms API requests/readback only, not effective scheduler
priority or core placement. Production activation and iPhone comparison remain
pending; no new setting or runtime diagnostic field was added.
