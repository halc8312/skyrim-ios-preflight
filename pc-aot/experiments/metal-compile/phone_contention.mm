// Authored scheduling/lifetime experiment, deliberately compiled without ARC.
#import "phone_contention.h"
#import <Metal/Metal.h>
#import <TargetConditionals.h>
#import <CommonCrypto/CommonDigest.h>
#import <dispatch/dispatch.h>
#import <pthread/qos.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if __has_feature(objc_arc)
#error authored_contention_requires_manual_reference_counting
#endif

#if !TARGET_OS_SIMULATOR && ((defined(__IPHONE_OS_VERSION_MAX_ALLOWED) && __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000) || (defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000))
#define SKYRIM_CONTENTION_HAS_METAL4 1
#else
#define SKYRIM_CONTENTION_HAS_METAL4 0
#endif

namespace SkyrimContention {
using Clock = std::chrono::steady_clock;
using namespace SkyrimContentionWorkload;
#if SKYRIM_CONTENTION_HAS_METAL4
using Compiler = id<MTL4Compiler>;
using Task = id<MTL4CompilerTask>;
#else
using Compiler = id;
using Task = id;
#endif

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static uint64_t ns(Clock::time_point origin) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - origin).count());
}
struct Stopped : std::exception {};
static void checkpoint(BOOL (^continued)(void)) {
    if (continued && !continued()) throw Stopped();
}

template<class T> class Held {
public:
    Held() = default;
    explicit Held(T value) : value_([value retain]) {}
    static Held adopt(T value) { Held result; result.value_ = value; return result; }
    Held(const Held& other) : Held(other.value_) {}
    Held(Held&& other) noexcept : value_(other.value_) { other.value_ = nil; }
    Held& operator=(Held other) noexcept { std::swap(value_, other.value_); return *this; }
    ~Held() { [value_ release]; }
    T get() const { return value_; }
    void reset() { Held empty; std::swap(value_, empty.value_); }
private:
    T value_ = nil;
};

class Queue {
public:
    explicit Queue(qos_class_t qos) {
        value_ = dispatch_queue_create("authored.metal.contention.submit",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, qos, 0));
        require(value_ != nullptr, "submission queue allocation failed");
    }
    ~Queue() { dispatch_release(value_); }
    dispatch_queue_t get() const { return value_; }
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
private:
    dispatch_queue_t value_ = nullptr;
};

static NSString* sha(const void* bytes, size_t length) {
    require(length && length <= 65536, "authored fingerprint input size outside bound");
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes, CC_LONG(length), digest);
    char output[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (unsigned i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i)
        std::snprintf(output + i * 2, 3, "%02x", digest[i]);
    return [NSString stringWithUTF8String:output];
}
static NSString* stringSHA(NSString* source) {
    NSData* data = [source dataUsingEncoding:NSUTF8StringEncoding];
    return sha(data.bytes, data.length);
}
static NSString* bounded(NSString* value) {
    if (!value) return @"";
    return value.length <= 1024 ? value : [value substringToIndex:1024];
}
static NSDictionary* diagnostic(NSError* error) {
    return error ? @{ @"present": @YES, @"domain": bounded(error.domain),
                      @"code": @(error.code), @"description": bounded(error.localizedDescription) }
                 : @{ @"present": @NO };
}

static NSString* const BaseSHA = @"26ed56a8f4d264965eb624911d42cbf925f1329f0f103d58f683fe1c52d07ad6";
// This new exported kernel changes real integer expressions through SALT. It is
// embedded in a provenance-hashed source file, not a new app resource. The base
// fixture remains byte-for-byte unchanged. All matched routes use identical text.
static NSString* const Kernel = @R"MSL(
kernel void phone_contention_kernel(device const uint* input [[buffer(0)]],
                                    device uint4* output [[buffer(1)]],
                                    uint id [[thread_position_in_grid]]) {
    if (id >= 16u) return;
    uint x = input[(id * 3u) & 127u] ^ CONTENTION_SALT;
    uint y = input[(id * 5u + 17u) & 127u] + CONTENTION_SALT;
    uint z = input[(id * 7u + 31u) & 127u] ^ (CONTENTION_SALT * 0x85ebca6bu);
    uint w = input[(id * 11u + 47u) & 127u] + (CONTENTION_SALT ^ 0xc2b2ae35u);
    #pragma unroll
    for (uint r = 0; r < 16u; ++r) {
        const uint a = input[(x + r * 7u) & 127u];
        const uint b = input[(y ^ r ^ id) & 127u];
        switch ((a ^ z ^ r) & 3u) {
            case 0: x = phone_rotate(x + a + w, 5); y ^= x; break;
            case 1: y = phone_rotate(y ^ b ^ x, 11); z += y; break;
            case 2: z = phone_rotate(z + a + y, 17); w ^= z; break;
            default: w = phone_rotate(w ^ b ^ z, 23); x += w; break;
        }
        x = (x ^ (z + CONTENTION_SALT + r)) * 0x7feb352du;
        y += phone_rotate(w ^ a, 7);
        z = (z + (y ^ b)) * 0x846ca68bu;
        w ^= phone_rotate(x + CONTENTION_SALT, 13);
    }
    output[id] = uint4(x, y, z, w);
}
)MSL";

static NSString* sourceFor(NSString* base, uint32_t value, bool invalid = false) {
    return [NSString stringWithFormat:@"%@\n%@#define CONTENTION_SALT 0x%08xu\n%@",
        base, invalid ? @"#error Intentional_Metal_contention_rejection\n" : @"", value, Kernel];
}
[[maybe_unused]] static Held<NSString*> loadBase(NSString* path) {
    NSError* error = nil;
    NSData* bytes = [NSData dataWithContentsOfFile:path options:0 error:&error];
    require(bytes && bytes.length && bytes.length <= 65536, "authored base fixture unavailable/oversized");
    require([sha(bytes.bytes, bytes.length) isEqual:BaseSHA], "authored base fixture fingerprint mismatch");
    auto result = Held<NSString*>::adopt([[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding]);
    require(result.get() != nil, "authored base fixture is not UTF-8");
    return result;
}
static NSDictionary* optionRecord() {
    return @{ @"language_version": @"2.4", @"fast_math_enabled": @NO,
              @"preprocessor_macros": @{ @"PHONE_CASE": @"0", @"PHONE_VARIANT": @"0", @"PHONE_ROUNDS": @"12" } };
}
static Held<MTLCompileOptions*> options() {
    auto value = Held<MTLCompileOptions*>::adopt([MTLCompileOptions new]);
    value.get().languageVersion = MTLLanguageVersion2_4;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    value.get().fastMathEnabled = NO;
#pragma clang diagnostic pop
    value.get().preprocessorMacros = optionRecord()[@"preprocessor_macros"];
    return value;
}
static NSString* optionSHA() {
    NSData* data = [NSJSONSerialization dataWithJSONObject:optionRecord() options:NSJSONWritingSortedKeys error:nullptr];
    return sha(data.bytes, data.length);
}

struct Lifetime { std::atomic<unsigned> created{0}, destroyed{0}; };
struct Inputs {
    std::shared_ptr<Lifetime> lifetime;
    Held<id<MTLDevice>> device;
    Held<Compiler> compiler;
    Held<NSString*> source;
    Held<MTLCompileOptions*> compileOptions;
#if SKYRIM_CONTENTION_HAS_METAL4
    Held<MTL4LibraryDescriptor*> descriptor;
#endif
    Inputs(std::shared_ptr<Lifetime> counts, id<MTLDevice> d, Compiler c,
           NSString* s, MTLCompileOptions* o, bool metal4)
        : lifetime(std::move(counts)), device(d), compiler(c),
          source(Held<NSString*>::adopt([s copy])),
          compileOptions(Held<MTLCompileOptions*>::adopt([o copy])) {
#if SKYRIM_CONTENTION_HAS_METAL4
        if (metal4) {
            if (@available(iOS 26.0, macOS 26.0, *)) {
                descriptor = Held<MTL4LibraryDescriptor*>::adopt([MTL4LibraryDescriptor new]);
                descriptor.get().source = source.get();
                descriptor.get().options = compileOptions.get();
            }
        }
#else
        require(!metal4, "Metal 4 symbols unavailable in this build");
#endif
        ++lifetime->created;
    }
    ~Inputs() { ++lifetime->destroyed; }
};
struct State {
    std::mutex mutex;
    std::condition_variable changed;
    Held<id<MTLLibrary>> library;
    Held<NSError*> error;
    std::string failure;
    unsigned callbacks = 0;
    bool launchDone = false, callbackBodyDone = false, launchFailed = false;
    qos_class_t actualSubmissionQoS = QOS_CLASS_UNSPECIFIED;
    uint64_t enqueued = 0, apiEnter = 0, apiReturn = 0, callbackEnter = 0, callbackDone = 0, launchDoneAt = 0;
};

// The callback captures only shared input/payload owners, never the task owner.
// There is no task -> block -> task cycle. Both callback results are retained
// before the callback pool exits. The caller adopts the +1 new... task, joins on
// its worker without callback locks, then releases that task on the same worker.
class Request : public std::enable_shared_from_this<Request> {
public:
    const bool metal4;
    const Clock::time_point origin;
    const std::shared_ptr<State> state = std::make_shared<State>();
    const std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>();
    std::shared_ptr<Inputs> input;
    Held<Task> task;
    bool submitted = false, drained = false, joined = false, released = false, callerPoolEnded = false;
    uint64_t drainReturned = 0, taskJoinNs = 0;

    Request(id<MTLDevice> device, Compiler compiler, NSString* source, bool isMetal4, Clock::time_point start)
        : metal4(isMetal4), origin(start) {
        // Mutating and draining caller-owned temporary inputs is intentional:
        // actual submission uses independently copied snapshots.
        @autoreleasepool {
            NSMutableString* callerSource = [NSMutableString stringWithString:source];
            auto callerOptions = options();
            input = std::make_shared<Inputs>(lifetime, device, compiler, callerSource, callerOptions.get(), metal4);
            [callerSource setString:@"caller temporary changed after snapshot"];
            callerOptions.get().preprocessorMacros = @{};
        }
        callerPoolEnded = true;
        require([input->source.get() isEqual:source] &&
            [input->compileOptions.get().preprocessorMacros isEqual:optionRecord()[@"preprocessor_macros"]],
            "owned request input snapshot changed");
    }
    void start(dispatch_queue_t queue) {
        require(!submitted, "request submitted twice");
        const auto self = shared_from_this();
        const auto owned = input;
        const auto data = state;
        const auto epoch = origin;
        state->enqueued = ns(origin);
        submitted = true;
        dispatch_async(queue, ^{
            @autoreleasepool {
                @try {
                    try {
                        MTLNewLibraryCompletionHandler callback = ^(id<MTLLibrary> library, NSError* error) {
                            // Keeping owned inputs alive also covers a callback
                            // arriving before the API has returned its task.
                            (void)owned;
                            const uint64_t entered = ns(epoch);
                            @autoreleasepool {
                                @try {
                                    try {
                                        Held<id<MTLLibrary>> keptLibrary(library);
                                        Held<NSError*> keptError(error);
                                        std::lock_guard<std::mutex> lock(data->mutex);
                                        ++data->callbacks;
                                        if (data->callbacks == 1) {
                                            data->callbackEnter = entered;
                                            data->library = std::move(keptLibrary);
                                            data->error = std::move(keptError);
                                        } else data->failure = "duplicate compiler callback";
                                    } catch (const std::exception& e) {
                                        std::lock_guard<std::mutex> lock(data->mutex); data->failure = e.what();
                                    } catch (...) {
                                        std::lock_guard<std::mutex> lock(data->mutex); data->failure = "callback native exception";
                                    }
                                } @catch (NSException* e) {
                                    std::lock_guard<std::mutex> lock(data->mutex);
                                    data->failure = e.name.UTF8String ?: "callback Objective-C exception";
                                }
                            }
                            {
                                std::lock_guard<std::mutex> lock(data->mutex);
                                data->callbackDone = ns(epoch);
                                data->callbackBodyDone = true;
                            }
                            data->changed.notify_all();
                        };
                        data->actualSubmissionQoS = qos_class_self();
                        data->apiEnter = ns(epoch);
                        if (self->metal4) {
#if SKYRIM_CONTENTION_HAS_METAL4
                            if (@available(iOS 26.0, macOS 26.0, *)) {
                                self->task = Held<Task>::adopt([owned->compiler.get()
                                    newLibraryWithDescriptor:owned->descriptor.get() completionHandler:callback]);
                            }
#endif
                            require(self->task.get() != nil, "Metal 4 asynchronous API returned no task");
                        } else {
                            [owned->device.get() newLibraryWithSource:owned->source.get()
                                options:owned->compileOptions.get() completionHandler:callback];
                        }
                        data->apiReturn = ns(epoch);
                    } catch (const std::exception& e) {
                        std::lock_guard<std::mutex> lock(data->mutex);
                        data->failure = e.what(); data->launchFailed = true;
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(data->mutex);
                        data->failure = "launch native exception"; data->launchFailed = true;
                    }
                } @catch (NSException* e) {
                    std::lock_guard<std::mutex> lock(data->mutex);
                    data->failure = e.name.UTF8String ?: "launch Objective-C exception"; data->launchFailed = true;
                }
            }
            {
                std::lock_guard<std::mutex> lock(data->mutex);
                data->launchDoneAt = ns(epoch);
                data->launchDone = true;
            }
            data->changed.notify_all();
        });
    }
    void waitForLaunch() {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait(lock, [&] { return state->launchDone; });
    }
    void drain() {
        if (!submitted || drained) return;
        waitForLaunch();
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            // No callback is promised after an API invocation itself throws.
            // Its copied block owns inputs independently if the framework did
            // accept work. Such a case is explicitly failed, never verified.
            if (!state->launchFailed)
                state->changed.wait(lock, [&] { return state->callbackBodyDone; });
        }
        if (task.get()) {
#if SKYRIM_CONTENTION_HAS_METAL4
            if (@available(iOS 26.0, macOS 26.0, *)) {
                auto before = Clock::now();
                [task.get() waitUntilCompleted];
                taskJoinNs = ns(before);
                joined = true;
            }
#endif
        }
        drained = true;
        drainReturned = ns(origin);
    }
    void verify() const {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->failure.empty()) throw std::runtime_error(state->failure);
        require(drained && !state->launchFailed, "asynchronous request did not drain normally");
        require(state->callbacks == 1 && state->callbackBodyDone && state->launchDone,
                "callback/launch completion count mismatch");
        require(state->callbackEnter >= state->apiEnter && state->apiReturn >= state->apiEnter,
                "request timestamp order invalid");
#if SKYRIM_CONTENTION_HAS_METAL4
        if (metal4) {
            if (@available(iOS 26.0, macOS 26.0, *))
                require(joined && task.get().status == MTL4CompilerTaskStatusFinished, "Metal 4 task not finished after worker join");
        }
#endif
    }
    void releaseAdapterOwners() {
        require(drained, "attempted release before drain");
        task.reset();
        input.reset();
        released = true;
    }
};

// A thrown join is outside the successful Metal task contract. Retain at most
// the current trial's three owners for process lifetime and refuse every later
// run. One active suite plus a fixed-size quarantine prevents unbounded growth.
struct SessionSafety {
    std::mutex mutex;
    bool active = false, disabled = false;
    std::array<std::shared_ptr<Request>, 3> pending;
    unsigned pendingCount = 0;
};
static SessionSafety& sessionSafety() { static SessionSafety state; return state; }
class RunLease {
public:
    explicit RunLease(SessionSafety& state) : state_(state) {
        std::lock_guard<std::mutex> lock(state_.mutex);
        require(!state_.disabled, "contention session disabled after exceptional task join; restart app");
        require(!state_.active, "another contention run is already active");
        state_.active = true;
    }
    ~RunLease() { std::lock_guard<std::mutex> lock(state_.mutex); state_.active = false; }
    RunLease(const RunLease&) = delete;
    RunLease& operator=(const RunLease&) = delete;
private:
    SessionSafety& state_;
};
static void retainFailedJoin(SessionSafety& safety, const std::shared_ptr<Request>& value) noexcept {
    std::lock_guard<std::mutex> lock(safety.mutex);
    safety.disabled = true;
    for (const auto& existing : safety.pending) if (existing == value) return;
    // Only one suite may run and it owns at most three accepted requests. There
    // cannot be a fourth distinct owner after the session is disabled.
    if (safety.pendingCount >= safety.pending.size()) std::terminate();
    safety.pending[safety.pendingCount++] = value;
}

// Every request goes into this worker-owned set BEFORE submission. Exceptions or
// stop unwind through the same drain; no known accepted task is abandoned.
struct DrainingRequests {
    std::vector<std::shared_ptr<Request>> values;
    ~DrainingRequests() {
        for (const auto& value : values) {
            bool failed = false;
            @try { try { value->drain(); } catch (...) { failed = true; } }
            @catch (NSException*) { failed = true; }
            if (failed) retainFailedJoin(sessionSafety(), value);
        }
    }
};

static void readback(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library,
                     const Input& input, uint32_t value, NSMutableDictionary* report,
                     BOOL (^continued)(void) = nil) {
    checkpoint(continued);
    require(library != nil, "successful library callback returned nil");
    NSError* error = nil;
    auto function = Held<id<MTLFunction>>::adopt([library newFunctionWithName:@"phone_contention_kernel"]);
    require(function.get() != nil, "authored scheduling kernel missing");
    auto started = Clock::now();
    auto pipeline = Held<id<MTLComputePipelineState>>::adopt(
        [device newComputePipelineStateWithFunction:function.get() error:&error]);
    report[@"readback_legacy_pso_ns"] = @(ns(started));
    report[@"readback_pso_diagnostic"] = diagnostic(error);
    require(pipeline.get() != nil, "legacy readback compute pipeline failed");
    checkpoint(continued);
    auto inputBuffer = Held<id<MTLBuffer>>::adopt([device newBufferWithBytes:input.data()
        length:sizeof(input) options:MTLResourceStorageModeShared]);
    auto output = Held<id<MTLBuffer>>::adopt([device newBufferWithLength:sizeof(Output) options:MTLResourceStorageModeShared]);
    require(inputBuffer.get() && output.get() && output.get().contents, "readback buffer unavailable");
    std::memset(output.get().contents, 0xa5, sizeof(Output));
    auto command = Held<id<MTLCommandBuffer>>([queue commandBuffer]);
    auto encoder = Held<id<MTLComputeCommandEncoder>>([command.get() computeCommandEncoder]);
    require(command.get() && encoder.get(), "readback command/encoder unavailable");
    [encoder.get() setComputePipelineState:pipeline.get()];
    [encoder.get() setBuffer:inputBuffer.get() offset:0 atIndex:0];
    [encoder.get() setBuffer:output.get() offset:0 atIndex:1];
    [encoder.get() dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
    [encoder.get() endEncoding];
    checkpoint(continued);
    started = Clock::now();
    [command.get() commit];
    // Off the main thread. No deadline pretends that accepted GPU work stopped.
    [command.get() waitUntilCompleted];
    report[@"readback_command_wall_ns"] = @(ns(started));
    report[@"command_diagnostic"] = diagnostic(command.get().error);
    require(command.get().status == MTLCommandBufferStatusCompleted && !command.get().error,
            "authored GPU command failed");
    const auto expected = oracle(input, value);
    const uint32_t* actual = static_cast<const uint32_t*>(output.get().contents);
    NSMutableArray* words = [NSMutableArray arrayWithCapacity:expected.size()];
    bool matched = true;
    for (unsigned index = 0; index < expected.size(); ++index) {
        [words addObject:@(actual[index])];
        matched = matched && actual[index] == expected[index];
    }
    report[@"compute_words"] = words;
    report[@"expected_words_sha256"] = sha(expected.data(), sizeof(expected));
    report[@"actual_words_sha256"] = sha(actual, sizeof(expected));
    report[@"oracle_verified"] = @(matched);
    report[@"command_status"] = @"completed";
    require(matched, "authored scheduling GPU output differs from integer oracle");
}

static NSMutableDictionary* requestRecord(const std::shared_ptr<Request>& request, uint32_t value, unsigned role) {
    request->verify();
    const auto& s = *request->state;
    NSMutableDictionary* report = [NSMutableDictionary dictionaryWithDictionary:@{
        @"status": @"running", @"role": role == 2 ? @"urgent" : @"prefetch", @"role_index": @(role),
        @"salt": @(value), @"source_sha256": stringSHA(request->input->source.get()),
        @"options_sha256": optionSHA(), @"enqueued_ns": @(s.enqueued), @"api_enter_ns": @(s.apiEnter),
        @"observed_submission_thread_qos": @(s.actualSubmissionQoS),
        @"api_return_ns": @(s.apiReturn), @"callback_enter_ns": @(s.callbackEnter),
        @"callback_body_done_ns": @(s.callbackDone), @"launch_body_done_ns": @(s.launchDoneAt),
        @"queue_delay_ns": @(s.apiEnter - s.enqueued), @"api_call_wall_ns": @(s.apiReturn - s.apiEnter),
        @"request_to_callback_ns": @(s.callbackEnter - s.apiEnter),
        @"enqueue_to_callback_ns": @(s.callbackEnter - s.enqueued),
        @"worker_drain_return_ns": @(request->drainReturned), @"task_join_ns": @(request->taskJoinNs),
        @"callback_count": @(s.callbacks), @"callback_pool_exited": @(s.callbackBodyDone),
        @"caller_temporary_pool_exited": @(request->callerPoolEnded), @"owned_input_snapshots_verified": @YES,
        @"metal4_task_adopted": @(request->metal4 && request->task.get() != nil),
        @"metal4_task_joined": @(request->joined), @"metal4_task_finished": @(request->metal4 && request->joined),
        @"library_diagnostic": diagnostic(s.error.get()),
        @"library_present": @(s.library.get() != nil)
    }];
    return report;
}
static void finishRequest(const std::shared_ptr<Request>& request, NSMutableDictionary* report) {
    request->releaseAdapterOwners();
    report[@"adapter_task_reference_released_after_drain"] = @(request->metal4 && request->released);
    report[@"adapter_input_reference_released_after_drain"] = @(request->released);
    report[@"input_owner_created"] = @(request->lifetime->created.load());
    report[@"input_owner_destroyed_at_observation"] = @(request->lifetime->destroyed.load());
    // Framework ownership of copied callbacks may outlive our release. These are
    // adapter-owner counts, not claims about retainCount or driver deallocation.
    report[@"status"] = @"passed";
}

static void overlap(const DrainingRequests& requests, NSMutableDictionary* report) {
    const auto& urgent = *requests.values.at(2)->state;
    std::vector<std::pair<uint64_t, uint64_t>> intersections;
    std::vector<std::pair<uint64_t, int>> events;
    unsigned atUrgent = 0;
    for (unsigned i = 0; i < requests.values.size(); ++i) {
        const auto& state = *requests.values[i]->state;
        events.emplace_back(state.apiEnter, 1); events.emplace_back(state.callbackEnter, -1);
        if (i == 2) continue;
        atUrgent += state.apiEnter <= urgent.apiEnter && urgent.apiEnter < state.callbackEnter;
        const auto first = std::max(state.apiEnter, urgent.apiEnter);
        const auto last = std::min(state.callbackEnter, urgent.callbackEnter);
        if (last > first) intersections.emplace_back(first, last);
    }
    std::sort(events.begin(), events.end());
    int active = 0, maximum = 0;
    for (auto event : events) { active += event.second; maximum = std::max(maximum, active); }
    std::sort(intersections.begin(), intersections.end());
    uint64_t total = 0, end = 0;
    for (auto span : intersections) {
        total += span.second > std::max(end, span.first) ? span.second - std::max(end, span.first) : 0;
        end = std::max(end, span.second);
    }
    report[@"prefetch_inflight_at_urgent_api_enter"] = @(atUrgent);
    report[@"urgent_prefetch_overlap_ns"] = @(total);
    report[@"host_overlap_observed"] = @(total > 0);
    report[@"max_host_requests_inflight"] = @(maximum);
    report[@"urgent_request_to_callback_ns"] = @(urgent.callbackEnter - urgent.apiEnter);
    report[@"urgent_queue_delay_ns"] = @(urgent.apiEnter - urgent.enqueued);
    report[@"urgent_enqueue_to_callback_ns"] = @(urgent.callbackEnter - urgent.enqueued);
}

[[maybe_unused]] static NSMutableDictionary* runTrial(id<MTLDevice> device, Compiler compiler, id<MTLCommandQueue> gpuQueue,
        NSString* base, const Input& input, unsigned profile, unsigned bank, bool metal4, bool contended,
        Clock::time_point epoch, NSMutableArray* records, BOOL (^continued)(void)) {
    NSMutableDictionary* report = [NSMutableDictionary dictionaryWithDictionary:@{
        @"status": @"running", @"profile": profile ? @"urgent_user_initiated_prefetch_utility" : @"neutral_user_initiated",
        @"profile_index": @(profile), @"bank_index": @(bank), @"api": metal4 ? @"metal4_async" : @"legacy_async",
        @"mode": contended ? @"contended" : @"idle", @"jobs": [NSMutableArray array],
        @"urgent_qos": @"user_initiated", @"prefetch_qos": profile ? @"utility" : @"user_initiated",
        @"submitted_work_equal": @YES, @"max_allowed_outstanding": @3,
        @"driver_concurrency_verified": @NO, @"cold_os_cache_verified": @NO
    }];
    [records addObject:report];
    Queue urgentQueue(QOS_CLASS_USER_INITIATED);
    Queue prefetchQueue(profile ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED);
    DrainingRequests requests;
    requests.values.reserve(3);
    bool stopped = false;
    for (unsigned role = 0; role < 3; ++role) {
        if (continued && !continued()) { stopped = true; break; }
        auto request = std::make_shared<Request>(device, compiler, sourceFor(base, salt(profile, bank, role)), metal4, epoch);
        requests.values.push_back(request);
        request->start(role == 2 ? urgentQueue.get() : prefetchQueue.get());
        // Ensure the preceding API has actually been invoked, not merely queued.
        // Callbacks are never gated; warm calls may finish before urgent starts.
        request->waitForLaunch();
        if (!contended && role < 2) request->drain();
    }
    if (requests.values.size() == 3) requests.values[2]->drain();
    for (const auto& request : requests.values) request->drain();
    for (const auto& request : requests.values) request->verify();
    if (continued && !continued()) stopped = true;
    report[@"submitted_requests"] = @(requests.values.size());
    report[@"drained_requests"] = @(requests.values.size());
    if (requests.values.size() == 3) overlap(requests, report);
    // Validation begins only AFTER all compilation callbacks/drains. Its PSO and
    // GPU work cannot inflate the measured urgent compilation intervals above.
    for (unsigned role = 0; role < requests.values.size(); ++role) {
        const auto& request = requests.values[role];
        NSMutableDictionary* job = requestRecord(request, salt(profile, bank, role), role);
        [report[@"jobs"] addObject:job];
        if (!stopped) {
            try {
                readback(device, gpuQueue, request->state->library.get(), input, salt(profile, bank, role), job, continued);
            } catch (const Stopped&) { stopped = true; }
        }
        finishRequest(request, job);
        if (stopped) {
            job[@"status"] = @"drained_not_gpu_validated";
            job[@"readback_skipped_due_to_stop"] = @YES;
        }
    }
    if (stopped) { report[@"status"] = @"stopped"; throw Stopped(); }
    report[@"status"] = @"passed";
    return report;
}

[[maybe_unused]] static void errorControl(id<MTLDevice> device, Compiler compiler, NSString* base, bool metal4,
                         Clock::time_point epoch, NSMutableArray* records) {
    Queue queue(QOS_CLASS_USER_INITIATED);
    DrainingRequests requests;
    auto request = std::make_shared<Request>(device, compiler, sourceFor(base, salt(0, 0, 0), true), metal4, epoch);
    requests.values.push_back(request);
    request->start(queue.get()); request->drain(); request->verify();
    NSMutableDictionary* record = requestRecord(request, salt(0, 0, 0), 2);
    record[@"api"] = metal4 ? @"metal4_async" : @"legacy_async";
    record[@"kind"] = @"intentional_compiler_error_retained_across_callback_pool";
    [records addObject:record];
    require(request->state->library.get() == nil && request->state->error.get() != nil,
            "invalid MSL did not produce retained error and nil library");
    require([request->state->error.get().localizedDescription containsString:@"Intentional_Metal_contention_rejection"],
            "compiler error did not identify the intentional rejection");
    record[@"expected_rejection_verified"] = @YES;
    finishRequest(request, record);
}

static NSDictionary* environment() {
    NSProcessInfo* process = NSProcessInfo.processInfo;
    return @{ @"thermal_state": @(process.thermalState), @"low_power": @(process.lowPowerModeEnabled) };
}

static void runSuite(NSMutableDictionary* report, NSString* path, BOOL metal4First, BOOL (^continued)(void),
                     Clock::time_point epoch) {
    checkpoint(continued);
    require(!NSThread.isMainThread, "contention experiment must not run on the main thread");
    RunLease lease(sessionSafety());
#if !(TARGET_OS_IOS && !TARGET_OS_SIMULATOR && !TARGET_OS_MACCATALYST)
    (void)path; (void)metal4First; (void)epoch;
    report[@"status"] = @"unsupported";
    report[@"unsupported_reason"] = @"physical_iphone_required";
    return;
#else
#if SKYRIM_CONTENTION_HAS_METAL4
    if (@available(iOS 26.0, *)) {
        auto device = Held<id<MTLDevice>>::adopt(MTLCreateSystemDefaultDevice());
        require(device.get() != nil, "default Metal device unavailable");
        if (![device.get() supportsFamily:MTLGPUFamilyMetal4] ||
            ![device.get() respondsToSelector:@selector(newCompilerWithDescriptor:error:)]) {
            report[@"status"] = @"unsupported";
            report[@"unsupported_reason"] = @"metal4_compiler_unavailable";
            return;
        }
        NSError* error = nil;
        auto descriptor = Held<MTL4CompilerDescriptor*>::adopt([MTL4CompilerDescriptor new]);
        auto compiler = Held<Compiler>::adopt([device.get() newCompilerWithDescriptor:descriptor.get() error:&error]);
        report[@"compiler_diagnostic"] = diagnostic(error);
        require(compiler.get() != nil, "Metal 4 compiler creation failed");
        auto gpuQueue = Held<id<MTLCommandQueue>>::adopt([device.get() newCommandQueue]);
        require(gpuQueue.get() != nil, "GPU readback queue unavailable");
        auto base = loadBase(path);
        const auto input = makeInput();
        report[@"base_source_sha256"] = BaseSHA;
        report[@"options"] = optionRecord(); report[@"options_sha256"] = optionSHA();
        report[@"input_sha256"] = sha(input.data(), sizeof(input));
        report[@"input_word_count"] = @(input.size()); report[@"output_word_count"] = @64;
        NSMutableArray* inputs = [NSMutableArray arrayWithCapacity:input.size()];
        for (uint32_t word : input) [inputs addObject:@(word)];
        report[@"input_words"] = inputs;
        // Within each QoS profile all four (API,mode) combinations receive one
        // fresh bank first. The other combinations revisit the SAME sources.
        // Alternating profile order limits, but cannot eliminate, time/cache bias.
        for (unsigned bank = 0; bank < 4; ++bank) {
            for (unsigned profileSlot = 0; profileSlot < 2; ++profileSlot) {
                for (unsigned apiSlot = 0; apiSlot < 2; ++apiSlot) {
                    for (unsigned modeSlot = 0; modeSlot < 2; ++modeSlot) {
                        const auto selected = plan(bank, profileSlot, apiSlot, modeSlot, bool(metal4First));
                        checkpoint(continued);
                        NSMutableDictionary* trial = runTrial(device.get(), compiler.get(), gpuQueue.get(), base.get(), input,
                            selected.profile, bank, selected.metal4, selected.contended, epoch, report[@"trials"], continued);
                        trial[@"api_slot"] = @(apiSlot); trial[@"mode_slot"] = @(modeSlot);
                        trial[@"profile_slot"] = @(profileSlot);
                        trial[@"first_bank_encounter_in_this_probe"] = @(apiSlot == 0 && modeSlot == 0);
                        report[@"completed_trials"] = @([report[@"trials"] count]);
                    }
                }
            }
        }
        for (unsigned slot = 0; slot < 2; ++slot) {
            checkpoint(continued);
            errorControl(device.get(), compiler.get(), base.get(), bool(slot ^ unsigned(metal4First)), epoch,
                         report[@"error_controls"]);
        }
        checkpoint(continued);
        report[@"status"] = @"passed";
        report[@"physical_iphone_verified"] = @YES;
        report[@"authored_gpu_outputs_verified"] = @YES;
        report[@"mrc_async_lifetime_verified"] = @YES;
        report[@"metal4_task_lifetime_verified"] = @YES;
        return;
    }
#endif
    report[@"status"] = @"unsupported";
    report[@"unsupported_reason"] = @"ios26_sdk_and_runtime_required";
#endif
}
} // namespace SkyrimContention

NSDictionary* SkyrimMetalRunPhoneContention(NSString* path, BOOL metal4First, BOOL (^continued)(void)) {
    using namespace SkyrimContention;
    const auto epoch = Clock::now();
    NSDictionary* answer = nil;
    @autoreleasepool {
        NSMutableDictionary* report = [NSMutableDictionary dictionaryWithDictionary:@{
            @"schema": @1, @"scope": @"authored_iphone_mrc_library_scheduling",
            @"status": @"running", @"metal4_first": @(metal4First),
            @"total_trials": @32, @"completed_trials": @0, @"total_normal_requests": @96,
            @"unique_salted_sources": @24, @"maximum_outstanding": @3,
            @"trials": [NSMutableArray array], @"error_controls": [NSMutableArray array],
            @"environment_before": environment(), @"physical_iphone_verified": @NO,
            @"authored_gpu_outputs_verified": @NO, @"mrc_async_lifetime_verified": @NO,
            @"metal4_task_lifetime_verified": @NO, @"production_moltenvk_lifetime_verified": @NO,
            @"compiler_service_parallelism_verified": @NO, @"compiler_service_qos_verified": @NO,
            @"performance_improvement_verified": @NO, @"cold_os_cache_verified": @NO,
            @"cancellation_requested": @NO,
            @"source_scope": @"verified_base_plus_fixed_authored_salted_compute_kernel",
            @"cache_scope": @"first_in_this_probe_is_not_cold_os_cache; sources_revisited_by_matched_routes",
            @"timing_scope": @"monotonic_ns_relative_to_contention_start; api_enter_to_callback_entry_includes_delivery_delay; queue_and_worker_wake_separate",
            @"overlap_scope": @"host_API_request_intervals_only;_no_claim_of_simultaneous_compiler_execution",
            @"idle_scope": @"same_two_prefetch_requests_drained_before_urgent; contended_submits_urgent_after_prefetch_API_returns",
            @"qos_scope": @"documented_submission_queue_QoS; internal_service_scheduling_not_observed",
            @"comparison_scope": @"primary_matched_comparisons_within_same_profile_and_bank; QoS_profiles_have_different_semantic_salts_so_cross_profile_time_difference_is_not_a_QoS_only_effect",
            @"ownership_scope": @"manual_reference_counting; copied_inputs; retained_callback_results; adopted_task; worker_drain_before_adapter_release",
            @"exceptional_join_policy": @"disable_process_session_and_retain_at_most_three_owners; further_runs_refused_until_app_restart",
            @"readback_scope": @"legacy_compute_PSO_and_command_after_all_trial_compilations; not_full_Metal4_rendering",
            @"limits": @"different_semantic_banks_can_have_different_cost; cache_and_time_order_not_eliminated; callbacks_may_finish_before_urgent; started_work_not_cancelled; no_completion_deadline"
        }];
        @try {
            try { runSuite(report, path, metal4First, continued, epoch); }
            catch (const Stopped&) { report[@"status"] = @"stopped"; report[@"stop_requested"] = @YES; }
            catch (const std::exception& e) { report[@"status"] = @"failed"; report[@"failure"] = bounded([NSString stringWithUTF8String:e.what()]); }
            catch (...) { report[@"status"] = @"failed"; report[@"failure"] = @"unknown native exception"; }
        } @catch (NSException* e) {
            report[@"status"] = @"failed";
            report[@"failure"] = bounded(e.name); report[@"exception_reason"] = bounded(e.reason);
        }
        for (NSMutableDictionary* trial in report[@"trials"]) {
            if ([trial[@"status"] isEqual:@"running"]) trial[@"status"] = report[@"status"];
            for (NSMutableDictionary* job in trial[@"jobs"])
                if ([job[@"status"] isEqual:@"running"]) job[@"status"] = report[@"status"];
        }
        for (NSMutableDictionary* control in report[@"error_controls"])
            if ([control[@"status"] isEqual:@"running"]) control[@"status"] = report[@"status"];
        {
            auto& safety = sessionSafety();
            std::lock_guard<std::mutex> lock(safety.mutex);
            report[@"session_disabled_after_exceptional_join"] = @(safety.disabled);
            report[@"retained_owner_count_after_exceptional_join"] = @(safety.pendingCount);
        }
        report[@"environment_after"] = environment(); report[@"elapsed_ns"] = @(ns(epoch));
        // JSON roundtrip severs mutable containers. Return +0 in the caller's
        // pool, not an autoreleased pointer into the pool ending below.
        NSError* error = nil;
        NSData* encoded = [NSJSONSerialization dataWithJSONObject:report options:0 error:&error];
        if (encoded && encoded.length <= 512 * 1024) {
            answer = [[NSJSONSerialization JSONObjectWithData:encoded options:0 error:&error] retain];
        }
        if (!answer) answer = [@{ @"schema": @1, @"status": @"failed",
            @"failure": @"contention report serialization failed or exceeded 512 KiB bound",
            @"physical_iphone_verified": @NO, @"mrc_async_lifetime_verified": @NO } retain];
    }
    return [answer autorelease];
}
