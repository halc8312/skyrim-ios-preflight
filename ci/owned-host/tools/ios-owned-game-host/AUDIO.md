# Native audio transport

The host accepts the existing HLE output: 256 samples per channel, six planar
channels (front left, front right, center, LFE, rear left, rear right), 48 kHz.
Eight independent queues align clients at the same output time and add them.
They do not concatenate unrelated client callbacks. The stereo downmix follows
the existing web host policy: center/rear gain `1/sqrt(2)`, LFE gain `0.5`.
Nonfinite input becomes zero; nominal input and final mixed output are bounded
to `[-1,1]`. Master gain and mute affect the combined output, not separate stems.

`audio_output::initialize()` allocates fixed storage before guest execution.
Install HLE `AudioObserver` version 1 before any client registration and map its
three events to `activate_client`, `submit_frame`, and `deactivate_client` using
the event's exact index and generation. The observer is optional and preserves
the previous web output callback and worker pacing. Notifications run outside
the client mutex. The transport tolerates reordered activation/termination and
tags packets with generation and playback epoch.

The observer protects already queued/drained frames and unregister/reuse during
sample copying. Legacy guest driver handles contain an index only: a submit
that begins after index reuse cannot identify a former client. Existing worker
callback/wrapped-argument lifetime rules are unchanged. This addition does not
claim to resolve every legacy guest callback/unregister race.

Each client queue holds at most 8192 stereo sample frames. Full queues refuse
new packets without overwriting unread samples. Profiles select startup and
rebuffer reservoirs: balanced 2048, low 512, robust 4096 frames. These are not
hardware latency promises. A stopped transport drops submitted packets; the
next Start skips all older epochs and buffers fresh audio. In-flight render
quanta may finish. Gain changes/unmute ramp over 128 samples; mute is immediate
at the next pull.

`OwnedHostAudio.h` defines two host-only, version-1 C structs (160 and 256 bytes)
and the serialized native lifecycle API. Status contains numeric state, actual
active hardware rate/buffer duration, coarse route kind, and transport counters.
It exports no device names, paths or game strings. Counters are approximate
concurrent snapshots; frame units are documented in the header. Underrun counts
include requested active-client frames withheld during initial buffering.
Nonzero output counts prepared samples, not audible sound or successful XMA
decoding. Reserved fields remain zero.

The native backend uses AVAudioSession `.playback`, with no microphone or
background-audio permission. It requests rate/buffer preferences while inactive,
then reads actual values after activation. AVAudioSourceNode supplies planar
Float32 stereo at 48 kHz; the main mixer adapts it to the route's hardware format.
The source render block does not allocate, lock, dispatch, access files, query
Objective-C objects, or call guest code. It only validates buffers, fills/pulls
samples and accesses lock-free atomic words. Pull is bounded to 8192 frames.

Engine/session changes run on a serial controller queue. The shared SC render
gate closes admission and waits up to 500 ms for admitted callbacks before a
graph is discarded. The proof does not depend on an undocumented `stop()`
callback barrier. Retired-generation callbacks can only produce silence;
overlapping callbacks are refused without waiting. A timeout retains the graph
and refuses replacement. Render format failure is reported and quiesced by a
later controller status call, not by the render callback.

Output disconnection pauses playback and requires explicit Start. Interruption
end resumes only when Apple permits it and user playback intent remains set.
Media-services loss/reset discards the graph and requires a new user Start.
Root app integration owns foreground/background playback intent; it must not
start output after the app has suspended. No microphone authorization is asked.

## Public checks

Portable Rust tests cover channel order/downmix, concurrent-client mixing,
nonfinite/clipped samples, prebuffer/underrun, full queues, slot reuse and late
events, stop/profile epochs, mute/ramp, cursor wrap, producer/consumer ordering,
invalid inputs, C layout and zero allocation/deallocation while rendering.
HLE tests cover optional observer delivery outside the mutex, generation tags,
slot reuse during a copied frame and preserved legacy output behavior.

The exact native admission helper also has an authored C++ fixture:

```sh
clang++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror \
  sdk-test/audio_render_gate_test.cpp -o /tmp/audio-render-gate-test
/tmp/audio-render-gate-test
```

Its four cases cover an admitted callback completing before replacement,
retired-token refusal, overlapping-callback refusal and 10,000 retirement
cycles. `NDEBUG` is forbidden. This is a portable concurrency check, not an
Apple audio-engine execution test.

`sdk-test/tests/OwnedHostAudioTests.swift` supplies six serial Simulator SDK
tests for C layouts/controls, playback/start/stop/actual hardware status,
interruption permission/intent, output disconnect, media reset and silent
planar pull without clients. Its report explicitly leaves audible output,
game audio and physical-iPhone verification false. The SDK build/run is a
separate required gate; Linux Rust/C++ checks do not satisfy it.

The known production direct-XMA-MMIO dispatch gap is separate from this output
transport. Successfully moving authored PCM through the mixer or starting an
engine does not prove the game's audio contexts decode or its sound is correct.

## Primary references

- [Xenia audio clients and per-client driver queues](https://github.com/xenia-project/xenia/blob/master/src/xenia/apu/audio_system.h)
- [Xenia planar channel conversion](https://github.com/xenia-project/xenia/blob/master/src/xenia/apu/conversion.h)
- [Apple AVAudioSourceNode](https://developer.apple.com/documentation/avfaudio/avaudiosourcenode)
- [Apple audio-session preferences and actual hardware values](https://developer.apple.com/library/archive/qa/qa1631/_index.html)
- [Apple route changes](https://developer.apple.com/documentation/avfaudio/responding-to-audio-route-changes)
- [Apple interruption handling](https://developer.apple.com/documentation/avfaudio/handling-audio-interruptions)
- [Apple media-services reset](https://developer.apple.com/documentation/avfaudio/avaudiosession/mediaserviceswereresetnotification)
