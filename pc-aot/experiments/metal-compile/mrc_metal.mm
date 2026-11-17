// Authored legacy Metal + MRC ownership probe. No Metal 4 task or game input.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Block.h>
#include "owner_retirement.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#if __has_feature(objc_arc)
#error Compile this authored fixture with -fno-objc-arc
#endif

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
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

static NSString* const SuccessSource = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "#if AUTHORED_WARNING\n#warning authored_nonfatal_diagnostic\n#endif\n"
    "kernel void probe(device uint4* output [[buffer(0)]], uint id [[thread_position_in_grid]]) {\n"
    "if (id < 4) output[id] = uint4(id + AUTHORED_BIAS, (id + 1u) * 7u, 10u, 0x12345678u); }\n";
static NSString* const InvalidSource = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void probe(device uint* output [[buffer(0)]]) { output[0] = definitely_missing_authored_identifier; }\n";
static NSString* const WarningMarker = @"authored_nonfatal_diagnostic";
static NSString* authoredSource(bool invalid) { return invalid ? InvalidSource : SuccessSource; }

static NSDictionary* errorInfo(NSError* error) {
    if (!error) return @{ @"present": @NO };
    return @{ @"present": @YES, @"domain": [error domain] ?: @"", @"code": @([error code]),
              @"description": [error localizedDescription] ?: @"", @"full_description": [error description] ?: @"",
              @"user_info_description": [[error userInfo] description] ?: @"" };
}

struct Payload { Held<id<MTLLibrary>> library; Held<NSError*> error; };
struct NoTask {};
using Bridge = metal_compile_experiment::OwnerRetirement<Payload, NoTask>;
struct State {
    std::mutex mutex;
    std::condition_variable changed;
    bool callerPoolExpired = false, launchExited = false, callbackBodyFinished = false;
    bool beforeLaunchExit = false, retiredAfterLaunch = false;
    unsigned callbacks = 0, notifications = 0, retirements = 0;
    std::string asyncFailure;
    Payload original, published;
    std::atomic<unsigned> liveOwners{0};
};
struct Owner {
    std::shared_ptr<State> state;
    Held<id<MTLDevice>> device;
    Held<NSString*> source;
    Held<MTLCompileOptions*> options;
    Owner(std::shared_ptr<State> stateIn, id<MTLDevice> deviceIn, NSString* sourceIn, unsigned bias, bool warning)
        : state(std::move(stateIn)), device(deviceIn), source(Held<NSString*>::adopt([sourceIn copy])),
          options(Held<MTLCompileOptions*>::adopt([MTLCompileOptions new])) {
        ++state->liveOwners;
        MTLCompileOptions* value = options.get();
        value.languageVersion = MTLLanguageVersion2_4;
        value.preserveInvariance = NO;
        value.optimizationLevel = MTLLibraryOptimizationLevelDefault;
        value.preprocessorMacros = @{ @"AUTHORED_BIAS": [NSString stringWithFormat:@"%uu", bias],
                                      @"AUTHORED_WARNING": warning ? @"1" : @"0" };
        if (@available(macOS 15.0, *)) {
            value.mathMode = MTLMathModeSafe;
            value.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            value.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
    }
    ~Owner() { --state->liveOwners; }
};
typedef void (^Completion)(id<MTLLibrary>, NSError*);
struct CompletionHolder {
    Completion value = nil;
    ~CompletionHolder() { if (value) Block_release(value); }
};

static void asyncFailure(const std::shared_ptr<State>& state, const char* message) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->asyncFailure = message ? message : "unknown callback failure";
    state->changed.notify_all();
}

static void readback(id<MTLDevice> device, id<MTLLibrary> library, unsigned bias, NSMutableDictionary* result) {
    auto function = Held<id<MTLFunction>>::adopt([library newFunctionWithName:@"probe"]);
    require(function.get() != nil, "compiled authored kernel missing");
    NSError* error = nil;
    auto pipeline = Held<id<MTLComputePipelineState>>::adopt([device newComputePipelineStateWithFunction:function.get() error:&error]);
    result[@"pipeline_error"] = errorInfo(error);
    require(pipeline.get() != nil, "legacy compute pipeline creation failed");
    auto buffer = Held<id<MTLBuffer>>::adopt([device newBufferWithLength:16 * sizeof(uint32_t) options:MTLResourceStorageModeShared]);
    require(buffer.get() && [buffer.get() contents], "shared buffer unavailable");
    std::memset([buffer.get() contents], 0xa5, 16 * sizeof(uint32_t));
    auto queue = Held<id<MTLCommandQueue>>::adopt([device newCommandQueue]);
    auto command = Held<id<MTLCommandBuffer>>([queue.get() commandBuffer]);
    auto encoder = Held<id<MTLComputeCommandEncoder>>([command.get() computeCommandEncoder]);
    require(queue.get() && command.get() && encoder.get(), "legacy command setup failed");
    [encoder.get() setComputePipelineState:pipeline.get()];
    [encoder.get() setBuffer:buffer.get() offset:0 atIndex:0];
    [encoder.get() dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
    [encoder.get() endEncoding];
    struct Done { std::mutex mutex; std::condition_variable changed; bool value = false; };
    auto done = std::make_shared<Done>();
    [command.get() addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        (void)completed;
        std::lock_guard<std::mutex> lock(done->mutex);
        done->value = true;
        done->changed.notify_all();
    }];
    [command.get() commit];
    {
        std::unique_lock<std::mutex> lock(done->mutex);
        require(done->changed.wait_for(lock, std::chrono::seconds(10), [&] { return done->value; }),
                "GPU completion budget expired; no cancellation claimed");
    }
    result[@"command_error"] = errorInfo([command.get() error]);
    result[@"command_completed"] = @([command.get() status] == MTLCommandBufferStatusCompleted);
    require([command.get() status] == MTLCommandBufferStatusCompleted && [command.get() error] == nil,
            "GPU command did not complete successfully");
    NSMutableArray* values = [NSMutableArray array];
    const uint32_t* actual = static_cast<const uint32_t*>([buffer.get() contents]);
    bool matches = true;
    for (uint32_t index = 0; index < 4; ++index) {
        const uint32_t expected[4] = {index + bias, (index + 1u) * 7u, 10u, 0x12345678u};
        for (uint32_t component = 0; component < 4; ++component) {
            [values addObject:@(actual[4 * index + component])];
            matches = matches && actual[4 * index + component] == expected[component];
        }
    }
    result[@"compute_words"] = values;
    require(matches, "authored compute readback mismatch");
    result[@"exact_compute_readback_verified"] = @YES;
}

// Only the caller-side start boundary is synchronized. This cannot establish
// how many compiler/GPU jobs Metal actually executes simultaneously.
class LaunchBarrier {
public:
    explicit LaunchBarrier(unsigned participants) : participants_(participants) {}
    void arrive() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++arrived_;
        changed_.notify_all();
        if (!changed_.wait_for(lock, std::chrono::seconds(10), [&] { return cancelled_ || arrived_ == participants_; })) {
            cancelled_ = true;
            changed_.notify_all();
            throw std::runtime_error("parallel launch barrier budget expired");
        }
        require(!cancelled_, "parallel launch barrier canceled after another case failed");
    }
    void cancel() { std::lock_guard<std::mutex> lock(mutex_); cancelled_ = true; changed_.notify_all(); }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    unsigned participants_, arrived_ = 0;
    bool cancelled_ = false;
};

static void runCase(id<MTLDevice> device, NSString* name, unsigned bias, LaunchBarrier* barrier, NSMutableDictionary* result) {
    const bool invalid = [name isEqualToString:@"invalid_source"];
    const bool controlled = [name isEqualToString:@"controlled_publication"];
    const bool warning = [name isEqualToString:@"warning_source"];
    auto state = std::make_shared<State>();
    std::shared_ptr<Bridge> bridge;
    Owner* raw = nullptr;
    CompletionHolder completion;
    @autoreleasepool {
        NSMutableString* callerSource = [NSMutableString stringWithString:authoredSource(invalid)];
        auto owner = std::make_shared<Owner>(state, device, callerSource, bias, warning);
        raw = owner.get();
        bridge = Bridge::create(
            [state, raw, invalid, warning, bias](Payload payload) {
                require(state->callerPoolExpired && [raw->source.get() isEqualToString:authoredSource(invalid)],
                        "owned source snapshot changed or caller pool still active");
                require([raw->options.get() languageVersion] == MTLLanguageVersion2_4 &&
                        [[raw->options.get() preprocessorMacros] isEqualToDictionary:@{
                            @"AUTHORED_BIAS": [NSString stringWithFormat:@"%uu", bias],
                            @"AUTHORED_WARNING": warning ? @"1" : @"0" }], "owned options changed");
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->notifications;
                state->beforeLaunchExit = !state->launchExited;
                state->published = std::move(payload);
                state->changed.notify_all();
            },
            [state, owner](NoTask) mutable {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    ++state->retirements;
                    state->retiredAfterLaunch = state->launchExited;
                }
                owner.reset();
                state->changed.notify_all();
            });
        [callerSource setString:@"mutated caller source is not submitted"];
        auto capturedBridge = bridge;
        completion.value = Block_copy(^(id<MTLLibrary> library, NSError* error) {
            @autoreleasepool {
                @try {
                    try {
                        Payload payload{Held<id<MTLLibrary>>(library), Held<NSError*>(error)};
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            ++state->callbacks;
                            if (state->callbacks == 1) state->original = payload;
                        }
                        if (!controlled) capturedBridge->callback(std::move(payload));
                    } catch (const std::exception& failure) { asyncFailure(state, failure.what()); }
                } @catch (NSException* failure) { asyncFailure(state, [[failure description] UTF8String]); }
            }
            // Our callback pool has drained. This still does not assert task
            // release/join or destruction of Metal's retained completion block.
            std::lock_guard<std::mutex> lock(state->mutex);
            state->callbackBodyFinished = true;
            state->changed.notify_all();
        });
    }
    state->callerPoolExpired = true; // Set before the only provider launch.
    result[@"caller_pool_expired_before_launch"] = @YES;
    if (barrier) barrier->arrive();
    result[@"library_compile_attempts"] = @1;
    @autoreleasepool {
        [raw->device.get() newLibraryWithSource:raw->source.get() options:raw->options.get() completionHandler:completion.value];
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->launchExited = true;
    }
    bridge->finishLaunch(NoTask{});
    if (!controlled) bridge->requestDestroy();
    Payload original;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        require(state->changed.wait_for(lock, std::chrono::seconds(10), [&] {
                    return state->callbackBodyFinished || !state->asyncFailure.empty(); }),
                "library callback budget expired; no cancellation claimed");
        require(state->asyncFailure.empty(), state->asyncFailure.c_str());
        require(state->callbacks == 1 && state->callbackBodyFinished, "callback count/body completion mismatch");
        original = state->original;
    }
    if (controlled) {
        bridge->requestDestroy();
        require(!bridge->retired() && state->liveOwners == 1, "destroy request canceled the controlled pending notification");
        result[@"destroy_requested_before_controlled_publication"] = @YES;
        bridge->callback(original);
    }
    Payload published;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        result[@"callbacks"] = @(state->callbacks);
        result[@"notifications"] = @(state->notifications);
        result[@"retirements"] = @(state->retirements);
        result[@"notification_before_launch_exit"] = @(state->beforeLaunchExit);
        result[@"retirement_after_launch_exit"] = @(state->retiredAfterLaunch);
        require(state->notifications == 1 && state->retirements == 1 && state->retiredAfterLaunch,
                "single notification/deferred retirement failed");
        published = state->published;
    }
    require(bridge->retired() && state->liveOwners == 0, "fixture owner still alive after retirement");
    result[@"tracked_owner_retired"] = @YES;
    result[@"callback_pool_drained_before_observation"] = @YES;
    result[@"library_created"] = @(published.library.get() != nil);
    result[@"library_error"] = errorInfo(published.error.get());
    require(original.library.get() == published.library.get() && original.error.get() == published.error.get(),
            "original callback objects were replaced during publication");
    result[@"original_callback_objects_preserved"] = @YES;
    if (warning) {
        NSError* diagnostic = published.error.get();
        const bool markerPresent = diagnostic &&
            ([[diagnostic description] containsString:WarningMarker] ||
             [[diagnostic localizedDescription] containsString:WarningMarker] ||
             [[[diagnostic userInfo] description] containsString:WarningMarker]);
        result[@"warning_marker_present"] = @(markerPresent);
        require(published.library.get() != nil && diagnostic != nil && markerPresent,
                "expected nonnil library plus marked real NSError warning was not observed");
    }
    if (invalid) {
        require(published.library.get() == nil && published.error.get() != nil,
                "invalid authored source was not rejected with NSError");
    } else {
        require(published.library.get() != nil, "valid authored source returned no library");
        // A nonnil library may also carry a warning; keep it without converting
        // that warning into a nil result or silently discarding the NSError.
        readback(device, published.library.get(), bias, result);
    }
    result[@"status"] = @"passed";
}

static void guardedCase(id<MTLDevice> device, NSString* name, unsigned bias, LaunchBarrier* barrier, NSMutableDictionary* result) {
    @autoreleasepool {
        @try {
            try { runCase(device, name, bias, barrier, result); }
            catch (const std::exception& error) {
                result[@"status"] = @"failed";
                result[@"reason"] = [NSString stringWithUTF8String:error.what()];
                if (barrier) barrier->cancel();
            } catch (...) {
                result[@"status"] = @"failed";
                result[@"reason"] = @"unknown C++ exception";
                if (barrier) barrier->cancel();
            }
        } @catch (NSException* error) {
            result[@"status"] = @"failed";
            result[@"reason"] = [error description];
            if (barrier) barrier->cancel();
        }
    }
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        const char* reportArgument = nullptr;
        bool parallel = false, warningSource = false;
        for (int index = 1; index < argc; ++index) {
            if (std::string(argv[index]) == "--report" && index + 1 < argc && !reportArgument) reportArgument = argv[++index];
            else if (std::string(argv[index]) == "--parallel-cases" && !parallel) parallel = true;
            else if (std::string(argv[index]) == "--warning-source" && !warningSource) warningSource = true;
            else { std::fprintf(stderr, "invalid argument\n"); return 2; }
        }
        if (!reportArgument) { std::fprintf(stderr, "usage: mrc-metal --report NEW.json [--parallel-cases] [--warning-source]\n"); return 2; }
        struct stat existing {};
        if (lstat(reportArgument, &existing) == 0) return 2;
        if (errno != ENOENT) { std::perror("report path check"); return 2; }
        NSString* path = [NSString stringWithUTF8String:reportArgument];
        if (!path) return 2;
        NSMutableDictionary* report = [NSMutableDictionary dictionaryWithDictionary:@{
            @"schema": @1, @"scope": @"authored-legacy-metal-mrc-owner-retirement", @"status": @"running",
            @"process_id": @(getpid()), @"manual_reference_counting": @YES,
            @"execution_mode": parallel ? @"parallel" : @"sequential", @"warning_source_requested": @(warningSource),
            @"internal_compiler_parallelism_verified": @NO,
            @"legacy_async_library_verified": @NO, @"exact_compute_readback_verified": @NO,
            @"metal4_task_semantics_verified": @NO, @"real_compile_timeout_verified": @NO,
            @"compiler_cancellation_verified": @NO, @"production_options_parity_verified": @NO,
            @"production_adapter_verified": @NO, @"physical_iphone_verified": @NO,
            @"game_rendering_verified": @NO, @"performance_improvement_verified": @NO,
            @"control_scope": @"Controlled publication follows actual completion; it is not a compiler timeout or cancellation.",
            @"ownership_scope": @"Only the authored Owner counter and retained callback payload; not Metal-wide release/leak proof.",
            @"options": @{ @"language": @"2.4", @"math": @"safe/precise (legacy fastMath=false fallback)",
                           @"preserve_invariance": @NO, @"optimization": @"default", @"macros_recorded_per_case": @YES } }];
        NSMutableArray* cases = [NSMutableArray array];
        report[@"cases"] = cases;
        @try {
            try {
                auto device = Held<id<MTLDevice>>::adopt(MTLCreateSystemDefaultDevice());
                if (!device.get()) {
                    report[@"status"] = @"unsupported";
                    report[@"reason"] = @"no_default_metal_device";
                } else {
                    report[@"device"] = @{ @"name": [device.get() name] ?: @"" };
                    NSMutableArray* names = [NSMutableArray arrayWithArray:@[ @"success", @"invalid_source", @"controlled_publication" ]];
                    if (warningSource) [names addObject:@"warning_source"];
                    for (NSString* name in names) {
                        const unsigned bias = parallel && [name isEqualToString:@"controlled_publication"] ? 13u :
                            parallel && [name isEqualToString:@"warning_source"] ? 23u : 3u;
                        NSMutableDictionary* result = [NSMutableDictionary dictionaryWithDictionary:@{
                            @"case": name, @"status": @"running", @"library_compile_attempts": @0,
                            @"bias": @(bias), @"macros": @{ @"AUTHORED_BIAS": [NSString stringWithFormat:@"%uu", bias],
                                @"AUTHORED_WARNING": [name isEqualToString:@"warning_source"] ? @"1" : @"0" },
                            @"exact_compute_readback_verified": @NO,
                            @"controlled_publication": @([name isEqualToString:@"controlled_publication"]),
                            @"destroy_requested_before_controlled_publication": @NO }];
                        [cases addObject:result];
                    }
                    if (parallel) {
                        LaunchBarrier barrier(static_cast<unsigned>([cases count]));
                        std::vector<std::thread> threads;
                        threads.reserve([cases count]);
                        try {
                            for (NSMutableDictionary* result in cases) {
                                NSString* name = result[@"case"];
                                const unsigned bias = [result[@"bias"] unsignedIntValue];
                                threads.emplace_back([&, result, name, bias] { guardedCase(device.get(), name, bias, &barrier, result); });
                            }
                        } catch (...) {
                            barrier.cancel();
                            for (auto& thread : threads) if (thread.joinable()) thread.join();
                            throw;
                        }
                        for (auto& thread : threads) thread.join();
                    } else {
                        for (NSMutableDictionary* result in cases) {
                            guardedCase(device.get(), result[@"case"], [result[@"bias"] unsignedIntValue], nullptr, result);
                            if (![result[@"status"] isEqualToString:@"passed"]) break;
                        }
                    }
                    for (NSDictionary* result in cases) require([result[@"status"] isEqualToString:@"passed"],
                        "one or more authored cases failed; inspect individual case diagnostics");
                    report[@"status"] = @"passed";
                    report[@"legacy_async_library_verified"] = @YES;
                    report[@"exact_compute_readback_verified"] = @YES;
                }
            } catch (const std::exception& error) {
                report[@"status"] = @"failed";
                report[@"reason"] = [NSString stringWithUTF8String:error.what()];
            }
        } @catch (NSException* error) {
            report[@"status"] = @"failed";
            report[@"reason"] = [error description];
        }
        NSError* error = nil;
        NSData* data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
        if (!data || ![data writeToFile:path options:NSDataWritingAtomic error:&error]) return 1;
        return [report[@"status"] isEqualToString:@"passed"] ? 0 : [report[@"status"] isEqualToString:@"unsupported"] ? 78 : 1;
    }
}
