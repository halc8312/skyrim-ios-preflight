#import "OwnedHostAudio.h"
#include "OwnedHostAudioRenderGate.h"
#include "OwnedHostAudioControlGate.h"
#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#if defined(OWNED_HOST_SDK_TRACE_TEST)
#include <cstdio>
#endif

// The source render block only uses these lock-free words and the Rust pull.
// No Objective-C messages, allocation, locks, file access or guest calls occur
// in that block. All engine/session work is confined to the controller queue.
static OHAudioRenderGate renderGate;
static OHAudioPlaybackIntent playbackIntent;
static std::atomic<bool> renderFormatFailed{false};
static OHAudioRequestAdmission requestAdmission;
static constexpr uint32_t sourceRate = 48000;
static constexpr uint32_t maxPullFrames = 8192;

static void AudioPhase(uint64_t token, uint32_t phase) {
#if defined(OWNED_HOST_SDK_TRACE_TEST)
    // Public SDK Debug only. No route/device/game names or per-frame output.
    std::fprintf(stderr, "PUBLIC_AUDIO_PHASE %llu %u\n",
                 static_cast<unsigned long long>(token), phase);
#else
    (void)token; (void)phase;
#endif
}

static void ClosePlaybackAdmission() {
    renderGate.close();
    OHHostAudioSetRunningV1(0); // Cheap atomics; no initialization or Apple call.
}

static OSStatus RenderSource(uint64_t generation, BOOL *silence,
                             AVAudioFrameCount frames, AudioBufferList *output) {
    OHAudioRenderScope active(renderGate);
    if (silence) *silence = YES;
    if (frames == 0) return noErr;
    if (!output || output->mNumberBuffers != 2 || frames > maxPullFrames) {
        if (renderGate.accepts(generation)) renderFormatFailed.store(true, std::memory_order_relaxed);
        return kAudio_ParamError;
    }
    const uint32_t bytes = frames * sizeof(float);
    for (uint32_t i = 0; i != 2; ++i) {
        const AudioBuffer &buffer = output->mBuffers[i];
        if (buffer.mNumberChannels != 1 || !buffer.mData || buffer.mDataByteSize < bytes ||
            (reinterpret_cast<uintptr_t>(buffer.mData) % alignof(float)) != 0) {
            if (renderGate.accepts(generation)) renderFormatFailed.store(true, std::memory_order_relaxed);
            return kAudio_ParamError;
        }
    }
    float *left = static_cast<float *>(output->mBuffers[0].mData);
    float *right = static_cast<float *>(output->mBuffers[1].mData);
    std::memset(left, 0, bytes); std::memset(right, 0, bytes);
    if (!renderGate.accepts(generation)) return noErr;
    const uint32_t flags = OHHostAudioPullStereoV1(left, right, frames);
    if (silence) *silence = (flags & 1) ? NO : YES;
    return noErr;
}

static uint32_t RoundedPositive(double value, double scale) {
    const double scaled = value * scale;
    return std::isfinite(scaled) && scaled > 0 && scaled <= UINT32_MAX
        ? static_cast<uint32_t>(std::llround(scaled)) : 0;
}

static uint32_t RouteKind(AVAudioSession *session) {
    uint32_t result = 0;
    for (AVAudioSessionPortDescription *port in session.currentRoute.outputs) {
        AVAudioSessionPort type = port.portType;
        if ([type isEqualToString:AVAudioSessionPortAirPlay]) return 4;
        if ([type isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
            [type isEqualToString:AVAudioSessionPortBluetoothLE] ||
            [type isEqualToString:AVAudioSessionPortBluetoothHFP]) return 3;
        if ([type isEqualToString:AVAudioSessionPortHeadphones] ||
            [type isEqualToString:AVAudioSessionPortLineOut] ||
            [type isEqualToString:AVAudioSessionPortUSBAudio] ||
            [type isEqualToString:AVAudioSessionPortHDMI]) result = 2;
        else if (result == 0 && ([type isEqualToString:AVAudioSessionPortBuiltInSpeaker] ||
                                [type isEqualToString:AVAudioSessionPortBuiltInReceiver])) result = 1;
        else if (result == 0) result = 5;
    }
    return result;
}

@interface OHOwnedAudioController : NSObject {
    dispatch_queue_t _queue;
    AVAudioEngine *_engine;
    AVAudioSourceNode *_source;
    NSArray *_sessionObservers;
    id _configurationObserver;
    OHNativeAudioStatusV1 _status;
    BOOL _wanted;
    BOOL _interrupted;
    BOOL _requiresResume;
    BOOL _servicesLost;
    float _gain;
    uint32_t _muted;
    uint32_t _profile;
    uint64_t _intentGeneration;
}
- (void)enqueue:(dispatch_block_t)block;
- (void)ensureObservers;
- (int32_t)applyIntent:(uint64_t)token;
- (int32_t)controlsGain:(float)gain muted:(uint32_t)muted profile:(uint32_t)profile;
- (OHNativeAudioStatusV1)status;
- (void)readHardware;
- (BOOL)quiesce:(BOOL)deactivateSession;
- (BOOL)beginEngine;
- (void)interruption:(NSUInteger)type options:(NSUInteger)options;
- (void)routeChanged:(NSUInteger)reason;
- (void)configurationChanged:(AVAudioEngine *)engine;
- (void)servicesChanged:(BOOL)lost;
@end

@implementation OHOwnedAudioController
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _queue = dispatch_queue_create("owned-host.audio-lifecycle", DISPATCH_QUEUE_SERIAL);
    _gain = 1.0f;
    _status.version = 1; _status.struct_size = sizeof(_status); _status.state = 1;
    return self;
}

- (void)ensureObservers {
    if (_sessionObservers) return;
    // Controller creation has no Apple audio work. Even the first session
    // access happens inside the background request's bounded caller wait.
    AudioPhase(0, 100);
    AVAudioSession *session = AVAudioSession.sharedInstance;
    NSNotificationCenter *notifications = NSNotificationCenter.defaultCenter;
    __weak OHOwnedAudioController *weakSelf = self;
    id interruption = [notifications addObserverForName:AVAudioSessionInterruptionNotification
        object:session queue:nil usingBlock:^(NSNotification *note) {
            const NSUInteger type = [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
            const NSUInteger options = [note.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
            OHOwnedAudioController *controller = weakSelf;
            if (controller) dispatch_async(controller->_queue, ^{ [controller interruption:type options:options]; });
        }];
    id route = [notifications addObserverForName:AVAudioSessionRouteChangeNotification
        object:session queue:nil usingBlock:^(NSNotification *note) {
            const NSUInteger reason = [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
            OHOwnedAudioController *controller = weakSelf;
            if (controller) dispatch_async(controller->_queue, ^{ [controller routeChanged:reason]; });
        }];
    id reset = [notifications addObserverForName:AVAudioSessionMediaServicesWereResetNotification
        object:session queue:nil usingBlock:^(NSNotification *) {
            OHOwnedAudioController *controller = weakSelf;
            if (controller) dispatch_async(controller->_queue, ^{ [controller servicesChanged:NO]; });
        }];
    id lost = [notifications addObserverForName:AVAudioSessionMediaServicesWereLostNotification
        object:session queue:nil usingBlock:^(NSNotification *) {
            OHOwnedAudioController *controller = weakSelf;
            if (controller) dispatch_async(controller->_queue, ^{ [controller servicesChanged:YES]; });
        }];
    _sessionObservers = @[interruption, route, reset, lost];
    AudioPhase(0, 101);
}

- (void)enqueue:(dispatch_block_t)block {
    dispatch_async(_queue, block);
}

- (void)readHardware {
    AVAudioSession *session = AVAudioSession.sharedInstance;
    _status.actual_sample_rate_hz = RoundedPositive(session.sampleRate, 1);
    _status.io_buffer_duration_us = RoundedPositive(session.IOBufferDuration, 1000000);
    _status.output_channels = static_cast<uint32_t>(session.outputNumberOfChannels);
    _status.route_kind = RouteKind(session);
}

// Invalidate first, stop the graph, then wait outside the render thread before
// releasing/replacing it. A delayed old callback sees a mismatched generation
// and can only write silence. A timed-out graph is retained and cannot restart.
- (BOOL)quiesce:(BOOL)deactivateSession {
    renderGate.close();
    OHHostAudioSetRunningV1(0);
    AudioPhase(_intentGeneration, 70);
    [_engine stop];
    AudioPhase(_intentGeneration, 71);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (renderGate.active_count() != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            _status.state = 7; _status.last_error = 4; return NO;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (_configurationObserver) {
        [NSNotificationCenter.defaultCenter removeObserver:_configurationObserver];
        _configurationObserver = nil;
    }
    // The entire graph is discarded; explicit detach is unnecessary and can
    // itself fail when cleanup follows a partially constructed graph.
    _source = nil; _engine = nil;
    _status.engine_running = 0;
    _status.actual_sample_rate_hz = 0; _status.io_buffer_duration_us = 0; _status.output_channels = 0;
    if (deactivateSession) {
        AudioPhase(_intentGeneration, 72);
        [AVAudioSession.sharedInstance setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:nullptr];
        AudioPhase(_intentGeneration, 73);
    }
    return YES;
}

- (BOOL)beginEngine {
    AudioPhase(_intentGeneration, 1);
    if (!_wanted || _interrupted || _requiresResume || _servicesLost ||
        !playbackIntent.permits(_intentGeneration)) return NO;
    if (_engine.isRunning && renderGate.enabled()) return YES;
    if (![self quiesce:YES]) return NO;
    _status.state = 2; _status.last_error = 0;
    if (OHHostAudioInitializeV1() != 0 || OHHostAudioSetControlsV1(_gain, _muted, _profile) != 0) {
        _status.state = 7; _status.last_error = 1; return NO;
    }
    AVAudioSession *session = AVAudioSession.sharedInstance;
    NSError *error = nil;
    // Preferences are requested while inactive. Their acceptance is not a
    // hardware-format guarantee; actual rate/duration are read after activation.
    AudioPhase(_intentGeneration, 10);
    if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&error]) {
        _status.state = 7; _status.last_error = 2; return NO;
    }
    AudioPhase(_intentGeneration, 11);
    [session setPreferredSampleRate:sourceRate error:nullptr];
    const double requestedFrames = _profile == 1 ? 256.0 : (_profile == 2 ? 1024.0 : 512.0);
    AudioPhase(_intentGeneration, 12);
    [session setPreferredIOBufferDuration:requestedFrames / sourceRate error:nullptr];
    AudioPhase(_intentGeneration, 13);
    if (![session setActive:YES error:&error]) {
        _status.state = 7; _status.last_error = 2; return NO;
    }
    if (!playbackIntent.permits(_intentGeneration)) { [self quiesce:YES]; return NO; }
    AudioPhase(_intentGeneration, 14);
    [self readHardware];
    if (_status.actual_sample_rate_hz == 0 || _status.output_channels == 0) {
        [self quiesce:YES]; _status.state = 7; _status.last_error = 3; return NO;
    }
    const uint64_t generation = renderGate.next_generation();
    @try {
        AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sourceRate channels:2];
        if (!format || format.isInterleaved || format.commonFormat != AVAudioPCMFormatFloat32) {
            [self quiesce:YES]; _status.state = 7; _status.last_error = 3; return NO;
        }
        AudioPhase(_intentGeneration, 20);
        _engine = [[AVAudioEngine alloc] init];
        AudioPhase(_intentGeneration, 21);
        // Ordering experiment: materialize the documented singleton output
        // before mainMixerNode lazily connects it. Phase 25 distinguishes the
        // output getter returning from the following mixer connection; both
        // Debug and Release still use the real hardware-output graph.
        AVAudioOutputNode *outputNode = _engine.outputNode;
        AudioPhase(_intentGeneration, 25);
        if (!outputNode) {
            [self quiesce:YES]; _status.state = 7; _status.last_error = 3; return NO;
        }
        AVAudioMixerNode *mixer = _engine.mainMixerNode;
        AudioPhase(_intentGeneration, 22);
        _source = [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:
            ^OSStatus(BOOL *silence, const AudioTimeStamp *, AVAudioFrameCount frames, AudioBufferList *output) {
                return RenderSource(generation, silence, frames, output);
            }];
        AudioPhase(_intentGeneration, 23);
        [_engine attachNode:_source];
        AudioPhase(_intentGeneration, 24);
        [_engine connect:_source to:mixer format:format];
        // The mixer converts the fixed 48 kHz source to the active output route.
        mixer.outputVolume = 1.0f;
        __weak OHOwnedAudioController *weakSelf = self;
        __weak AVAudioEngine *watchedEngine = _engine;
        _configurationObserver = [NSNotificationCenter.defaultCenter
            addObserverForName:AVAudioEngineConfigurationChangeNotification object:_engine queue:nil
            usingBlock:^(NSNotification *) {
                OHOwnedAudioController *controller = weakSelf;
                AVAudioEngine *engine = watchedEngine;
                if (controller && engine) dispatch_async(controller->_queue, ^{ [controller configurationChanged:engine]; });
            }];
        AudioPhase(_intentGeneration, 30);
        [_engine prepare];
        AudioPhase(_intentGeneration, 31);
        if (![_engine startAndReturnError:&error]) {
            [self quiesce:YES]; _status.state = 7; _status.last_error = 3; return NO;
        }
    } @catch (NSException *) {
        [self quiesce:YES]; _status.state = 7; _status.last_error = 3; return NO;
    }
    // The short intent lock makes cancel and final admission mutually
    // exclusive. Never hold it across an Apple call or render quiescence.
    const bool committed = playbackIntent.commit(_intentGeneration, [] {
        renderFormatFailed.store(false, std::memory_order_relaxed);
        OHHostAudioSetRunningV1(1);
        renderGate.open();
    });
    if (!committed) { [self quiesce:YES]; return NO; }
    AudioPhase(_intentGeneration, 40);
    _status.engine_running = 1; _status.state = 3; _status.engine_restarts += 1;
    [self readHardware];
    return YES;
}

- (int32_t)applyIntent:(uint64_t)token {
    const auto intent = playbackIntent.snapshot();
    if (token == 0 || intent.token != token) return -4;
    _intentGeneration = token; _wanted = intent.wanted; _requiresResume = NO;
    int32_t result;
    if (_wanted) result = [self beginEngine] ? 0 : -1;
    else {
        result = [self quiesce:YES] ? 0 : -1;
        if (result == 0) { _status.state = 1; _status.last_error = 0; }
    }
    return playbackIntent.current(token) ? result : -4;
}

- (int32_t)controlsGain:(float)gain muted:(uint32_t)muted profile:(uint32_t)profile {
    if (!std::isfinite(gain) || gain < 0 || gain > 1 || muted > 1 || profile > 2) return -1;
    if (OHHostAudioInitializeV1() != 0 || OHHostAudioSetControlsV1(gain, muted, profile) != 0) {
        _status.last_error = 1; return -1;
    }
    const BOOL changedProfile = _profile != profile;
    _gain = gain; _muted = muted; _profile = profile;
    if (changedProfile && _wanted && !_interrupted && !_requiresResume && !_servicesLost) {
        if (![self quiesce:YES]) return -1;
        // The app applies controls before a newly published Start. Retire the
        // old graph now, but let that exact later Apply own its new admission.
        if (playbackIntent.permits(_intentGeneration) && ![self beginEngine]) return -1;
    }
    return 0;
}

- (void)interruption:(NSUInteger)type options:(NSUInteger)options {
    if (type == AVAudioSessionInterruptionTypeBegan) {
        _status.interruptions += 1; _interrupted = YES;
        if ([self quiesce:NO]) _status.state = 4;
    } else if (type == AVAudioSessionInterruptionTypeEnded && _interrupted) {
        _interrupted = NO;
        if ((options & AVAudioSessionInterruptionOptionShouldResume) && _wanted && !_requiresResume && !_servicesLost) {
            [self beginEngine];
        } else {
            _requiresResume = playbackIntent.snapshot().wanted;
            if (_requiresResume) playbackIntent.invalidate(ClosePlaybackAdmission);
            _status.state = 1;
        }
    }
}

- (void)routeChanged:(NSUInteger)reason {
    _status.route_changes += 1;
    _status.route_kind = RouteKind(AVAudioSession.sharedInstance);
    if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable && playbackIntent.snapshot().wanted) {
        _requiresResume = YES;
        playbackIntent.invalidate(ClosePlaybackAdmission);
        if ([self quiesce:YES]) _status.state = 5;
        return;
    }
    // Category changes include our own session setup. Engine configuration
    // notification handles real format changes without recursive setup loops.
    if (reason == AVAudioSessionRouteChangeReasonCategoryChange) return;
    if (_wanted && !_interrupted && !_requiresResume && !_servicesLost) {
        AVAudioSession *session = AVAudioSession.sharedInstance;
        if (_engine.isRunning && RoundedPositive(session.sampleRate, 1) == _status.actual_sample_rate_hz &&
            static_cast<uint32_t>(session.outputNumberOfChannels) == _status.output_channels) {
            [self readHardware]; return;
        }
        if ([self quiesce:YES]) [self beginEngine];
    }
}

- (void)configurationChanged:(AVAudioEngine *)engine {
    if (engine != _engine || !_wanted || _interrupted || _requiresResume || _servicesLost) return;
    AVAudioSession *session = AVAudioSession.sharedInstance;
    const uint32_t rate = RoundedPositive(session.sampleRate, 1);
    const uint32_t channels = static_cast<uint32_t>(session.outputNumberOfChannels);
    if (engine.isRunning && rate == _status.actual_sample_rate_hz && channels == _status.output_channels) return;
    if ([self quiesce:YES]) [self beginEngine];
}

- (void)servicesChanged:(BOOL)lost {
    _servicesLost = lost; _requiresResume = YES; _interrupted = NO;
    playbackIntent.invalidate(ClosePlaybackAdmission);
    if (!lost) _status.media_resets += 1;
    if ([self quiesce:NO]) _status.state = 6;
    // Apple requires a new user action after media-services reset. Category,
    // preferences and objects are recreated only by that later explicit Start.
}

- (OHNativeAudioStatusV1)status {
    if (renderFormatFailed.load(std::memory_order_relaxed) && _status.state == 3) {
        // Lifecycle work is deferred to this controller call, never the render
        // block. A format failure must not continue to report healthy output.
        playbackIntent.invalidate(ClosePlaybackAdmission);
        [self quiesce:YES]; _requiresResume = YES; _status.state = 7; _status.last_error = 6;
    }
    OHNativeAudioStatusV1 result = _status;
    result.user_wants_playback = playbackIntent.snapshot().wanted;
    result.interrupted = _interrupted;
    result.resume_required = _requiresResume;
    result.profile = _profile;
    result.engine_running = _engine.isRunning && renderGate.enabled();
    result.render_callbacks_active = renderGate.active_count();
    if (renderFormatFailed.load(std::memory_order_relaxed) && result.last_error == 0) result.last_error = 6;
    OHHostAudioReadTransportV1(&result.transport, sizeof(result.transport));
    return result;
}
@end

static OHOwnedAudioController *Controller() {
    static OHOwnedAudioController *controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ controller = [[OHOwnedAudioController alloc] init]; });
    return controller;
}

struct NativeResult {
    int32_t code = -1;
    OHNativeAudioStatusV1 status{};
};

static int32_t Perform(OHOwnedAudioController *controller, uint64_t cancelToken,
                       NativeResult (^operation)(void), NativeResult &out) {
    if (!requestAdmission.begin()) return -5;
    auto completion = std::make_shared<OHAudioCompletion<NativeResult>>();
    [controller enqueue:^{
        if (!completion->begin()) { requestAdmission.finish(); return; }
        NativeResult result;
        @try {
            [controller ensureObservers];
            result = operation();
        } @catch (NSException *) {
            playbackIntent.cancel(cancelToken, ClosePlaybackAdmission);
            result.code = -1;
        }
        requestAdmission.finish();
        completion->finish(result);
    }];
    if (!completion->wait_for(std::chrono::seconds(5), out)) {
        playbackIntent.cancel(cancelToken, ClosePlaybackAdmission);
        return -3;
    }
    return out.code;
}

extern "C" uint64_t OHNativeAudioSetPlaybackIntentV1(uint32_t wanted) {
    return playbackIntent.set(wanted, ClosePlaybackAdmission);
}
extern "C" int32_t OHNativeAudioApplyPlaybackIntentV1(uint64_t token) {
    if (NSThread.isMainThread) return -2;
    if (token == 0) return -1;
    if (!playbackIntent.current(token)) return -4;
    OHOwnedAudioController *controller = Controller();
    NativeResult result;
    return Perform(controller, token, ^NativeResult {
        NativeResult value; value.code = [controller applyIntent:token]; return value;
    }, result);
}
extern "C" int32_t OHNativeAudioStartV1(void) {
    if (NSThread.isMainThread) return -2;
    const auto token = OHNativeAudioSetPlaybackIntentV1(1);
    return token ? OHNativeAudioApplyPlaybackIntentV1(token) : -1;
}
extern "C" int32_t OHNativeAudioStopV1(void) {
    if (NSThread.isMainThread) return -2;
    const auto token = OHNativeAudioSetPlaybackIntentV1(0);
    return token ? OHNativeAudioApplyPlaybackIntentV1(token) : -1;
}
extern "C" int32_t OHNativeAudioSetControlsV1(float gain, uint32_t muted, uint32_t profile) {
    if (NSThread.isMainThread) return -2;
    if (!std::isfinite(gain) || gain < 0 || gain > 1 || muted > 1 || profile > 2) return -1;
    OHOwnedAudioController *controller = Controller();
    NativeResult result;
    return Perform(controller, playbackIntent.snapshot().token, ^NativeResult {
        NativeResult value;
        value.code = [controller controlsGain:gain muted:muted profile:profile]; return value;
    }, result);
}
extern "C" int32_t OHNativeAudioReadStatusV1(OHNativeAudioStatusV1 *out, uint32_t bytes) {
    if (NSThread.isMainThread) return -2;
    if (!out || reinterpret_cast<uintptr_t>(out) % alignof(OHNativeAudioStatusV1) != 0 || bytes != sizeof(*out)) return -1;
    OHOwnedAudioController *controller = Controller();
    NativeResult result;
    const int32_t code = Perform(controller, 0, ^NativeResult {
        NativeResult value; value.status = [controller status]; value.code = 0; return value;
    }, result);
    if (code == 0) *out = result.status;
    return code;
}
