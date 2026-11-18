// Host-entry boundary controls, not iPhone or Metal 4 GPU evidence.
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import "runtime_host.h"
#include <cstdio>
#include <stdexcept>

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static void unverified(NSDictionary* report) {
    for (NSString* key in @[@"gpu_execution_verified", @"metal4_library_verified",
            @"metal4_async_adapter_verified", @"physical_iphone_verified",
            @"full_metal4_verified", @"game_rendering_verified",
            @"performance_improvement_verified", @"cold_os_cache_verified"])
        check([report[key] isEqual:@NO], "non-device control promoted verified evidence");
    check([NSJSONSerialization isValidJSONObject:report], "report is not JSON serializable");
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
                check([unsupported[@"schema"] unsignedIntegerValue] == 3 &&
                      [unsupported[@"total_cases"] unsignedIntegerValue] == 12 &&
                      [unsupported[@"total_samples"] unsignedIntegerValue] == 192,
                      "workload comparison totals/schema changed");
                check([callbackFailed[@"status"] isEqual:@"failed"] && failedProgress == 1 &&
                      [callbackFailed[@"failure"] containsString:@"controlled progress"],
                      "callback failure was swallowed or escaped");
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
                std::puts("METAL_PHONE_HOST_CONTROLS cases=5 gpu_execution_verified=0 physical_iphone_verified=0");
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
