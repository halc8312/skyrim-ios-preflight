// Host-entry boundary controls, not iPhone or Metal 4 GPU evidence.
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#define SKYRIM_METAL_RUNTIME_NO_MAIN 1
#include "runtime.mm"
#include <cstdio>
#include <stdexcept>

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static void unverified(NSDictionary* report) {
    for (NSString* key in @[@"gpu_execution_verified", @"metal4_library_verified",
            @"metal4_async_adapter_verified", @"physical_iphone_verified",
            @"full_metal4_verified", @"game_rendering_verified",
            @"performance_improvement_verified", @"cold_os_cache_verified",
            @"serial_comparison_verified", @"contention_control_verified",
            @"parallel_compilation_verified", @"mrc_lifetime_verified"])
        check([report[key] isEqual:@NO], "non-device control promoted verified evidence");
    check([NSJSONSerialization isValidJSONObject:report], "report is not JSON serializable");
}

static NSMutableDictionary* progressReport() {
    return [@{ @"status": @"running", @"completed_samples": @0,
        @"progress_timing": phoneProgressTiming(), @"samples": [NSMutableArray new] } mutableCopy];
}

static void checkpointControls() {
    // These synthetic records exercise actual checkpoint policy/guards only;
    // they contain no Metal results and establish no GPU execution evidence.
    NSMutableDictionary* report = progressReport();
    __block NSUInteger calls = 0, guards = 0, savedSamples = 0;
    NSUInteger largestSuffix = 0;
    BOOL (^keepGoing)(void) = ^BOOL { ++guards; return YES; };
    void (^save)(NSDictionary*) = ^(NSDictionary* snapshot) {
        ++calls;
        savedSamples = [snapshot[@"completed_samples"] unsignedIntegerValue];
    };
    for (NSString* phase in @[@"device", @"source", @"compiler"])
        phoneCheckpoint(report, phase, keepGoing, save);
    NSMutableArray* samples = report[@"samples"];
    for (NSUInteger caseIndex = 0; caseIndex < 12; ++caseIndex) {
        phoneCheckpoint(report, @"case_start", keepGoing, save);
        for (NSUInteger sampleIndex = 0; sampleIndex < 16; ++sampleIndex) {
            (void)sampleIndex;
            phoneCheckpoint(report, @"library", keepGoing, save);
            phoneCheckpoint(report, @"gpu_readback", keepGoing, save);
            const NSUInteger completed = samples.count + 1;
            [samples addObject:@{ @"synthetic_sample_index": @(completed) }];
            report[@"completed_samples"] = @(completed);
            if (completed - savedSamples > largestSuffix) largestSuffix = completed - savedSamples;
            phoneCheckpoint(report, @"sample_complete", keepGoing, save);
        }
    }
    for (NSUInteger i = 0; i < 4; ++i) phoneCheckpoint(report, @"invalid_source", keepGoing, save);
    for (NSUInteger i = 0; i < 2; ++i) {
        phoneCheckpoint(report, @"retained_success", keepGoing, save);
        phoneCheckpoint(report, @"retained_error", keepGoing, save);
    }
    for (NSString* phase in @[@"contention_control_start", @"contention_control_complete"])
        phoneCheckpoint(report, phase, keepGoing, save);
    NSDictionary* timing = report[@"progress_timing"];
    check(calls == 73 && guards == 1202 && largestSuffix == 4 && savedSamples == 192,
          "coalesced callbacks/guards/recoverable suffix mismatch");
    check([timing[@"considered_count"] unsignedIntegerValue] == 601 &&
          [timing[@"completed_count"] unsignedIntegerValue] == 73 &&
          [timing[@"skipped_count"] unsignedIntegerValue] == 528 &&
          [timing[@"incomplete_count"] unsignedIntegerValue] == 0,
          "coalesced counter partition mismatch");
    check([phoneSnapshot(report)[@"samples"] count] == 192, "coalescing discarded terminal samples");
    for (NSString* key in @[@"snapshot_serialization", @"synchronous_callback"])
        check([timing[key][@"count"] unsignedIntegerValue] == 73, "skipped boundary serialized or delivered a snapshot");

    NSMutableDictionary* skipped = progressReport();
    __block NSUInteger skippedGuards = 0, skippedCallbacks = 0;
    bool stopped = false;
    try {
        phoneCheckpoint(skipped, @"library", ^BOOL { return ++skippedGuards == 1; },
            ^(NSDictionary* snapshot) { (void)snapshot; ++skippedCallbacks; });
    } catch (const ProbeInterrupted&) { stopped = true; }
    check(stopped && skippedGuards == 2 && skippedCallbacks == 0 &&
          [skipped[@"progress_timing"][@"skipped_count"] unsignedIntegerValue] == 1,
          "skipped boundary lost its exit stop guard");
    NSMutableDictionary* noCallback = progressReport();
    phoneCheckpoint(noCallback, @"case_start", nil, nil);
    check([noCallback[@"progress_timing"][@"considered_count"] unsignedIntegerValue] == 1 &&
          [noCallback[@"progress_timing"][@"skipped_count"] unsignedIntegerValue] == 1,
          "nil callback counted as persisted progress");

    NSMutableDictionary* failedStore = progressReport();
    __block BOOL continuing = YES;
    stopped = false;
    try {
        phoneCheckpoint(failedStore, @"case_start", ^BOOL { return continuing; }, ^(NSDictionary* snapshot) {
            (void)snapshot;
            continuing = NO; // Same contract as host writeSnapshot failure.
        });
    } catch (const ProbeInterrupted&) { stopped = true; }
    check(stopped && [failedStore[@"progress_timing"][@"completed_count"] unsignedIntegerValue] == 1 &&
          [failedStore[@"progress_timing"][@"incomplete_count"] unsignedIntegerValue] == 0,
          "normal callback reporting store failure did not stop subsequent work");
}

static void resultDisplayControls() {
    // Explicitly authored formatting inputs only; these are not saved as
    // measurements and cannot establish iPhone/Metal execution evidence.
    NSMutableArray* trials = [NSMutableArray new];
    for (NSUInteger apiIndex = 0; apiIndex < 2; ++apiIndex) {
        for (NSUInteger modeIndex = 0; modeIndex < 2; ++modeIndex) {
            for (NSUInteger i = 0; i < 8; ++i) {
                const uint64_t duration = (modeIndex ? (i + 1) * (apiIndex ? 2 : 10) : i + 1 + apiIndex) * 1000000ULL;
                [trials addObject:@{ @"api": apiIndex ? @"metal4_async" : @"legacy_async",
                    @"mode": modeIndex ? @"contended" : @"idle", @"status": @"passed",
                    @"first_assigned_in_run": @YES,
                    @"host_overlap_observed": @(modeIndex && apiIndex == 0 && i >= 6),
                    @"urgent_request_to_callback_ns": @(duration),
                    @"urgent_enqueue_to_callback_ns": @999000000ULL }];
            }
        }
    }
    NSDictionary* control = @{ @"schema": @2, @"status": @"passed", @"trials": trials };
    NSString* text = SkyrimMetalPhoneContentionResultText(@{ @"schema": @5, @"contention_control": control });
    check([text containsString:@"従来方式\n単独 4.50 ms ／ 先読みあり 45.00 ms\n実際の重なり 2 / 8回"],
          "display excluded fast nonoverlapping observations or used wrong interval");
    check([text containsString:@"Metal 4\n単独 5.50 ms ／ 先読みあり 9.00 ms\n実際の重なり 0 / 8回（未観測）"],
          "zero overlap became a failure, missing planned median, or a zero duration");
    check(![text containsString:@"999"] && ![text containsString:@"75.00"],
          "display used enqueue interval or actual-only sample selection");

    NSArray* statuses = @[@"not_run", @"incomplete", @"stopped", @"unsupported", @"failed"];
    NSArray* labels = @[@"未実施", @"未完了", @"途中で停止", @"この環境では未対応", @"確認が必要な結果"];
    for (NSUInteger i = 0; i < statuses.count; ++i) {
        NSString* value = SkyrimMetalPhoneContentionResultText(@{ @"schema": @5,
            @"contention_control": @{ @"schema": @2, @"status": statuses[i], @"trials": trials } });
        check([value containsString:labels[i]] && ![value containsString:@" ms"],
              "incomplete/unavailable result was formatted as a completed comparison");
    }
    for (id invalidDuration in @[@YES, @(-1), @"100", NSNull.null]) {
        NSMutableArray* malformed = [trials mutableCopy];
        NSMutableDictionary* invalid = [malformed[0] mutableCopy];
        invalid[@"urgent_request_to_callback_ns"] = invalidDuration;
        malformed[0] = invalid;
        NSString* value = SkyrimMetalPhoneContentionResultText(@{ @"schema": @5,
            @"contention_control": @{ @"schema": @2, @"status": @"passed", @"trials": malformed } });
        check([value containsString:@"記録の確認が必要"] && ![value containsString:@" ms"],
              "invalid duration was displayed as measured time");
    }
    NSMutableArray* missing = [trials mutableCopy];
    [missing removeLastObject];
    check([SkyrimMetalPhoneContentionResultText(@{ @"schema": @5,
        @"contention_control": @{ @"schema": @2, @"status": @"passed", @"trials": missing } })
        containsString:@"記録の確認が必要"], "incomplete planned set was presented as eight observations");

    NSDictionary* old = @{ @"schema": @4, @"contention_control": @{ @"status": @"passed", @"trials": @[
        @{ @"status": @"passed", @"api": @"legacy_async", @"mode": @"contended",
           @"urgent_enqueue_to_callback_ns": @1000000, @"urgent_request_to_callback_ns": @50000000,
           @"prefetch_inflight_at_urgent_api_enter": @0 },
        @{ @"status": @"passed", @"api": @"legacy_async", @"mode": @"contended",
           @"urgent_enqueue_to_callback_ns": @5000000, @"urgent_request_to_callback_ns": @50000000,
           @"prefetch_inflight_at_urgent_api_enter": @1 }] } };
    NSString* oldText = SkyrimMetalPhoneContentionResultText(old);
    check([oldText containsString:@"以前の比較：最大値・開始待ちを含む"] &&
          [oldText containsString:@"従来　5.00 ms"] && ![oldText containsString:@"中央値"],
          "historical schema4 was reinterpreted using schema5 timing or statistic");
    for (NSNumber* schema in @[@1, @2, @3])
        check(SkyrimMetalPhoneContentionResultText(@{ @"schema": schema }) == nil,
              "historical serial-only result acquired scheduling measurements");
}

int main() {
    @autoreleasepool {
        @try {
            try {
                NSDictionary* main = SkyrimMetalRunPhoneComparison(@"missing-authored-fixture", 4, NO, nil, nil);
                check([main[@"status"] isEqual:@"failed"], "main-thread invocation did not fail");
                check([main[@"failure"] containsString:@"main thread"], "main-thread refusal not diagnosed");
                unverified(main);

                dispatch_queue_t queue = dispatch_queue_create("authored.phone-runtime.controls", DISPATCH_QUEUE_SERIAL);
                __block NSDictionary* invalid = nil;
                __block NSDictionary* stopped = nil;
                __block NSDictionary* unsupported = nil;
                __block NSDictionary* callbackFailed = nil;
                __block NSDictionary* firstSnapshot = nil;
                __block NSUInteger stoppedProgress = 0;
                __block NSUInteger unsupportedProgress = 0;
                __block NSUInteger failedProgress = 0;
                // Catch inside the block: exceptions must never unwind GCD frames.
                __block NSString* workerFailure = nil;
                __block BOOL workerWasMain = YES;
                dispatch_semaphore_t completed = dispatch_semaphore_create(0);
                // dispatch_sync may execute on its caller, including main.
                // This test must enter through a real asynchronous worker.
                dispatch_async(queue, ^{
                    @autoreleasepool {
                        workerWasMain = NSThread.isMainThread;
                        @try {
                            try {
                                invalid = SkyrimMetalRunPhoneComparison(@"missing-authored-fixture", 3, NO, nil, nil);
                                stopped = SkyrimMetalRunPhoneComparison(@"missing-authored-fixture", 4, NO,
                                    ^BOOL { return NO; }, ^(NSDictionary* snapshot) { (void)snapshot; ++stoppedProgress; });
                                unsupported = SkyrimMetalRunPhoneComparison(@"missing-authored-fixture", 4, YES,
                                    ^BOOL { return YES; }, ^(NSDictionary* snapshot) {
                                        ++unsupportedProgress;
                                        if (!firstSnapshot) firstSnapshot = snapshot;
                                    });
                                callbackFailed = SkyrimMetalRunPhoneComparison(@"missing-authored-fixture", 4, NO,
                                    ^BOOL { return YES; }, ^(NSDictionary* snapshot) {
                                        (void)snapshot;
                                        ++failedProgress;
                                        throw std::runtime_error("controlled progress callback failure");
                                    });
                            } catch (const std::exception& e) {
                                workerFailure = [NSString stringWithUTF8String:e.what()];
                            }
                        } @catch (NSException* e) {
                            workerFailure = e.name;
                        }
                    }
                    dispatch_semaphore_signal(completed);
                });
                check(dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) == 0,
                      "native worker did not complete within test deadline");
                check(!workerWasMain, "worker test ran on main thread");
                check(workerFailure == nil, "exception escaped callable runtime");
                check([invalid[@"status"] isEqual:@"failed"], "invalid repeat was accepted");
                check([invalid[@"failure"] containsString:@"repeat"], "invalid repeat not diagnosed");
                check([stopped[@"status"] isEqual:@"interrupted"], "stop became success/failure");
                check([stopped[@"stop_requested"] boolValue], "stop was not recorded");
                check(![stopped[@"cancellation_requested"] boolValue], "stop claimed Metal cancellation");
                check(stoppedProgress == 0 && [stopped[@"samples"] count] == 0,
                      "stop-before-work started a sample or progress callback");
                check(stopped[@"device"] == nil, "stop-before-work reached device acquisition");
                check([unsupported[@"status"] isEqual:@"unsupported"], "Mac host claimed physical iPhone support");
                check([unsupported[@"unsupported_reason"] isEqual:@"physical_iphone_required"],
                      "non-phone refusal lost its reason");
                check(unsupportedProgress == 1 && [firstSnapshot[@"phase"] isEqual:@"device"],
                      "initial progress phase/count mismatch");
                check(firstSnapshot[@"device"] == nil && unsupported[@"device"] != nil,
                      "progress snapshot changed after the callback");
                check([firstSnapshot[@"status"] isEqual:@"running"] &&
                      [unsupported[@"status"] isEqual:@"unsupported"], "snapshot status aliases final report");
                check([unsupported[@"schema"] unsignedIntegerValue] == 5 &&
                      [unsupported[@"total_cases"] unsignedIntegerValue] == 12 &&
                      [unsupported[@"total_samples"] unsignedIntegerValue] == 192,
                      "workload comparison totals/schema changed");
                check([main[@"contention_panel_requested"] isEqual:@"primary"] &&
                      [unsupported[@"contention_panel_requested"] isEqual:@"alternate"],
                      "selector no longer records its requested disjoint test panel");
                check([callbackFailed[@"status"] isEqual:@"failed"] && failedProgress == 1 &&
                      [callbackFailed[@"failure"] containsString:@"controlled progress"],
                      "callback failure was swallowed or escaped");
                check([unsupported[@"progress_timing"][@"considered_count"] unsignedIntegerValue] == 1 &&
                      [unsupported[@"progress_timing"][@"completed_count"] unsignedIntegerValue] == 1 &&
                      [unsupported[@"progress_timing"][@"skipped_count"] unsignedIntegerValue] == 0 &&
                      [unsupported[@"progress_timing"][@"incomplete_count"] unsignedIntegerValue] == 0,
                      "unsupported progress partition changed");
                check([callbackFailed[@"progress_timing"][@"incomplete_count"] unsignedIntegerValue] == 1,
                      "throwing callback lost its incomplete attempt");
                for (NSString* key in @[@"snapshot_serialization", @"synchronous_callback"]) {
                    NSDictionary* measured = unsupported[@"progress_timing"][key];
                    check([measured[@"count"] unsignedIntegerValue] == 1 &&
                          [measured[@"max_ns"] isEqual:measured[@"total_ns"]],
                          "completed progress interval count/max mismatch");
                    check([firstSnapshot[@"progress_timing"][key][@"count"] unsignedIntegerValue] == 0,
                          "published snapshot mutated with its own incomplete timing");
                    check([callbackFailed[@"progress_timing"][key][@"count"] unsignedIntegerValue] == 0,
                          "throwing callback counted as a completed progress call");
                    check([stopped[@"progress_timing"][key][@"count"] unsignedIntegerValue] == 0,
                          "stop-before-progress recorded a nonexistent interval");
                }
                for (NSDictionary* report in @[invalid, stopped, unsupported, firstSnapshot, callbackFailed]) unverified(report);
                checkpointControls();
                resultDisplayControls();
                std::puts("METAL_PHONE_HOST_CONTROLS cases=11 gpu_execution_verified=0 physical_iphone_verified=0");
                return 0;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "%s\n", e.what());
                return 1;
            }
        } @catch (NSException* e) {
            std::fprintf(stderr, "Objective-C exception: %s\n", e.name.UTF8String);
            return 1;
        }
    }
}
