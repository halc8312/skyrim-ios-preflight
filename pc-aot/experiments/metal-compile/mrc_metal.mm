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
    "kernel void probe(device uint4* output [[buffer(0)]], uint id [[thread_position_in_grid]]) {\n"
    "if (id < 4) output[id] = uint4(id + 3u, (id + 1u) * 7u, 10u, 0x12345678u); }\n";
static NSString* const InvalidSource = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void probe(device uint* output [[buffer(0)]]) { output[0] = definitely_missing_authored_identifier; }\n";
static NSString* const WarningMarker = @"authored_nonfatal_diagnostic";
static NSString* authoredSource(bool invalid, bool warning) {
    if (invalid) return InvalidSource;
    return warning ? [@"#warning authored_nonfatal_diagnostic\n" stringByAppendingString:SuccessSource] : SuccessSource;
}

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
    Owner(std::shared_ptr<State> stateIn, id<MTLDevice> deviceIn, NSString* sourceIn)
        : state(std::move(stateIn)), device(deviceIn), source(Held<NSString*>::adopt([sourceIn copy])),
          options(Held<MTLCompileOptions*>::adopt([MTLCompileOptions new])) {
        ++state->liveOwners;
        MTLCompileOptions* value = options.get();
        value.languageVersion = MTLLanguageVersion2_4;
        value.preserveInvariance = NO;
        value.optimizationLevel = MTLLibraryOptimizationLevelDefault;
        value.preprocessorMacros = @{};
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

static void readback(id<MTLDevice> device, id<MTLLibrary> library, NSMutableDictionary* result) {
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
        const uint32_t expected[4] = {index + 3u, (index + 1u) * 7u, 10u, 0x12345678u};
        for (uint32_t component = 0; component < 4; ++component) {
            [values addObject:@(actual[4 * index + component])];
            matches = matches && actual[4 * index + component] == expected[component];
        }
    }
    result[@"compute_words"] = values;
    require(matches, "authored compute readback mismatch");
    result[@"exact_compute_readback_verified"] = @YES;
}

static void runCase(id<MTLDevice> device, NSString* name, NSMutableDictionary* result) {
    const bool invalid = [name isEqualToString:@"invalid_source"];
    const bool controlled = [name isEqualToString:@"controlled_publication"];
    const bool warning = [name isEqualToString:@"warning_source"];
    auto state = std::make_shared<State>();
    std::shared_ptr<Bridge> bridge;
    Owner* raw = nullptr;
    CompletionHolder completion;
    @autoreleasepool {
        NSMutableString* callerSource = [NSMutableString stringWithString:authoredSource(invalid, warning)];
        auto owner = std::make_shared<Owner>(state, device, callerSource);
        raw = owner.get();
        bridge = Bridge::create(
            [state, raw, invalid, warning](Payload payload) {
                require(state->callerPoolExpired && [raw->source.get() isEqualToString:authoredSource(invalid, warning)],
                        "owned source snapshot changed or caller pool still active");
                require([raw->options.get() languageVersion] == MTLLanguageVersion2_4 &&
                        [[raw->options.get() preprocessorMacros] count] == 0, "owned options changed");
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
        readback(device, published.library.get(), result);
    }
    result[@"status"] = @"passed";
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 3 || std::string(argv[1]) != "--report") { std::fprintf(stderr, "usage: mrc-metal --report NEW.json\n"); return 2; }
        struct stat existing {};
        if (lstat(argv[2], &existing) == 0) return 2;
        if (errno != ENOENT) { std::perror("report path check"); return 2; }
        NSString* path = [NSString stringWithUTF8String:argv[2]];
        if (!path) return 2;
        NSMutableDictionary* report = [NSMutableDictionary dictionaryWithDictionary:@{
            @"schema": @1, @"scope": @"authored-legacy-metal-mrc-owner-retirement", @"status": @"running",
            @"process_id": @(getpid()), @"manual_reference_counting": @YES,
            @"legacy_async_library_verified": @NO, @"exact_compute_readback_verified": @NO,
            @"metal4_task_semantics_verified": @NO, @"real_compile_timeout_verified": @NO,
            @"compiler_cancellation_verified": @NO, @"production_options_parity_verified": @NO,
            @"production_adapter_verified": @NO, @"physical_iphone_verified": @NO,
            @"game_rendering_verified": @NO, @"performance_improvement_verified": @NO,
            @"control_scope": @"Controlled publication follows actual completion; it is not a compiler timeout or cancellation.",
            @"ownership_scope": @"Only the authored Owner counter and retained callback payload; not Metal-wide release/leak proof.",
            @"options": @{ @"language": @"2.4", @"math": @"safe/precise (legacy fastMath=false fallback)",
                           @"preserve_invariance": @NO, @"optimization": @"default", @"macros": @{} } }];
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
                    for (NSString* name in @[ @"success", @"invalid_source", @"controlled_publication", @"warning_source" ]) {
                        NSMutableDictionary* result = [NSMutableDictionary dictionaryWithDictionary:@{
                            @"case": name, @"status": @"running", @"library_compile_attempts": @0,
                            @"exact_compute_readback_verified": @NO,
                            @"controlled_publication": @([name isEqualToString:@"controlled_publication"]),
                            @"destroy_requested_before_controlled_publication": @NO }];
                        [cases addObject:result];
                        runCase(device.get(), name, result);
                    }
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
