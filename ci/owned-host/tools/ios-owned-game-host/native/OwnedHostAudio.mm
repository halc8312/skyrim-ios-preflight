#import "OwnedHostAudio.h"
#include "OwnedHostAudioRenderGate.h"
#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

// The source render block only uses these lock-free words and the Rust pull.
// No Objective-C messages, allocation, locks, file access or guest calls occur
// in that block. All engine/session work is confined to the controller queue.
static OHAudioRenderGate renderGate;
static std::atomic<bool> renderFormatFailed{false};
static char controllerQueueKey;
static constexpr uint32_t sourceRate = 48000;
static constexpr uint32_t maxPullFrames = 8192;

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
}
- (void)sync:(dispatch_block_t)block;
- (int32_t)start;
- (int32_t)stop;
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
    dispatch_queue_set_specific(_queue, &controllerQueueKey, &controllerQueueKey, nullptr);
    _gain = 1.0f;
    _status.version = 1; _status.struct_size = sizeof(_status); _status.state = 1;
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
    return self;
}

- (void)sync:(dispatch_block_t)block {
    if (dispatch_get_specific(&controllerQueueKey)) block();
    else dispatch_sync(_queue, block);
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
    [_engine stop];
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
        [AVAudioSession.sharedInstance setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:nullptr];
    }
    return YES;
}

- (BOOL)beginEngine {
    if (!_wanted || _interrupted || _requiresResume || _servicesLost) return NO;
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
    if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&error]) {
        _status.state = 7; _status.last_error = 2; return NO;
    }
    [session setPreferredSampleRate:sourceRate error:nullptr];
    const double requestedFrames = _profile == 1 ? 256.0 : (_profile == 2 ? 1024.0 : 512.0);
    [session setPreferredIOBufferDuration:requestedFrames / sourceRate error:nullptr];
    if (![session setActive:YES error:&error]) {
        _status.state = 7; _status.last_error = 2; return NO;
    }
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
        _engine = [[AVAudioEngine alloc] init];
        _source = [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:
            ^OSStatus(BOOL *silence, const AudioTimeStamp *, AVAudioFrameCount frames, AudioBufferList *output) {
                return RenderSource(generation, silence, frames, output);
            }];
        [_engine attachNode:_source];
        [_engine connect:_source to:_engine.mainMixerNode format:format];
        // The mixer converts the fixed 48 kHz source to the active output route.
        _engine.mainMixerNode.outputVolume = 1.0f;
        __weak OHOwnedAudioController *weakSelf = self;
        __weak AVAudioEngine *watchedEngine = _engine;
        _configurationObserver = [NSNotificationCenter.defaultCenter
            addObserverForName:AVAudioEngineConfigurationChangeNotification object:_engine queue:nil
            usingBlock:^(NSNotification *) {
                OHOwnedAudioController *controller = weakSelf;
                AVAudioEngine *engine = watchedEngine;
                if (controller && engine) dispatch_async(controller->_queue, ^{ [controller configurationChanged:engine]; });
            }];
        [_engine prepare];
        OHHostAudioSetRunningV1(1);
        if (![_engine startAndReturnError:&error]) {
            [self quiesce:YES]; _status.state = 7; _status.last_error = 3; return NO;
        }
    } @catch (NSException *) {
        [self quiesce:YES]; _status.state = 7; _status.last_error = 3; return NO;
    }
    renderFormatFailed.store(false, std::memory_order_relaxed);
    renderGate.open();
    _status.engine_running = 1; _status.state = 3; _status.engine_restarts += 1;
    [self readHardware];
    return YES;
}

- (int32_t)start {
    _wanted = YES; _requiresResume = NO;
    return [self beginEngine] ? 0 : -1;
}

- (int32_t)stop {
    _wanted = NO; _requiresResume = NO;
    if (![self quiesce:YES]) return -1;
    _status.state = 1; _status.last_error = 0;
    return 0;
}

- (int32_t)controlsGain:(float)gain muted:(uint32_t)muted profile:(uint32_t)profile {
    if (!std::isfinite(gain) || gain < 0 || gain > 1 || muted > 1 || profile > 2) return -1;
    if (OHHostAudioInitializeV1() != 0 || OHHostAudioSetControlsV1(gain, muted, profile) != 0) {
        _status.last_error = 1; return -1;
    }
    const BOOL changedProfile = _profile != profile;
    _gain = gain; _muted = muted; _profile = profile;
    if (changedProfile && _wanted && !_interrupted && !_requiresResume && !_servicesLost) {
        if (![self quiesce:YES] || ![self beginEngine]) return -1;
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
            _requiresResume = _wanted;
            _status.state = 1;
        }
    }
}

- (void)routeChanged:(NSUInteger)reason {
    _status.route_changes += 1;
    _status.route_kind = RouteKind(AVAudioSession.sharedInstance);
    if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable && _wanted) {
        _requiresResume = YES;
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
    if (!lost) _status.media_resets += 1;
    if ([self quiesce:NO]) _status.state = 6;
    // Apple requires a new user action after media-services reset. Category,
    // preferences and objects are recreated only by that later explicit Start.
}

- (OHNativeAudioStatusV1)status {
    if (renderFormatFailed.load(std::memory_order_relaxed) && _status.state == 3) {
        // Lifecycle work is deferred to this controller call, never the render
        // block. A format failure must not continue to report healthy output.
        [self quiesce:YES]; _requiresResume = YES; _status.state = 7; _status.last_error = 6;
    }
    OHNativeAudioStatusV1 result = _status;
    result.user_wants_playback = _wanted;
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

extern "C" int32_t OHNativeAudioStartV1(void) {
    OHOwnedAudioController *controller = Controller();
    __block int32_t result = -1; [controller sync:^{ result = [controller start]; }]; return result;
}
extern "C" int32_t OHNativeAudioStopV1(void) {
    OHOwnedAudioController *controller = Controller();
    __block int32_t result = -1; [controller sync:^{ result = [controller stop]; }]; return result;
}
extern "C" int32_t OHNativeAudioSetControlsV1(float gain, uint32_t muted, uint32_t profile) {
    OHOwnedAudioController *controller = Controller();
    __block int32_t result = -1;
    [controller sync:^{ result = [controller controlsGain:gain muted:muted profile:profile]; }]; return result;
}
extern "C" int32_t OHNativeAudioReadStatusV1(OHNativeAudioStatusV1 *out, uint32_t bytes) {
    if (!out || reinterpret_cast<uintptr_t>(out) % alignof(OHNativeAudioStatusV1) != 0 || bytes != sizeof(*out)) return -1;
    OHOwnedAudioController *controller = Controller();
    __block OHNativeAudioStatusV1 result{};
    [controller sync:^{ result = [controller status]; }]; *out = result; return 0;
}
