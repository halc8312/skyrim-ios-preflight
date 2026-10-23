/* Public audio transport. No microphone, guest ABI or title data. */
#ifndef OH_OWNED_HOST_AUDIO_H
#define OH_OWNED_HOST_AUDIO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Profiles: balanced=0 (2048 source frames), low=1 (512), robust=2 (4096).
 * The engine's source is always planar Float32 stereo at 48000 Hz. Hardware
 * sample rate/buffer duration are preferences, not guarantees. */
typedef struct OHHostAudioTransportV1 {
    /* Both audio status structs use numeric version=1 (not 0x00010000). */
    uint32_t version, struct_size, initialized, running;
    uint32_t active_clients, profile, sample_rate_hz, channels;
    uint32_t capacity_frames, target_frames, queued_frames_max, muted;
    uint32_t gain_milli, reserved0, reserved1, reserved2;
    /* Frames here mean stereo sample frames, not 256-frame HLE packets.
     * submitted/consumed/underrun/overflow/stale are summed across clients;
     * rendered/non_silent count output frames. All counters are cumulative. */
    uint64_t submitted_frames, consumed_frames, rendered_frames, non_silent_frames;
    uint64_t underrun_frames, overflow_frames, stale_frames, nonfinite_samples;
    uint64_t clipped_samples, rejected_events, transport_epoch, controls_revision;
} OHHostAudioTransportV1;

/* state: 0 uninitialized, 1 stopped, 2 starting, 3 running, 4 interrupted,
 * 5 route paused, 6 media services reset/lost, 7 failed.
 * route_kind: 0 unavailable, 1 built-in speaker/receiver, 2 wired/headphones,
 * 3 Bluetooth, 4 AirPlay, 5 other. No device names are exported.
 * last_error: 0 none, 1 transport init, 2 session setup/activation,
 * 3 engine format/start, 4 render did not quiesce, 6 invalid render format.
 * Native lifecycle/status calls serialize internally; never call them from
 * an audio render callback. Repeated Start/Stop are safe. */
typedef struct OHNativeAudioStatusV1 {
    uint32_t version, struct_size, state, user_wants_playback;
    uint32_t engine_running, interrupted, resume_required, actual_sample_rate_hz;
    uint32_t io_buffer_duration_us, output_channels, route_kind, profile;
    uint32_t last_error, render_callbacks_active, reserved0, reserved1;
    uint64_t route_changes, interruptions, media_resets, engine_restarts;
    OHHostAudioTransportV1 transport;
} OHNativeAudioStatusV1;

int32_t OHNativeAudioStartV1(void);
int32_t OHNativeAudioStopV1(void);
int32_t OHNativeAudioSetControlsV1(float gain, uint32_t muted, uint32_t profile);
/* Exact-size, aligned, exclusive output. On error it is left untouched. */
int32_t OHNativeAudioReadStatusV1(OHNativeAudioStatusV1 *out, uint32_t bytes);

/* Rust transport entry points. Initialize/controls/lifecycle are non-render
 * calls. SetRunning(0) invalidates queued audio, without resetting counters.
 * Exactly one render consumer; native engine replacement waits for quiescence.
 * Pull accepts 1..8192 disjoint, aligned writable floats per channel; it has
 * bounded work and performs no allocation, locks, I/O or guest calls. It always
 * fills valid buffers, with zero for missing audio. Return bit 0 means at least
 * one output sample is nonzero. Invalid buffers/count return 0 untouched. */
int32_t OHHostAudioInitializeV1(void);
int32_t OHHostAudioSetRunningV1(uint32_t running);
int32_t OHHostAudioSetControlsV1(float gain, uint32_t muted, uint32_t profile);
uint32_t OHHostAudioPullStereoV1(float *left, float *right, uint32_t frames);
int32_t OHHostAudioReadTransportV1(OHHostAudioTransportV1 *out, uint32_t bytes);

#ifdef __cplusplus
}
static_assert(sizeof(OHHostAudioTransportV1) == 160, "audio transport ABI");
static_assert(sizeof(OHNativeAudioStatusV1) == 256, "native audio status ABI");
#endif
#endif
