# Metal compilation experiments

This isolated experiment compares ways to prepare an **authored** shader library.
It is not connected to the Skyrim application, MoltenVK patches, or shader settings.
The manual `pc2011-metal-compile-checks` workflow runs authored checks only.
The separate `pc2011-metal-legacy-checks` workflow runs archive and option-token
boundaries without requesting a Metal 4 runtime. Neither workflow builds the app.
The tracked experiment contains no Skyrim shaders or game data.

## Separate iPhone comparison app

The `pc2011-metal-phone-probe` manual workflow builds an isolated UIKit app,
`MetalProbe` (`org.skyrimios.metalprobe`, iOS 26 or later). It compares the legacy
and Metal 4 library APIs, synchronously and asynchronously, through legacy
render/compute pipelines. Builds 3 through 5 use three bounded authored workload families:
branching arithmetic, dependent table lookups, and coupled mixing rounds, with
four semantic variants each (twelve cases). Variants change integer expressions
and constants, not just names. Each family assigns every route first exactly
once; a family offset avoids the same variant-to-route mapping in every family.
Runtime input buffers prevent the outputs from being constant expressions.
Each case runs all four routes four times (192 samples), with each route occupying
each order position once. Every render and compute word must match the CPU
oracle before a sample is accepted. The original small fixture remains unchanged
for error and retained-result controls and for the standalone CLI.
This tests a library preparation boundary, not Skyrim frame rate or a complete
Metal 4 renderer. It does not access the game, saves, accounts, or network.

The app records source/build identity, individual timing intervals, errors,
memory headroom and thermal state. It saves immutable JSON snapshots in its own
container and exposes the latest result through the system share sheet. An
unfinished saved run is presented as interrupted after relaunch. Stop and
backgrounding request a stop at the next operation boundary; already issued
compiler/GPU work remains owned until completion on the worker. This is not
Metal cancellation or a hard time limit, and the UI never joins that worker.

Schema 3 records family, semantic variant and first-requested API for each case.
The result UI groups the first sample of each case by family, showing the sum of
library, specialization and both PSO intervals. Old schema 1/2 records retain
their previous display. Different variants can still share compiler caches and
have different compilation costs; equal first-use counts are not proof of empty
OS caches or a backend performance advantage.

`progress_timing` separately measures JSON snapshot encode/decode and the
synchronous progress callback, counting only callbacks that returned normally.
`host_timing.progress_store` measures completed intermediate `writeSnapshot`
calls including validation, JSON and file I/O. It is nested inside the callback
interval and must not be added to it. Intermediate records contain only prior
completed totals; the final record contains all completed progress calls.
Initial/final store calls and the final return snapshot are explicitly excluded,
so the immutable final file does not pretend to know its own write duration.
Library/specialization/PSO clocks exclude these progress intervals.

Build 4 introduced the recording policy retained by build 5. Both keep the 192
serial samples, check stop requests at every operation boundary and save
intermediate results after each four completed samples and
at control boundaries. A normal full run makes 73 progress callbacks. The
final save includes every completed sample. Schemas 4 and 5 write sorted, compact
JSON within the existing 2 MiB store limit. An abrupt process kill can lose the
last four completed serial samples plus work in progress. The separate
scheduling check is saved when it returns.

Build 4's scheduling check submitted two prefetch requests and one urgent request in
32 trials. In the idle condition both prefetch requests finish before the urgent
request starts. In the contended condition the urgent request follows the two
prefetch submissions. Each trial records how many were actually outstanding at
urgent submission, their overlap and the time from urgent submission to callback.
The GPU verifies each returned library against the same integer calculation.
The adapter keeps the original callback results and errors alive through its
worker's checks and drains each returned Metal 4 task before releasing it.

Build 4 reused source banks across APIs and modes: a later route could consume
already-prepared work. Its two scheduling profiles also used different semantic
salts, so profile differences include a workload difference. The [build-5
design](#build-5-first-assigned-scheduling-comparison) addresses that first-assignment
imbalance with distinct sources and one fixed submission priority. Host timestamps
show when the app was waiting. Internal compiler scheduling and resulting game
frame rates each require separate evidence. See the [gameplay scenarios and
developer reports](MOLTENVK_PLAN.md#when-metal-4-could-shorten-a-skyrim-frame).

[Phone workflow run 7](https://codemagic.io/app/6ad977fcf240708df6941076/build/6afd2e9ed133a39c4a1282b4)
built the device and simulator packages from public `4ff4bb84`. All fourteen
package inputs matched development commit `881725ac`. The Mac ran 93 passing
checks. The three Linux-only signing-failure checks passed under WSL. The
paravirtual GPU returned the expected 384 words in six new legacy readbacks.
No prefetch request was outstanding when the urgent request entered in those
two host trials. The simulator exercised the unsupported, restore and share
views. The existing private link delivered the signed 119,309-byte build 4
package. The [first physical build-4 result](#build-4-physical-iphone-result)
records four overlapping foreground requests and the runtime checks on iPhone.

Build-3 preparation passed Windows Metal 6.2 compilation/linking for all twelve
cases without diagnostics, portable CPU golden/sensitivity/order controls in
normal and ASan/UBSan builds, fifteen packaging and twenty-three signing tests,
and six simulator-evidence parser controls. Evidence is in
`local/metal-compile/phone-workloads-windows-04/`. Phone workflow run 6
(`af6789125074ce3eda34e5eb02908a63877002ef`) then passed 64 checks without skips,
both Apple SDK targets, timed Foundation storage and 24 legacy-GPU readbacks
(3,072 words) on the Apple Paravirtual device. Its twelve input sources match
main `4e91844c190530d1746451f067890ecb98641918` exactly. The simulator recorded
one completed progress call, with host-store time nested inside callback time;
initial/final writes were excluded. Four simulator screens were visually reviewed
after collection, leaving the original `visual_review_pending` evidence intact.
Evidence is under `local/metal-probe-ios-ci/run-06/`. The dedicated signed build-3
IPA is 89,640 bytes, SHA-256
`ece5572607e8be9c058d6fe3fe2122202e9947e3612c2423637a653486d7ba27`.
The private comparison-app route served this build at delivery. Live HTTPS transfer/hash and
390-pixel light/dark page checks passed. Two subsequent physical build-3 runs
validated all 384 normal samples; their findings appear below. Metal 4 speedup,
parallel contention and production lifetime remain unverified. Earlier build
evidence below remains historical; production Skyrim is unchanged.

On an Apple host with the iOS 26 SDK, `scripts/build-metal-probe-ios.sh` creates
a fresh unsigned device IPA and build evidence under `local/metal-probe-ios/`.
An explicit `--simulator` produces a separate simulator app archive. Device and
simulator Mach-O platforms are checked independently. The manual CI also runs
the native host/store controls, launches the simulator UI, checks that a new
result from the current source is explicitly unsupported, and captures the
initial, result, restored and sharing screens for visual review. Simulator UI
success is never accepted as physical iPhone or Metal 4 execution evidence.

The dedicated `org.skyrimios.metalprobe` profile is now registered with Apple,
and a private IPA has been signed with the existing certificate for the registered
iPhone. No additional entitlements were requested beyond development identity.
The workflow itself neither signs nor installs the app, and the Skyrim identity
is never substituted. On the phone, start the comparison and share its saved
JSON; the initial backend is selectable. The OS/compiler cache
is not cleared, so first-observed and repeated samples are reported separately
without claiming a cold cache or a measured game performance improvement.

Build-2 phone workflow run 5 (`19adfba`, 2026-11-18 JST) passed 59 relevant
test methods without skips under Xcode 26.6 / SDK 26.5. The Apple Paravirtual
device exercised all three workloads through legacy Metal with both original
and changed runtime inputs: six GPU readbacks, 768 words, all matching the CPU
oracle. Twelve native stderr files were empty. This virtual-GPU evidence does
not establish Metal 4 or physical build-2 execution. Both ARM64 device and
simulator products were built; their twelve source snapshots matched main
`57b800a` byte for byte. The four simulator screens were visually reviewed; a
transient system notification on the result capture was absent from the restored
result capture. Original visual-review flags remain unchanged.

Evidence is retained under `local/metal-probe-ios-ci/run-05/`. The unsigned IPA
is 71,503 bytes; the separately signed and cryptographically verified IPA is
85,754 bytes (SHA-256
`f5b611c5a57ff1f34f3378fcd4d293a132917c41764815ae20e74cb56edf77a7`),
under `private/metal-probe-signing/run-20261118T012351Z-a95e13d8d6/`.
At that delivery the private comparison-app route served build 2. Real HTTPS page,
manifest and package checks passed, including the complete signed-package hash.
Unauthenticated requests returned 404; the other three routes and disabled
Funnel setting were preserved. Fresh live 390 px light/dark screenshots were
visually reviewed. Two subsequently received physical build-2 results are
recorded below; one comparison runs the complete balanced 48-sample sequence.

Earlier build-1 phone workflow run 4 (`9af993d`, 2026-11-18 JST) passed all 56 relevant test
methods on Apple hardware, including four native entry controls and ten store
cases in normal and ASan/UBSan builds. Xcode 26.6 / SDK 26.5 built the ARM64
device and simulator products without compiler warnings. The initial, explicit
unsupported, restored and share-sheet screens were visually reviewed on the
iPhone 17 Pro Max simulator. Device sources matched main `3447be4` byte for byte.
The unsigned device IPA is 59,756 bytes. Its separately signed private copy is
74,013 bytes (SHA-256
`4199fb2d503c55c12dd58534c96a0e25dec8f988046f51f685780136a928b2bc`).
Apple CMS/DER/profile verification, certificate and registered-device matching,
49 code pages, three resources, append-only changes against the original app,
and temporary-key cleanup passed. Evidence remains in
`private/metal-probe-signing/run-20261117T233201Z-d9d6ec7f85/`.
The later physical-phone reports below establish installation and authored GPU
execution. Signing and simulator evidence alone did not satisfy that gate.
Performance and production integration remain unverified.

`scripts/sign-metal-probe-ipa.py` is a local WSL adapter for the existing reviewed
signer and Apple-identity verification tools. Build 3 accepts only the five-file
authored device app, its exact input hash, a matching registered-device profile,
and the existing certificate/key. It does not log in, unseal a key, create a
certificate, install an app or substitute the game's bundle identity. Output
must be a new path under `private/metal-probe-signing/`; a cryptographic verification
failure cannot publish an IPA. Its authored controls are separate from the
completed private signing verification above. The verification helpers require
a modern `cryptography` version exposing `not_valid_before_utc`: WSL 41.0.7
failed this requirement, while private isolated Linux dependencies at 50.0.2
passed without modifying the original helpers. The launcher’s sibling-import
setup was corrected before key use; the subsequent dependency-version failure
is retained in the private signing run. The allowlisted `/metal-probe` OTA product/page
is separately deployed. Real private HTTPS page/manifest/package requests returned
200, the complete delivered IPA matched the signed hash, and unauthenticated
requests returned 404. All three pre-existing routes were unchanged and Funnel
remains disabled. At build-1 delivery, existing light/dark visual evidence was
reused; a fresh live screenshot after the version/size substitution was not
obtained. Build-2 live-page review is recorded above. The build-1 installation
link was sent to the user and two completed physical-phone reports were received.
No link token, account or unique device identifier is recorded in this document.
The live Skyrim updater was not replaced.

## First physical iPhone results

Two user-supplied records from 2026-11-18 08:38 and 08:42 JST passed validation
against the original build manifest (`9af993d`), authored source/options, prescribed
route rotation and expected GPU output. Both report A19 Pro (`applegpu_g18p`),
iOS 26.5.2 and the same device model. The first starts with Metal 4, the second
with legacy. Across both runs, 24 normal samples and four retained-success cases
match all 64 render words and 16 compute words. Eight normal invalid-source
checks and four retained-error cases return the expected nil/NSError; all eight
retention cases preserve input/result ownership and single callback delivery.
Their deliberate zero-duration publication-gate timeout occurs after compiler
completion and does not demonstrate compiler cancellation or a real timeout.

Library creation wall time, median of three samples per route in each run:

| Route | Metal 4 first (ms) | Legacy first (ms) |
| --- | ---: | ---: |
| Legacy synchronous | 0.044000 | 0.024250 |
| Metal 4 synchronous | 0.029875 | 0.045666 |
| Legacy asynchronous | 0.027208 | 0.063666 |
| Metal 4 asynchronous | 0.041333 | 0.033875 |

The ordering reverses for both synchronous and asynchronous comparisons. All
normal library calls span 0.016875–0.142042 ms. This single 1 KiB source, reused
serially with uncontrolled OS/compiler caches, does not reproduce the game's
long compile stalls or establish a backend speedup. Each whole run lasts about
129–130 ms; all sampled thermal states are zero and low-power mode is false.
Observed footprint peaks are 63.30/64.67 MiB and minimum advisory headroom is
3.235/3.234 GiB; snapshots do not bound transient peaks or long-session heat.
The immutable `host_build` false flags describe the earlier unsigned build,
while the runtime-level flags describe this successful physical execution.

Original records remain private and unmodified under
`private/metal-probe-results/20261118-083859/comparison.json` (SHA-256
`83a5cbc162f2af38c776c5fad9c17771a075e5bbd3048cd696287798f066acce`) and
`20261118-084250/comparison.json`
(`5ad4f71b21579144ec00781ae98104d8e6cb8b2844a8624acde8535b947fe69c`).
The build-2 comparison implements multiple more complex authored inputs with
all four routes equally represented at each execution position and fixed
expected outputs. These are integer compiler workloads, not representative
Skyrim shaders or a cold-cache benchmark. Position balancing does not balance
uncontrolled compiler-cache misses, and four repeats do not establish a tail
latency distribution. Keep first-observed and repeated samples separate.
Build-2 Apple compilation and virtual-GPU oracle checks passed as recorded above;
its subsequent physical results are recorded separately below.
Parallel contention and actual Metal 4
MRC lifetime checks are later, independent gates; this ARC probe does not close
them or validate a MoltenVK integration.

## Build-2 physical iPhone results

The 2026-11-18 11:19 and 11:20 JST reports both completed on A19 Pro / iOS 26.5.2,
starting with legacy and Metal 4 respectively. Their complete build manifests
match the distributed `19adfba` artifact and all twelve source hashes. All 96
normal samples match the verified CPU-oracle vectors word for word (12,288
render/compute words). Four retained-success GPU cases and twelve intentional
compiler-error cases also match expectations. No unexpected sample error or
interruption was observed. Every route occupies each position once per case,
but that does not balance which route first encounters a previously unused case.

The first run's initial sample for each case used legacy synchronous compilation:

| Authored workload | Library (ms) | Library + specialization + render/compute PSO (ms) |
| --- | ---: | ---: |
| Branching arithmetic | 136.133 | 301.300 |
| Dependent table lookups | 7.408 | 33.300 |
| Coupled mixing rounds | 17.750 | 91.523 |

Those three samples account for 96.71% of the first run's measured preparation
sum. The other 45 samples peak at 1.182001 ms for the same four-interval sum.
The second run's 48 samples peak at 1.603459 ms; each individual interval is
under 1 ms. This is consistent with substantial reuse, but OS cache contents
remain unobserved. A Metal-4-first second run on already-used sources does not
measure its previously-unseen compilation cost.

Library medians for the three repeated samples, first/second run in milliseconds:

| Workload | Legacy sync | Metal 4 sync | Legacy async | Metal 4 async |
| --- | ---: | ---: | ---: | ---: |
| Branching arithmetic | 0.072 / 0.138 | 0.054 / 0.155 | 0.106 / 0.225 | 0.115 / 0.185 |
| Dependent table lookups | 0.025 / 0.113 | 0.023 / 0.120 | 0.029 / 0.123 | 0.023 / 0.185 |
| Coupled mixing rounds | 0.037 / 0.105 | 0.026 / 0.105 | 0.038 / 0.115 | 0.062 / 0.128 |

There is no consistent backend advantage. Whole-run elapsed time increases from
2.522 to 4.671 seconds while the four preparation intervals total 440.617 then
42.261 ms. The remainder includes progress snapshot/serialization and store
calls, GPU/readback and controls; it is not measured compiler time. The successful
source path implies 155 progress callbacks, not an independently logged callback
count or store-duration measurement. Do not attribute the residual to fsync or
interpret whole-run elapsed time as a Metal 4 regression.

All 196 sampled run/sample boundary environments report thermal zero and low
power off. Observed footprint maxima are 65,192,760 / 66,077,568 bytes and minimum
advisory headroom is 3,474,799,816 / 3,473,915,008 bytes. These short serial runs
do not bound transient peaks, sustained heat, parallel requests or game latency.
Next, balance the first-use route across multiple new authored cases and measure
progress/store overhead separately. No OS-cold-cache claim follows from that
design either. Actual Metal 4 MRC ownership and contention remain separate gates.

Immutable originals are private:
`private/metal-probe-results/20261118-111958/comparison.json` (286,847 bytes,
SHA-256 `862f8544ca32363f8c7e85b580e43b78a6826627d5268dace4d078c1156151fd`)
and `20261118-112019/comparison.json` (286,960 bytes,
`33bb91d2a9808d8ab17dec5f2f54885075b3736493b5337c05914d674b49a44e`).

The immediate question is whether a library produced by `MTL4Compiler` or by an
offline Windows compiler can feed the existing Metal function-specialization,
render-pipeline, and compute-pipeline APIs with the same observable results.
That is narrower than migrating the renderer to Metal 4.

## Build-3 physical iPhone results

The 2026-11-18 12:09 and 12:10 JST records completed on A19 Pro / iOS 26.5.2,
starting with legacy and Metal 4 respectively. Both complete BuildInfo objects
match phone run 6 (`af678912`); all twelve source hashes match main `4e91844`.
Each has 192 samples over twelve cases, correct family/variant/route ordering
and three first-case assignments per API. All 49,152 normal GPU words and 320
retained-control words match the previously verified CPU/GPU evidence. Source,
option and input hashes, expected invalid-source rejection, single callback
delivery and task completion all validate. All 587 progress stores per run
succeeded. Original records remain unchanged and private:

- `private/metal-probe-results/20261118-120936/comparison.json`: 1,178,097 bytes,
  SHA-256 `4cc189de0c829fc23e5152f80f497245924808cb274c8077ec483d3e9def5f1f`.
- `private/metal-probe-results/20261118-121012/comparison.json`: 1,177,985 bytes,
  SHA-256 `2e61262da03a7f81c39e892b14d6e6f79fe917fd8bc45231eb2e490ea5b7f27e`.

Preparation means the sum of library, function specialization, render PSO and
compute PSO wall intervals. It is not GPU execution time or CPU utilization.

| Interval or partition | First run (ms) | Second run (ms) |
| --- | ---: | ---: |
| All 192 preparation samples | 740.473878 | 26.471767 |
| Twelve case-first samples | 716.720127 | 2.268419 |
| Other 180 samples | 23.753751 | 24.203348 |
| Maximum among those 180 | 0.216124 | 0.592417 |
| Snapshot JSON encode/decode | 1,814.581592 | 1,841.401300 |
| Synchronous progress callback | 4,734.074413 | 4,639.225461 |
| Store, already inside callback | 4,730.736542 | 4,635.900241 |
| Whole runtime interval | 7,530.080084 | 6,751.027292 |

Snapshot plus callback accounts for 86.97% and 95.99% of runtime respectively.
Store includes validation, JSON, locking and file I/O, not just disk time; do not
add it again. Initial/final stores and the return snapshot are explicitly outside
these counters. Other work includes device/setup, controls, GPU command handling,
output checks and observation, so the residual is not attributed to one cause.
The growing full report is encoded/decoded and stored at all 587 checkpoints.
This overhead belongs to the isolated measurement app, not the Skyrim renderer;
it is excluded from the preparation clocks but may still perturb the environment.

First-run case-first preparation, with a different semantic variant assigned to
each API in each family (therefore not a same-input backend comparison):

| Family | Legacy sync (ms) | Metal 4 sync (ms) | Legacy async (ms) | Metal 4 async (ms) |
| --- | ---: | ---: | ---: | ---: |
| Branching arithmetic | 208.996 | 35.421 | 34.999 | 36.066 |
| Dependent table lookups | 32.607 | 31.896 | 30.411 | 32.771 |
| Coupled mixing rounds | 68.242 | 68.902 | 70.027 | 66.382 |

The first legacy sample's library alone took 128.686500 ms. It is also the first
normal request in the process and compiler sequence; its excess cannot be assigned
to the legacy API. Other first libraries took about 6–8 ms regardless of route.
For coupled mixing, the two legacy PSO stages together took about 53 ms and are
outside the Metal 4 library replacement. This differs from the game's previously
observed MSL-dominated long waits. Run two reused identical case sources/options;
all its sample preparation sums were at most 0.805668 ms. Neither equal leader
counts nor changing the initial selector proves a cold OS cache or an API speedup.

All 386 environment observations per run reported thermal 0, low-power off and
successful footprint queries. Observed maximum footprints were 84,083,536 and
68,879,256 bytes; minimum advisory headroom was 3,455,909,040 and 3,471,113,320
bytes. These snapshots do not establish transient peaks or sustained thermal
behavior. Only authored ARC library compatibility through legacy PSOs is proven.

The next device experiment should coalesce progress serialization/persistence
while retaining every sample in the final record, cooperative stop checks at
every operation boundary, store-error handling and explicit interrupted recovery.
Document the maximum unsaved prefix rather than claiming crash-proof capture.
Combine that measurement fix with independent actual Metal 4 MRC task retirement,
production-option equivalence and bounded parallel-contention checks. Repeated
serial synthetic runs alone cannot satisfy those integration gates. This analysis
did not rebuild, modify production, or change defaults.

## Build-4 physical iPhone result

The 13:59 JST legacy-first run completed on the iPhone 17 Pro Max (A19 Pro,
iOS 26.5.2). Its full BuildInfo matched both the delivered signed IPA and the
unsigned run-7 IPA, with all fourteen source fingerprints matching `881725ac`.
The original JSON is `private/metal-probe-results/20261118-135932/comparison.json`
(946,494 bytes, SHA-256 `7edf215be2f5c509d7a32b61377cfd700363ce65dc21d7d7c9970d2c642cdec3`).

Of 32 scheduling trials, 16 waited for both prefetches before submitting the
urgent library request and 16 submitted it immediately after their API calls
returned. Four of the latter still had both prefetches outstanding. The table
shows the urgent request interval from API entry to its completion callback.
Each cell contains one observation from a different source bank.

| Submission priorities | Legacy async | Metal 4 async |
| --- | ---: | ---: |
| Equal | 13.445 ms | 7.835 ms |
| Lower priority for prefetch | 8.593 ms | 7.676 ms |

Every overlap occurred on the first use of a source group in the run. The
other API revisited the same source with the same options after it had already
been compiled. Zero groups gave an overlapping observation on **both** APIs.
The eight first urgent requests ranged from 6.090 to 13.445 ms. The 24 revisits
ranged from 0.014 to 0.029 ms, with no observed overlap. Prefetch and urgent
requests were outstanding together in the host. Their callbacks do not reveal
Metal's internal execution order. The OS cache was uncontrolled. These results
establish neither an API speed ranking nor a Skyrim frame-time improvement.

All 192 serial samples produced the expected 24,576 GPU words. The four serial
rejections and four retained-result controls behaved as expected. The new
scheduling control produced the expected 6,144 words across all 96 requests,
matching a separate integer reference calculation across 24 semantic source
hashes. All 98 normal and deliberate-error requests delivered one callback. In
the 49 Metal 4 cases the adapter adopted the task, waited for it to finish and
released its reference. Each of the two APIs preserved the intended compiler
error after the callback's autorelease pool ended. The run left no owner in
exceptional-join quarantine. This covers the authored worker's success/error
paths. The game's MoltenVK options, timeout paths and long-run memory behavior
remain separate tests.

All 73 intermediate stores succeeded (601 boundaries, 528 coalesced). Snapshot
encode/decode took 228.306 ms and the synchronous callback 561.359 ms, totalling
789.665 ms versus 6,548.656 and 6,480.627 ms in build 3. The 560.831 ms store
interval is already included in the callback. Each callback could still take
up to 17.928 ms. The complete probe took 1,538.605 ms including the 543.359 ms
scheduling suite. All 386 serial/environment observations reported nominal
thermal state and low-power mode off. Observed footprint grew from 24.94 MB to
88.36 MB at serial completion and 110.77 MB after the additional suite.

The follow-up development dry run assigned 32 distinct urgent
sources to two APIs and two modes, eight per condition. In eight four-trial
blocks the condition order was `[0,1,3,2] + block (mod 4)` and the source lane
was `(condition + floor(block / 2)) mod 4`. Each condition occupied each order
position and each source lane twice. The existing salt formula with group
indices 9 through 40 produced 96 distinct source hashes and 96 distinct CPU
output hashes across the three roles, with no overlap with build 4's 24 source
or output hashes. This verifies the proposed allocations on the development
host. Actual first-request cost, overlap and cache state still require a device.
Keep all planned trials in the primary comparison, and report actual overlap
and submission-queue delay separately.

## Build-5 first-assigned scheduling comparison

Run the **standard set once**: it compares both APIs after the unchanged 192-sample
serial suite. The optional alternate set changes the scheduling source panel and
starting API; a second run is not required to include Metal 4. Repeating the same
selection reuses its inputs and may benefit from existing caches.

The scheduling suite has eight blocks of four conditions: legacy/Metal 4 async,
each with idle and planned-contended submission. Both submission queues use the
same `user_initiated` priority. Each condition receives eight first-assigned
observations, with order positions and source lanes balanced. Each trial gets
three distinct semantic sources for its two prefetches and urgent request:
96 source identities per panel. Standard uses groups 9–40 and alternate 41–72;
the panels are disjoint. Different salts can still have different compile costs
or share compiler work, so this is not an exact-source crossover or a cold-cache
experiment.

The primary per-API statistic is the median over **all eight planned-contended
trials**, alongside the eight idle references. The interval runs from urgent
API entry to callback entry. Actual host overlap is a separate `n/8` count;
zero means unobserved, not zero latency or a failed run. Filtering the primary
median to overlapping requests would discard fast completions. Queue delay and
the original timestamps remain in the JSON. Analyze an exported result with:

```sh
python experiments/metal-compile/first_use_results.py <export.json>
```

The outer comparison is schema 5 and the scheduling result is nested schema 2.
Compact storage, the 2 MiB limit, 73 normal progress callbacks and cooperative
stop/recovery behavior remain unchanged. All **192 distinct sources across both
panels** have compiled and linked with the official Windows Metal toolchain.
OS/compiler caches are uncontrolled. The host timestamps measure when the
application calls each API and when its completion callback enters.

[Phone workflow run 10](https://codemagic.io/app/6ad977fcf240708df6941076/build/6afd4913d133a39c4a128a30)
built the signed package's device inputs and the simulator app from public
`7090f43b`. Its 14 input hashes match development commit `e0351c02`. Apple ran
105 passing checks and skipped three Linux-only cases, which passed on WSL.
The paravirtual GPU produced the expected 384 scheduling words and 3,072 serial
words. The simulator restored and shared an explicit unsupported result. The
separate layout example left saved results intact. The signed 122,980-byte IPA
served over the private HTTPS route matched the locally verified SHA-256.

### Standard set on the iPhone 17 Pro Max

The standard set completed on an A19 Pro running iOS 26.5.2. The device's
BuildInfo matched the delivered build 5, including all 14 source fingerprints.
The 96 distinct scheduling inputs matched the compiled Windows panel. The
6,144 scheduling output words and 24,576 serial output words matched their
expected values.

| Urgent request interval, ms | Trials per API | Legacy async | Metal 4 async |
| --- | ---: | ---: | ---: |
| Median after both prefetches finished | 8 | 6.534 | 6.465 |
| Median when submitted immediately after the prefetches | 8 | 9.139 | 9.170 |
| Maximum in the latter condition | 8 | 13.253 | 10.921 |

In all 16 planned overlaps, both prefetch requests were outstanding when the
urgent API call began. In the two idle groups, all urgent requests started
after the prefetches had finished. Metal 4 was quicker in four of eight contended blocks.
Three legacy and four Metal 4 contended requests exceeded 10 ms. The difference
between the two group medians was 0.031 ms. The smaller observed Metal 4 maximum
alone gives little reason to change the game's default on this sample.

The next gameplay comparison can use the existing build 14 switch for separate
draw-preparation waits. In the build 13 game trace, 1,808.043 ms of draw-side
pipeline lock waits overlapped a 1,870.729 ms frame gap. This switch can test
whether coordinating requests by exact draw state shortens that type of wait.
Score it by gameplay gaps over 100 ms per active minute
and their duration, with other options held fixed and initial cache growth and
saves identified. Those measurements answer a different question from this
phone test's elapsed time between API entry and callback. [Game trace and switch](../../docs/PERFORMANCE_WORK.md)

## What each environment establishes

`mrc_lifetime.py --output NEW_DIRECTORY` runs a separate Foundation-only ownership
fixture on Mac. Its seven authored cases use manual retain/release, copied Blocks,
an expired caller autorelease pool, immediate/late callbacks, warnings, nil task
returns, and Objective-C exceptions before/after notification. The default runs
normal, ASan/UBSan, and TSan binaries separately. It tracks its owner, fake task,
and token instances; it does not establish Metal task retirement, arbitrary
`MTLCompileOptions` copying, or production behavior. Portable report controls
cannot substitute for those native runs.

`mrc_metal.py --output NEW_DIRECTORY` connects the same owner-retirement model
to the actual legacy asynchronous library API under manual retain/release.
Its three core cases (success, invalid source and controlled publication) require
retained callback objects and exact compute readbacks where applicable. Optional
`--parallel-cases` launches these on separate host threads, with distinct macro
values checked against the GPU results. It does not establish internal compiler
parallelism. Normal and ASan/UBSan runs remain separate. Controlled publication
is delayed after actual completion; it does not prove compiler cancellation or
a real compilation timeout. Metal 4 task behavior remains unverified.

Run 9 passed the original three core cases in the normal binary, but its fourth
case failed: `#warning` produced a usable library without the expected NSError.
The raw failure remains in `local/metal-compile/mac-run-09/`; the requested
warning-notification boundary is unverified. `--warning-source` retains that
strict opt-in expectation. A successful core-only run cannot validate it.

Run 11 (`ad5739b`) passed both core modes, each in separate normal and
ASan/UBSan processes. All 12 case executions retained their original result/error,
notified once and retired the tracked owner after launch exit; the eight successful
GPU cases matched all 16 words. Parallel requests kept their distinct bias values.
Both sanitized success cases actually notified before launch exit. All 14 compiler
and runtime stderr files for these two units were empty. LeakSanitizer is disabled;
the owner counter is not a whole-Metal leak proof. The optional warning case was
not rerun and remains unverified.

The legacy-only CI workflow accepts an optional `authored_windows_library` build
input. It passes this through an environment variable, never shell-source
interpolation. `windows_fixture.py` accepts only one fixed, authored Windows 6.2
product (macOS 15 / MSL 2.4, 11,352 bytes, SHA-256 pinned in the script) and the
matching source hash, then runs the existing three-route GPU comparison. The
binary/base64 stays in private build input and ignored evidence; no generated
library is committed. A matching hash establishes content identity, not a
signature or reconstructed compiler options. Leave the input at `none` to keep
this additional unit unrequested. An explicitly empty resolved environment value
was rejected by Codemagic before any build step in run10; the nonempty sentinel
is handled only by the workflow guard, not accepted as a library by the verifier.
[Codemagic build inputs](https://docs.codemagic.io/knowledge-codemagic/build-inputs/)
documents the manual per-workflow input mechanism.

Run 9 (`5e37c51`, 2026-11-18 JST) verified that pinned Windows product on the Mac
Apple Paravirtual GPU: three iterations of source, asynchronous source, and
Windows-library loading each matched all 64 render and 16 compute words. This
closes the authored Windows-to-Mac compatibility check only; it does not establish
Metal 4 execution, iPhone performance or production shader coverage.

| Environment | Can establish | Cannot establish |
| --- | --- | --- |
| Windows with Apple Metal Developer Tools | MSL compilation, diagnostics, generated library size, and offline compiler elapsed time | Apple GPU execution, iPhone frame time, memory pressure, thermal behavior, or runtime Metal 4 speed |
| Mac with SDK and macOS 26 or later and a supported Metal device | Legacy and Metal 4 library creation, existing render/compute pipeline compatibility, GPU readback, and local elapsed time | iPhone gameplay improvement or the full patched DXVK/MoltenVK path |
| Target iPhone | Actual game stalls, startup cost, memory headroom, and thermal behavior | Not exercised by these standalone host scripts |

Apple describes Windows tools as shader/asset build tools. They do not supply a
Windows implementation of the iPhone Metal GPU runtime. An offline compiler's
product version is also not a statement that every Metal 4 runtime feature is
supported.

## Fixed fixture contract

`fixture.metal` supplies vertex, fragment, and compute functions. All routes use
MSL 2.4, disabled fast math, `PROBE_BIAS=3`, and function constant 0 equal to 7.
The render output is an exact RGBA32Float color. Four compute threads return
independently prescribed integer vectors. Runtime comparison also tests invalid
source rejection. A missing compiler, incompatible library, failed GPU command,
or unsupported runtime is not a successful validation result.

The default fixture tests the two library factories synchronously. The optional
`--async-library` route also checks both asynchronous factories, owned source and
option snapshots, retained errors/libraries and Metal 4 task completion. Its
deterministic publication gate tests a timed waiter without releasing the
request. That is not an actual compiler timeout, cancellation or production
MoltenVK lifetime test. Priority propagation and compiler-service contention
under game load remain outside this fixture.

`--probe-library-factory` is a separate opt-in diagnostic, run after the normal
comparison (including an unsupported result). It records the default device,
available device names/families, factory selector, and whether the factory can
create the authored library. It performs no function, PSO or GPU work and never
changes the normal Metal 4 eligibility check or unsupported exit code. A factory
success is not evidence that a virtualized device supports the full Metal 4 API.

## Offline preparation

Install Apple's official Windows tools, then identify the directory containing
`metal.exe` and `metallib.exe`. Use a new output directory for each run:

```powershell
python experiments/metal-compile/offline.py --tools 'C:/path/to/Apple/tools/bin' --target ios --deployment 18.0 --output local/metal-compile/ios-offline-01
```

To prepare the optional library for a **Mac** runtime comparison, generate a
macOS-targeted library instead:

```powershell
python experiments/metal-compile/offline.py --tools 'C:/path/to/Apple/tools/bin' --target macos --deployment 15.0 --output local/metal-compile/mac-offline-01
```

These are authored compiler smoke tests, not a match for every production
MoltenVK compile option. Keep compiler versions, exact arguments, source hashes,
output hashes, and failed-command logs. Do not substitute a host target when an
iPhone compilation fails. Windows elapsed time measures the offline preparation
on that PC; it is not comparable to an iPhone's runtime library latency.

`runtime.py` runs the separate Apple host comparison on a Mac with a sufficiently
new SDK and OS:

```sh
python3 experiments/metal-compile/runtime.py --output local/metal-compile/runtime-01 --repeat 3
```

On the same OS/SDK 26 host with an ordinary Metal device, an explicit
`--legacy-only --library /path/to/macos/fixture.metallib` comparison uses only
legacy source creation and offline library loading. It still requires every
render/compute readback, function-constant result and invalid-source rejection
to match. Optional `--async-library` adds only the legacy async factory and its
controlled success/error retention checks; it creates no Metal 4 task. The
Metal 4 factory probe flag remains incompatible. Results list the exact async
APIs and keep all Metal 4 evidence false. The default
Metal 4 comparison and its unsupported result are unchanged.

It can also receive a macOS-targeted library through
`--library /path/to/mac-offline-01/trial-01/fixture.metallib`. An iOS-targeted
library is not interchangeable with a macOS-targeted library. For the first
Windows run, use `--tools` explicitly after checking the official tool version
and installation layout; automatic discovery does not establish target support.

Raw command logs are written directly to files. On timeout or interruption the
supervisor attempts bounded child/group termination and records unsuccessful
cleanup. A process deadline does not cancel external Apple compiler services.
Both runners accept `--budget` for a shared tool-admission and wait allowance;
process startup, input I/O, cleanup and evidence publication are not a guaranteed
hard wall-clock deadline.

## Effective option observations

`options_probe.py` builds a GPU-free Mac probe of `MTLCompileOptions` getters.
Fresh defaults, legacy and modern math assignments, and typed authored macros
are separate cases. It records availability and observed numeric bits without
claiming to recover the original macro type or production assignment history.
See [option snapshot contract](OPTIONS.md) for the fields and limits.

```sh
python3 experiments/metal-compile/options_probe.py --output local/metal-compile/options-01
```

This captures authored option objects only. It does not compile shaders or prove
that the private batch profile matches effective game options.

## Private generated-source batches

`batch.py prepare` snapshots an explicitly selected local MSL set and a complete
declared profile. `batch.py run` verifies all source hashes/options before
starting a serial compile/link sequence. Both require a new private output
directory. The profile includes target, deployment version, MSL version, math
mode/functions, invariance, optimization and macros; missing values are not
inferred. Source/count/time bounds do not impose a compiler-process RAM cap.

On 2026-11-18, 48 previously generated private inputs (803,016 bytes; 29 distinct
source hashes) compiled and linked successfully with Windows tools 6.2 using
iOS 18.0, MSL 3.1, safe/precise math, invariance false, default optimization and
no macros. All 48 warned about the same unused `boolConstants` variable. Inputs
and complete evidence stay under ignored paths. This is a controlled profile,
not a capture of the game's effective MoltenVK options. No GPU output or iPhone
performance result is established. Duplicate source files were intentionally
retained; byte-identical source alone does not imply identical artifact bytes.

The include checker uses a narrow lexical view that preserves quoted comment
markers and line boundaries, including CR/CRLF and standard line splicing.
Raw strings, trigraphs and nonstandard whitespace splices are explicitly
unsupported; the original compiled bytes remain unchanged. This fixes two
authored cases that the prior comment regex missed. It is not a complete C++
preprocessor or a containment boundary. Location/time macros and `#line` still
make identical source/profile hashes insufficient for semantic reuse, so no
duplicate-compilation skipping is enabled.

## Request lifetime models

`--retirement-handoff-only` adds two CPU-only cases and one wrong-thread join
control. An owned fake task is transferred to a waiter before joining; early
notification stays immediate, and a logical destruction request does not cancel
pending work. WSL normal/ASan/UBSan runs passed. Run12 also passed this mode
on Mac in normal, ASan/UBSan and Darwin TSan binaries, with empty stderr in all
three runs. The committed fixture and recorded input hashes match. This is a
proposed ownership protocol test, not Metal task execution.

`request_lifetime.h` is a separate portable model, not connected to the runtime
fixture or production. It waits for both factory return and callback receipt
before notifying an owner exactly once. That matters when notification can
destroy the owner before a returned task is assigned. The model checks early,
late, simultaneous and defensive duplicate notifications, ownership cycles and
notification failures. When the callback precedes factory return, this barrier
delays notification compared with the fixed MoltenVK behavior. It must not be
adopted as a drop-in production fix.

`owner_retirement.h` is the separate design that preserves immediate completion
notification. Destruction is requested idempotently and deferred until the launch
and callback scopes have finished. It retains the returned task without moving
the existing wait/error semantics into a new completion barrier. This model is
also unconnected to production: actual Objective-C ownership and Metal callback
retirement still need their own tests.

Normal and ASan/UBSan runs passed nine authored cases and 300 concurrent event
iterations for each model. An intentionally unsafe model produced the expected ASan use-after-
free report. This is a model counterexample, not an observed early Metal
callback. The WSL TSan run could not start because of an unexpected memory
mapping; it is not a race-detector pass. Actual Metal task retirement still
needs validation on Apple hardware.

The second Apple-host run passed both models with Darwin TSan, alongside the
normal and ASan/UBSan controls. That closes the model's race-detector gap on this
host, but does not establish actual Metal callback ownership or game behavior.

Five additional host-protocol controls cover a provider that returns nil or
throws before/after a callback, and launch unwinding while a callback is held
open. Normal and ASan/UBSan runs passed with immediate single notification,
preserved first payload, a separate launch-fault record and deferred retirement.
These additions do not establish Objective-C unwinding or actual task release;
their Darwin TSan run is still pending.

The Apple workflow additionally runs both safe models with TSan. The portable
test supervisor bounds the entire unittest worker, permits cooperative child
cleanup before a limited process-group termination fallback, and preserves the
worker log. It does not scan or terminate unrelated processes or prove that
external Apple compiler services have stopped.


## Isolated iPhone comparison app

`ios/main.mm` and `runtime_host.h` expose the authored comparison in a separate
UIKit app (`org.skyrimios.metalprobe`, iOS 26). It has no game/container/account
or network access. The user selects which backend starts first, runs three
rotated rounds of four library routes, and shares a JSON report through the
system share sheet. The same fixture, compile options and exact GPU readback
checks are reused; legacy PSO creation remains common to both library backends.

Run the app in the foreground. Stop or leaving the foreground requests a stop
at operation boundaries; already-issued Metal work still drains on the worker.
This is not Metal cancellation or a bounded compiler completion guarantee.
A new run cannot begin while the previous worker is still active. Partial and
failed records are retained, and restarting the app labels unfinished work as
interrupted. JSON includes source/build provenance, ordering, per-stage wall
times and environment snapshots. OS compiler caches are not cleared; a first
observed sample is not a proven cold-cache benchmark.

`scripts/build-metal-probe-ios.sh` creates an **unsigned** physical-device IPA.
Its explicit `--simulator` mode creates a separate preview app. The manual
`pc2011-metal-phone-probe` workflow validates native boundaries and atomic record
storage, builds both targets and captures simulator ready/result/restore/share
screens. The simulator must record `unsupported`, never a Metal 4 GPU pass.
A matching dedicated registered-device profile is now available and the private
device IPA has passed signing verification, as recorded above. The Skyrim app's
bundle/profile is not substituted.

Apple builds, simulator UI review, private signing and the two authored physical
iPhone runs above are complete. The overnight Mac results below are separate
evidence for shared components. Game performance and production lifetime remain
unverified.

## Verification status

Latest completed run: [run12](https://codemagic.io/app/6ad977fcf240708df6941076/build/6afcd2f5d133a39c4a126814),
public source `b8598ac44f0098e88addacc61b101b859458e528`, 2026-11-18 JST.
All 294 Mac controls passed in 30.249 seconds without skips, and all ten requested
independent units passed. The optional Windows product was omitted; its unchanged
cross-host GPU evidence comes from run9. Run10 stopped at Codemagic configuration
validation before any Mac command; the `none` default fixed that separate issue.
Evidence: `local/metal-compile/mac-run-12/` (original artifact 620,067 bytes;
SHA-256 `f010a97072ea247ed577dd37d068d467b5e73044ac0ebccee0aa8249e48e31d3`).
Original failures remain preserved.
No result below establishes Metal 4 runtime or iPhone gameplay improvement.

On 2026-11-18, the integrated WSL control suite passed 105 tests, with the one
Darwin-only TSan check explicitly skipped. Earlier Windows controls and targeted
POSIX process-control checks also passed. These exercise failure handling,
time limits, retained evidence, expected-output validation, and refusal to promote
synthetic results into Apple GPU evidence. They do not compile the native fixture.
After adding archive/token boundaries and interrupt ownership fixes, the expanded
WSL suite passed 150 of 151 checks; the same Darwin-only check was skipped.
The first expanded run exposed a test-harness heredoc extraction error, which
was corrected; its original failed evidence was preserved. The new aggregate
CI helper keeps independent results and never turns unsupported into success.
With the legacy async route and two-float GPU observation added, the next WSL
suite passed 176 of 177 checks in 31.900 seconds; only Darwin TSan was skipped.
The supervisor also passed a real SIGINT-during-child-creation regression.
Evidence is retained in `local/metal-compile/controls-20261118-08/`.

Apple's signed Metal Developer Tools for Windows 6.2 was subsequently installed
in an ignored project-local directory. Its README identifies Xcode 26.2 as the
matching SDK release; the compiler and linker report version 32023.850. The
installer, launchers, and underlying compiler/linker all passed Authenticode
verification. Their hashes and installation details are retained in
`local/metal-compile/toolchain-6.2.json`.

The real compiler rejected the initial generic `-std=metal2.4` argument. The
runner now uses `ios-metal2.4` or `macos-metal2.4` for that language version;
MSL 3 and later retain the unified spelling. Six related control tests passed,
including 12 target/language combinations. After the correction, three iOS 18.0
trials and three macOS 15.0 trials successfully produced AIR and 11,352-byte
metallib files with no stderr diagnostics. The three artifact hashes match within
each target; the iOS and macOS outputs are distinct. Results are retained in
`local/metal-compile/windows-ios-6.2-02/` and
`local/metal-compile/windows-macos-6.2-01/`.

Four additional authored profiles passed one compile/link trial each with empty
stderr: iOS 18 / MSL 3.2, iOS 26 / MSL 4.0, macOS 15 / MSL 3.2, and macOS 26 /
MSL 4.0. This checks accepted compiler inputs and targets; it does not establish
runtime Metal 4 or production compile-option parity.

The original missing-tool result, rejected-argument result, and unsupported
Windows runtime result remain intact. Successful offline compilation does not
establish Metal 4 runtime compatibility, GPU output correctness, or faster iPhone
gameplay. The first manual Apple-host run at public source `7985e6f` passed 37
control tests, three offline macOS compile/link trials and native Objective-C++
compilation with Xcode 26.6 / SDK 26.5. Runtime returned explicit `unsupported`
because the default device did not pass the Metal 4 family check. The original
report did not record the device name or distinguish a missing device from an
unsupported family. This is not a GPU pass.
The Mac generated its own offline library, so it also does not validate a
Windows-produced artifact on a GPU. Original evidence is retained in
`local/metal-compile/mac-run-01/`. No production default, app build or deployment
was changed.

The second Apple-host run at public source `68a6963` passed all 106 controls,
including Darwin TSan, the actual option-getter probe, offline macOS compilation,
and the adapter's `arm64-apple-ios26.0` object compilation. The only device was
`Apple Paravirtual device`: Mac2 supported, Metal3/Metal4/Apple7/8/9 unsupported.
The explicit Metal 4 compiler factory also returned nil with
`MTLDeviceErrorDomain` code 1. The normal runtime stayed `unsupported` (78);
no Metal 4 library, pipeline, or GPU comparison ran. Evidence remains under
`local/metal-compile/mac-run-02/`. This environment can still test legacy Metal
boundaries, but cannot validate the proposed Metal 4 runtime on an iPhone.

The getter probe preserved all 14 authored numeric macro values, including
negative zero, but getters alone do not expose preprocessor token spelling.
The separate [macro diagnostic probe](MACROS.md) observes that next boundary
through intentional compiler rejection; it does not infer original numeric types.

The third Apple-host run at public source `5539d19` passed all 151 portable
controls (including Darwin TSan), SDK object checks, option getters, offline
compilation and actual macro-token observation. NSNumber float input `0x3f800001`
expanded to `1`, and negative zero to `-0`, while explicit string macros kept
their submitted tokens. This is a compiler-input observation, not yet a GPU or
game-correctness result; see [the macro boundary](MACROS.md).
The archive fixture failed to link because its runner resolved the `clang++`
symlink to `clang`, changing driver mode and omitting automatic C++ runtime
linking. The runner now preserves the reported executable spelling, with a
symlink regression control. The original failed archive evidence is retained in
`local/metal-compile/mac-run-03/`; corrected legacy archive execution was
subsequently verified in run 5.

Run 4 at public source `c86a082` passed all 177 controls, including Darwin TSan.
The two-float macro probe completed actual compute readback and confirmed both
NSNumber/string differences above. Legacy synchronous and asynchronous source
routes also completed matching first-iteration render/compute outputs. However,
offline libraries loaded without exposing the expected functions: the archive
fixture stopped before harvesting, and runtime comparison stopped on its first
offline vertex lookup. Neither full comparison passed. The linker invocation
used resolved `air-lld` rather than `metallib`. Actual symbol inspection found the
entry points in AIR and none in the 92-byte libraries. The runner now preserves
the invocation name and records the resolved binary separately; a symlink
regression covers discovery, version probes and linking. The corrected legacy
comparison and archive execution passed in run 5. Original evidence and both
installed pipeline-script manuals are retained in `local/metal-compile/mac-run-04/`.

Run 5 at public source `fb225fe` passed all 184 controls without a skip. After the
linker fix, legacy synchronous source, asynchronous source and offline-library
routes completed three iterations each with identical 64-word render and
16-word compute readbacks. The offline IR was generated on that Mac with Metal
compiler 32023.883; no Windows-built library was loaded. Controlled async retention
and invalid-source checks also passed. The archive fixture saved one kernel and, in a different process,
strictly found it, strictly rejected an unrecorded kernel, explicitly compiled
the fallback and verified both outputs. Empty/malformed archives were rejected
and saved bytes stayed unchanged. Each native archive process made zero source
library compiles and one IR-library URL load. Evidence is in
`local/metal-compile/mac-run-05/`. These are legacy APIs on the Paravirtual Mac
GPU, not Metal 4 or iPhone gameplay validation. The first and subsequent routes
have different OS-cache histories; their elapsed-time ratio is not a speedup
measurement.

Run 6 at public source `c3c12ae` passed all 214 controls in 18.904 seconds without
a skip, and all six independent units passed. The specialized archive restored
constant 7 strictly, rejected constant 11 for the same kernel name, and produced
the different prescribed output through the explicit value-11 fallback. The
integer macro probe observed `(4294967295, 8, 1)` for NSNumber and the unsuffixed
string, versus `(4294967295, 4, 0)` for the explicit uint string; see
[the three-expression comparison](MACROS.md). The `Apple Paravirtual device`
reported architecture `air64_v27`. Offline IR was generated on that Mac
with Metal compiler 32023.883, not on Windows. Evidence is retained in
`local/metal-compile/mac-run-06/` and [the run log](https://codemagic.io/app/6ad977fcf240708df6941076/build/6afcb807d133a39c4a125e73).
These legacy API observations do not establish Metal 4 runtime support, iPhone
behavior, production parity or a performance improvement.

## Direct Metal 4 archive boundary (runtime pending)

`archive_metal4.py` prepares a narrower future experiment for a macOS 26 host
with a Metal 4 device. It uses the observed `device.architecture.name`, checks
that exact architecture against the installed offline tool's macOS 26 query,
and generates an archive containing only `archive_recorded` from an authored
IR library containing both `archive_recorded` and `archive_unrecorded`.

Each recorded/unrecorded × supplied/nil-library lookup runs in a separate
process. Nil cases reject any library-path argument and never create an
MTLLibrary in application code. All cases call the archive directly; no
MTL4Compiler or compiler fallback is available. Recorded success additionally
requires exact compute readback through an ordinary Metal command encoder.
Unrecorded names must be rejected explicitly. A recorded nil-library rejection
remains a non-success result (exit 3), and an unsupported device remains exit 78.

```sh
# API compilation only; no device query, shader compilation, or GPU execution:
python3 experiments/metal-compile/archive_metal4.py --compile-only --output local/metal-compile/m4-archive-sdk-01
# Future execution requires an actual Metal 4 capable Mac:
python3 experiments/metal-compile/archive_metal4.py --output local/metal-compile/m4-archive-runtime-01
```

The manual Mac workflow includes only the first command. This compiles a Mac
executable and an iPhone SDK object without creating an iPhone application.
The existing cloud GPU is unsupported for the second command; repeating it
cannot supply that missing hardware. Run 7 compiled this probe with Xcode 26.6 /
SDK 26.5 for Mac and iPhone with zero warnings. All 235 controls passed on that
Mac, including Darwin TSan. The compile-only report has no native runs or GPU
work. Portable controls verify process separation, failed command
preservation, fresh output ownership, and report interpretation; they are not
Metal execution evidence.

`library_free_lookup_verified` means no application-created MTLLibrary was
loaded or supplied in that lookup process. It does not establish that the
driver performed no internal compilation or file loading. Descriptors omitted
from the authored script use SDK defaults, so a failed lookup alone does not
identify the cause. This Mac-generated archive also does not validate the
separate Windows-generated iOS archive or production options.

A separate Windows 6.2 authored preparation used this same two-kernel source
and one-recorded-kernel script for macOS 26 / explicit `applegpu_g13g`. It
produced a 10,032-byte Metal 4 archive in a 28.6-second command sequence; AIR
symbol inspection retained both control functions, and archive architecture
inspection reported only g13g. No matching physical device was observed or
used. Evidence is under `local/metal-compile/archive-macos26-windows-01/`;
this establishes offline script/tool acceptance, not runtime lookup.

## Binary archive boundary

The optional `archive.py --specialized` case uses one authored kernel name with
uint function constant 0 set to 7 for harvesting and 11 for an unrecorded state.
It requires the restored value-7 pipeline to hit strictly, value 11 to miss
strictly, and an explicit ordinary value-11 fallback to produce its different
16-word output. Function creation, constant type/index/value, separate process
identity, unchanged saved bytes and invalid archive controls remain checked.
This case requires macOS 15 or newer for device serialization of specialized
functions. Portable controls and actual Apple execution passed in run 6: both
16-word readbacks matched, the saved 5,899-byte archive stayed unchanged, and
the empty/malformed archive controls were rejected.
The existing constant-free fixture and its evidence remain separate.

`archive.py` tests two authored compute kernels through ordinary Metal APIs,
without requiring Metal 4. It compiles a macOS IR library, harvests one kernel
into a device-built archive, then starts another process to reload both files.
The reload must create that PSO with `FailOnBinaryArchiveMiss` and produce the
exact prescribed integers. The unrecorded second kernel must be rejected by the
strict route before an explicit archive-free fallback succeeds. Empty and
malformed archives must be rejected. An arbitrary NSError is not mislabeled as
a specific archive-miss error, and nil device is explicit unsupported.

```sh
python3 experiments/metal-compile/archive.py --output local/metal-compile/archive-new --budget 180
bash scripts/check-metal-compile-mac.sh --legacy-boundaries
```

Each blocking native phase is logged before entering the API. The runner limits
child time, preserves failures, compares process/device identities and archive
hashes, and never calls a source-library factory in the reload process. This
tests archive plus separately supplied IR, not the export of runtime MTLLibrary
bytes. This constant-free boundary passed on the Paravirtual Mac GPU in run 5.

A separate actual Windows Metal 6.2 smoke test produced a 13,680-byte legacy GPU
archive from one authored compute kernel, using iOS 18 and explicit Apple9
targets. `metal-lipo` reported the requested g16p/g16g targets. Its `.mtlp-json`
script and IR are not Metal 4 `.mtl4-json` serialization. The subsequent
`metal-source` extraction emitted an empty file and is retained as an unsupported
extraction result. Neither result validates the Windows archive on a device.

A second Windows smoke test produced a 31,568-byte render archive for the same
iOS 18 / Apple9 targets. The VS/FS entry points and BGRA8Unorm attachment came
from the authored fixture and Apple's documented configuration schema. A local
source snapshot replaced its indexed function constant with the fixed value 7;
the original source hash was unchanged. The result does not validate indexed
function-constant specialization, a Metal 4 archive, or actual GPU execution.
Evidence is in `local/metal-compile/archive-windows-render-ios18-apple9-01/`.
An explicit `-pack-scripts -pack-metallibs` follow-up produced the same archive
hash. Extraction yielded per-architecture library/object files but no pipeline
configuration script, so script extraction remains unsupported for that trial.
Its original inputs were unchanged; evidence is retained in
`local/metal-compile/archive-windows-packed-render-ios18-apple9-01/`. Do not treat
offline archive extraction as a public runtime MTLLibrary export API.

Metal 4's direct archive lookup remains a separate unverified boundary. The
public [`MTL4LibraryFunctionDescriptor.library`](https://developer.apple.com/documentation/metal/mtl4libraryfunctiondescriptor/library)
and function-name descriptions do not establish a `library=nil` render/compute
lookup contract. The explicit unique-name rule on
[`MTL4BinaryFunctionDescriptor.name`](https://developer.apple.com/documentation/metal/mtl4binaryfunctiondescriptor/name)
belongs to binary-function lookup and cannot be transferred to pipeline lookup.
The next capable-device test should compare separate restore processes with a
known IR library, with no application library/compiler factory calls, and with
an unrecorded function name. Call archive factories directly so fallback cannot
hide a miss. If the library-free case passes exact readbacks, then test two
different libraries with the same entry-point names before relying on name
identity. This would establish only the observed OS/GPU/descriptor case, not
absence of internal library work or a universally portable archive key.

The installed Apple `metal4-pipelines-script.5` manual says a missing or empty
script `library` selects the library passed on the `metal-nt` command line.
That offline-script rule does not establish runtime `library=nil` behavior.

`archive_offline.py` reproduces a narrower Windows boundary using the official
`.mtl4-json` schema: one authored, constant-free IR library feeds one compute
and one render descriptor. It fixes iOS 26, Apple9 and `applegpu_g16p`, checks
the target architecture query and each generated archive with `metal-lipo`,
and preserves sources, tools, scripts, hashes and failures. Commands run one
at a time with a shared 180-second admission/wait budget; no compiler memory
ceiling or hard bound on external services is claimed. It accepts no arbitrary
shader or configuration input, and it is not part of the iPhone app.

```powershell
python experiments/metal-compile/archive_offline.py --tools F:/SkyrimIOS/local/tools/metal-6.2/bin --output local/metal-compile/archive-offline-new
```

The 13 portable controls passed, followed by one real Windows Metal 6.2 run:
all 12 commands succeeded, with compute/render archives of 12,704/15,600 bytes,
both reporting only `applegpu_g16p`. Evidence is in
`local/metal-compile/archive-offline-runner-windows-01/`. This establishes
offline Metal 4 script translation only. GPU loading/readback, library-free
lookup, game settings equivalence and iPhone performance remain unverified.

The optional `--specialized` mode uses one authored function and uint constant
index 0, generates value-7/value-11 archives separately, and checks omission of
the required value. Its 22 portable controls passed. The real Windows 6.2 run
generated distinct 12,368/12,384-byte positive archives, but the omitted-value
case returned exit 0 with a warning and an archive. The runner therefore reports
`failed`, retains both positive results and the negative evidence, and does not
claim the compiler enforces this boundary. This is not a GPU or Skyrim failure.
Evidence: `local/metal-compile/archive-specialized-runner-windows-01/`.
No warning-promotion option was added. Before replaying captured recipes, required
constant completeness needs separate validation; archive existence is insufficient.

```powershell
python experiments/metal-compile/archive_offline.py --specialized --tools F:/SkyrimIOS/local/tools/metal-6.2/bin --output local/metal-compile/archive-specialized-new
```

Family filtering is not an exclusive chip selection: this tool's iOS 26 Apple9
query includes six architectures, including both returned for Apple10. The
fixed `g16p` artifact must not be assumed to match either test phone. Apple's
feature tables identify A17 Pro as Apple9 and A19 Pro as Apple10, but family
support alone does not establish the artifact architecture. Record the actual
[`device.architecture.name`](https://developer.apple.com/documentation/metal/mtlarchitecture/name),
OS and runtime capabilities, then verify loading and matching GPU outputs on
that device. The isolated runtime fixture now records the architecture name;
the production phone diagnostics are unchanged.

```sh
python -m unittest discover -s tests -p 'test_metal_compile_*.py' -v
```

## Before integrating with MoltenVK

The [pinned integration plan](MOLTENVK_PLAN.md) identifies the asynchronous
library boundary, manual ownership requirements, and the remaining runtime
gates. It is a reviewed design, not an applied production patch.
The separate [direct archive plan](ARCHIVE_PLAN.md) explains the earlier lookup
and conversion-metadata work needed to avoid library creation, including the
specialization and eager-cache-restoration limits.

1. Establish actual Apple compilation and matching render/compute readbacks for
   the authored source through the chosen API route. A later representative-MSL
   comparison also needs exact production options. Additional game-data conversion
   was rejected by automatic approval review and remains unperformed; current work
   uses authored inputs or reads existing outputs. Synthetic colors are not game
   fidelity.
2. Preserve the existing source, macros, language version, fast-math settings,
   failures, ownership, and callback lifetime. Do not put a synchronous Metal 4
   compile under the existing device monitor: that would hold the monitor for
   the entire compilation, unlike the current asynchronous launch.
3. Capture request origin before dispatch. The current timing patch captures
   context as values; it does not restore thread-local context inside a reused
   GCD worker. Replacing the API alone does not separate draw and prefetch QoS.
4. Keep started work alive through completion. A host-side timeout is not Metal
   task cancellation. Validate this before adding the new route to the app.
5. Compare one change at a time on the target phone. Preserve the working path
   for unsupported OS/device combinations and evaluate actual frame gaps,
   startup time, failures, memory, and temperature.

## Connection to first-use preparation

The three external reviews' common-recipe proposal still needs actual shader
inputs. The bounded private inventory in `experiments/shader-inputs/` identified
10,475 candidate records across 122 FXP entries, with 6,659 distinct stage/hash
keys (309 VS and 6,350 FS). An authored-only pinned DXSO oracle agrees on 21
synthetic consumption/stage/hash cases under ASan/UBSan. This is not complete FXP
grammar validation or measured runtime pipeline coverage. A subsequent bounded
native check matched consumption/stage/hash for 32 real candidates (16 VS and
16 PS, 61,392 bytes), after 28 authored boundary/limit controls passed. These
children used guard pages, UBSan and OS limits; no SPIR-V compilation or GPU work
was performed. The actual device `.dxvk-cache` has not been located in the
available local files; current numeric diagnostics do not contain that cache.

The next input study would compare existing candidate records and DXVK stage/hash
keys with a private device-cache copy once available. Any new game-data conversion
still needs resolution of the approval-review block above. Generated
fixed-function shaders and unresolved keys remain separate categories. Only then
can a small, known recipe set test early library or final-pipeline preparation.
History-based weights can refine that set later; they cannot supply missing
shader inputs for a first-time player.

## Primary references

- [Apple Metal tools](https://developer.apple.com/metal/tools/)
- [Metal 4 compilation workflow](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api)
- [MTL4LibraryDescriptor](https://developer.apple.com/documentation/metal/mtl4librarydescriptor)
- [MTL4Compiler](https://developer.apple.com/documentation/metal/mtl4compiler)
- [Binary archive harvesting](https://developer.apple.com/documentation/metal/creating-binary-archives-from-device-built-pipeline-state-objects)
