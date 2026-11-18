// Include the real MRC implementation to exercise internal ownership/readback
// helpers on a Mac. Public phone entry remains unsupported on this host.
#include "../../experiments/metal-compile/phone_contention.mm"

using namespace SkyrimContention;
static void check(bool value, const char* message) { require(value, message); }

static void inspectTrial(NSDictionary* trial, bool idle) {
    check([trial[@"status"] isEqual:@"passed"] && [trial[@"jobs"] count] == 3, "legacy trial incomplete");
    check([trial[@"planned_overlap"] boolValue] == !idle && [trial[@"actual_overlap_evaluated"] boolValue],
          "planned overlap was confused with measured overlap");
    check([trial[@"urgent_qos"] isEqual:@"user_initiated"] && [trial[@"prefetch_qos"] isEqual:@"user_initiated"],
          "neutral comparison changed requested QoS");
    check([trial[@"max_host_requests_inflight"] unsignedIntegerValue] <= 3, "unbounded host requests");
    if (idle) check([trial[@"prefetch_inflight_at_urgent_api_enter"] unsignedIntegerValue] == 0 &&
                    [trial[@"urgent_prefetch_overlap_ns"] unsignedLongLongValue] == 0,
                    "idle experiment overlapped urgent and prefetch");
    for (NSDictionary* job in trial[@"jobs"]) {
        check([job[@"status"] isEqual:@"passed"] && [job[@"oracle_verified"] boolValue], "GPU oracle not verified");
        check([job[@"compute_words"] count] == 64 && [job[@"callback_count"] unsignedIntegerValue] == 1,
              "readback/callback count mismatch");
        check([job[@"caller_temporary_pool_exited"] boolValue] && [job[@"callback_pool_exited"] boolValue] &&
              [job[@"owned_input_snapshots_verified"] boolValue], "MRC boundary was not exercised");
        check([job[@"observed_submission_thread_qos"] isKindOfClass:NSNumber.class],
              "actual submission thread QoS was not observed");
        check([job[@"adapter_input_reference_released_after_drain"] boolValue] &&
              ![job[@"metal4_task_joined"] boolValue] && ![job[@"metal4_task_adopted"] boolValue],
              "legacy lifetime test fabricated a Metal 4 task");
        const auto entered = [job[@"api_enter_ns"] unsignedLongLongValue];
        const auto returned = [job[@"api_return_ns"] unsignedLongLongValue];
        const auto callback = [job[@"callback_enter_ns"] unsignedLongLongValue];
        check(returned >= entered && callback >= entered &&
              [job[@"request_to_callback_ns"] unsignedLongLongValue] == callback - entered,
              "API/callback timestamps inconsistent");
    }
}

static NSArray* inspectSourcePanels(NSString* base) {
    // Frozen SHA-256 of ordered ASCII source hashes: all legacy sources first,
    // then each disjoint 96-source panel. This invokes the real sourceFor(),
    // covering formatting/salt drift as well as the portable integer oracle.
    NSArray* expected = @[@"7811f2498dbfcb761c80a5f530c2f398451142b81b1fa781a6002c1b0404b225",
                          @"40da8a562c60436107a1e2651131cd8dc2eaca2a7358bd59811cc15a8862319e",
                          @"de0a9f8ff8a26a60837622be1944528165cb6c100b4cd71102001b453de1c053"];
    constexpr unsigned first[] = {1, 9, 41}, last[] = {8, 40, 72};
    NSMutableSet* all = [NSMutableSet setWithCapacity:216];
    NSMutableArray* records = [NSMutableArray array];
    for (unsigned panel = 0; panel < 3; ++panel) {
        NSMutableString* ordered = [NSMutableString string];
        for (unsigned group = first[panel]; group <= last[panel]; ++group)
            for (unsigned role = 0; role < 3; ++role) {
                NSString* fingerprint = stringSHA(sourceFor(base, groupSalt(group, role)));
                check(![all containsObject:fingerprint], "source panels or legacy identities overlap");
                [all addObject:fingerprint]; [ordered appendString:fingerprint];
            }
        NSString* digest = stringSHA(ordered);
        check([digest isEqual:expected[panel]], "frozen complete source identity digest changed");
        [records addObject:@{ @"first_group": @(first[panel]), @"last_group": @(last[panel]),
            @"count": @((last[panel] - first[panel] + 1) * 3), @"ordered_hashes_sha256": digest }];
    }
    check(all.count == 216, "combined source identity count mismatch");
    for (bool alternate : {false, true}) {
        NSMutableDictionary* verified = [NSMutableDictionary dictionary];
        verifyPanelIdentities(base, alternate, verified);
        check([verified[@"planned_distinct_normal_sources_verified"] boolValue], "actual panel preflight failed");
    }
    return records;
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        @try {
            try {
                check(argc == 3, "usage: contention-controls phone_fixture.metal report.json");
                NSDictionary* mainResult = SkyrimMetalRunPhoneContention(@"missing-fixture", NO, nil);
                check([mainResult[@"status"] isEqual:@"failed"] && [mainResult[@"failure"] containsString:@"main thread"],
                      "main-thread call was accepted");
                __block NSDictionary* completedReport = nil;
                __block NSString* workerFailure = nil;
                dispatch_queue_t worker = dispatch_queue_create("authored.contention.native-test", DISPATCH_QUEUE_SERIAL);
                dispatch_semaphore_t done = dispatch_semaphore_create(0);
                NSString* path = [NSString stringWithUTF8String:argv[1]];
                dispatch_async(worker, ^{
                    @autoreleasepool {
                        @try {
                            try {
                                check(!NSThread.isMainThread, "test did not reach a worker");
                                NSDictionary* stopped = SkyrimMetalRunPhoneContention(@"missing-fixture", NO, ^BOOL { return NO; });
                                check([stopped[@"status"] isEqual:@"stopped"] && [stopped[@"trials"] count] == 0,
                                      "stop-before-work started requests");
                                NSDictionary* unsupported = SkyrimMetalRunPhoneContention(@"missing-fixture", YES, nil);
                                check([unsupported[@"status"] isEqual:@"unsupported"] &&
                                      [unsupported[@"unsupported_reason"] isEqual:@"physical_iphone_required"],
                                      "Mac boundary claimed physical support");
                                check([unsupported[@"schema"] unsignedIntegerValue] == 2 &&
                                      [unsupported[@"source_group_first"] unsignedIntegerValue] == 41 &&
                                      [unsupported[@"source_group_last"] unsignedIntegerValue] == 72 &&
                                      ![unsupported[@"all_96_normal_source_identities_distinct"] boolValue],
                                      "unsupported panel metadata fabricated completed identities");
                                for (NSDictionary* record in @[stopped, unsupported]) {
                                    check(![record[@"physical_iphone_verified"] boolValue] &&
                                          ![record[@"mrc_async_lifetime_verified"] boolValue], "boundary fabricated device validation");
                                    check([NSJSONSerialization isValidJSONObject:record], "boundary report is invalid JSON");
                                }
                                auto device = Held<id<MTLDevice>>::adopt(MTLCreateSystemDefaultDevice());
                                check(device.get() != nil, "Mac test requires a real Metal device");
                                auto queue = Held<id<MTLCommandQueue>>::adopt([device.get() newCommandQueue]);
                                auto base = loadBase(path);
                                NSArray* sourcePanels = inspectSourcePanels(base.get());
                                const auto input = makeInput();
                                NSMutableArray* trials = [NSMutableArray array];
                                NSMutableArray* errors = [NSMutableArray array];
                                const auto epoch = Clock::now();
                                NSString* panel = [NSString stringWithUTF8String:panelNamespace(false)];
                                auto idle = runTrial(device.get(), nil, queue.get(), base.get(), input, plan(0, 0, false), panel, true, epoch, trials, nil);
                                auto contended = runTrial(device.get(), nil, queue.get(), base.get(), input, plan(0, 1, false), panel, true, epoch, trials, nil);
                                inspectTrial(idle, true); inspectTrial(contended, false);
                                check([idle[@"source_group"] unsignedIntegerValue] == 9 &&
                                      [contended[@"source_group"] unsignedIntegerValue] == 10, "native trial assignment changed");
                                errorControl(device.get(), nil, base.get(), false, epoch, errors);
                                check([errors count] == 1 && [errors[0][@"expected_rejection_verified"] boolValue] &&
                                      [errors[0][@"callback_pool_exited"] boolValue], "retained compiler error control failed");
                                // A stop between requests drains the one already
                                // submitted, then leaves GPU validation explicit.
                                NSMutableArray* partial = [NSMutableArray array];
                                __block unsigned checks = 0;
                                bool caught = false;
                                try {
                                    (void)runTrial(device.get(), nil, queue.get(), base.get(), input, plan(0, 1, false), panel, false,
                                        epoch, partial, ^BOOL { return ++checks == 1; });
                                } catch (const Stopped&) { caught = true; }
                                NSDictionary* stoppedTrial = [partial firstObject];
                                check(caught && [stoppedTrial[@"status"] isEqual:@"stopped"] &&
                                      [stoppedTrial[@"submitted_requests"] unsignedIntegerValue] == 1 &&
                                      [stoppedTrial[@"drained_requests"] unsignedIntegerValue] == 1 &&
                                      [stoppedTrial[@"jobs"] count] == 1, "partial stop abandoned or added work");
                                check([stoppedTrial[@"jobs"][0][@"status"] isEqual:@"drained_not_gpu_validated"],
                                      "partial stop fabricated GPU success");
                                checks = 0; caught = false;
                                try {
                                    (void)runTrial(device.get(), nil, queue.get(), base.get(), input, plan(0, 1, false), panel, false,
                                        epoch, partial, ^BOOL { return ++checks <= 3; });
                                } catch (const Stopped&) { caught = true; }
                                NSDictionary* stopAfterCompiles = [partial lastObject];
                                check(caught && [stopAfterCompiles[@"drained_requests"] unsignedIntegerValue] == 3 &&
                                      [stopAfterCompiles[@"jobs"] count] == 3, "stop after compile lost completed drains");
                                for (NSDictionary* job in stopAfterCompiles[@"jobs"])
                                    check([job[@"readback_skipped_due_to_stop"] boolValue] && job[@"compute_words"] == nil,
                                          "stop before readback submitted new GPU work");
                                // Local policy control, not a claim that a real
                                // framework task threw. No Metal call is made by
                                // these unsubmitted owners and no global session
                                // state is poisoned by the unit test.
                                SessionSafety safety;
                                bool refusedBusy = false, refusedDisabled = false;
                                {
                                    RunLease active(safety);
                                    try { RunLease other(safety); }
                                    catch (const std::runtime_error&) { refusedBusy = true; }
                                }
                                std::array<std::shared_ptr<Request>, 3> owners;
                                for (unsigned i = 0; i < owners.size(); ++i) {
                                    owners[i] = std::make_shared<Request>(nil, nil, sourceFor(base.get(), groupSalt(1, i)), false, epoch);
                                    retainFailedJoin(safety, owners[i]);
                                    retainFailedJoin(safety, owners[i]);
                                }
                                try { RunLease disabled(safety); }
                                catch (const std::runtime_error&) { refusedDisabled = true; }
                                check(refusedBusy && refusedDisabled && safety.pendingCount == 3 && safety.disabled && !safety.active,
                                      "exceptional join policy grew or admitted another run");
                                completedReport = [@{ @"status": @"passed", @"scope": @"mac_legacy_mrc_contention_controls",
                                    @"boundary_controls": @3, @"legacy_readbacks": @6, @"compiler_errors": @1, @"stop_after_submit_controls": @2,
                                    @"bounded_retirement_policy_control": @YES,
                                    @"source_identity_panels": sourcePanels,
                                    @"physical_iphone_verified": @NO, @"metal4_task_lifetime_verified": @NO,
                                    @"mac_legacy_mrc_verified": @YES, @"device_name": device.get().name ?: @"",
                                    @"base_source_sha256": BaseSHA, @"input_sha256": sha(input.data(), sizeof(input)),
                                    @"options": optionRecord(), @"options_sha256": optionSHA(),
                                    @"trials": trials, @"error_controls": errors, @"partial_stop": partial } retain];
                            } catch (const std::exception& e) {
                                workerFailure = [[NSString stringWithUTF8String:e.what()] retain];
                            } catch (...) { workerFailure = [@"unknown native worker exception" retain]; }
                        } @catch (NSException* e) { workerFailure = [e.name retain]; }
                    }
                    dispatch_semaphore_signal(done);
                });
                // Test-runner timeout can kill this test process. The real
                // adapter itself never returns a fabricated task timeout.
                dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
                dispatch_release(done); dispatch_release(worker);
                check(workerFailure == nil, workerFailure.UTF8String ?: "worker failed");
                check(completedReport != nil, "test produced no report");
                NSError* error = nil;
                NSData* data = [NSJSONSerialization dataWithJSONObject:completedReport options:NSJSONWritingPrettyPrinted error:&error];
                check(data && [data writeToFile:[NSString stringWithUTF8String:argv[2]] options:NSDataWritingAtomic error:&error],
                      "native evidence could not be written");
                [completedReport release]; [workerFailure release];
                std::puts("PHONE_CONTENTION_MAC_MRC boundaries=3 readbacks=6 compiler_errors=1 stop_after_submit=2 physical_iphone_verified=0 metal4_task_verified=0");
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
