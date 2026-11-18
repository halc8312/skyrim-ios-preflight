// Native legacy GPU control for the actual phone workload helpers. A Mac pass
// establishes neither Metal 4 execution nor physical iPhone execution.
#define SKYRIM_METAL_RUNTIME_NO_MAIN 1
#include "runtime.mm"

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSMutableDictionary* report = [@{ @"status": @"failed", @"scope": @"authored-mac-legacy-workloads-control",
            @"physical_iphone_verified": @NO, @"metal4_library_verified": @NO,
            @"game_rendering_verified": @NO, @"performance_improvement_verified": @NO,
            @"samples": [NSMutableArray new] } mutableCopy];
        int result = 1;
        @try {
            try {
                require(argc == 3, "source and report paths required");
                NSError* error = nil;
                NSData* data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]
                                                   options:0 error:&error];
                requireObject(data, error, "workload source");
                NSString* source = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
                requireObject(source, nil, "workload source encoding");
                NSString* hash = phoneSourceHash(data);
                require([hash isEqualToString:@"26ed56a8f4d264965eb624911d42cbf925f1329f0f103d58f683fe1c52d07ad6"],
                        "authored workload source hash changed");
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                requireObject(device, nil, "actual legacy Metal device");
                id<MTLCommandQueue> queue = [device newCommandQueue];
                requireObject(queue, nil, "legacy Metal queue");
                report[@"device"] = deviceInfo(device);
                NSMutableArray* samples = report[@"samples"];
                for (unsigned index = 0; index < SkyrimPhoneWorkloads::CaseCount; ++index) {
                    @autoreleasepool {
                        error = nil;
                        id<MTLLibrary> library = sourceLibrary(device, nil, source, false, &error, phoneCompileOptions(index));
                        requireObject(library, error, "legacy authored workload library");
                        require(error == nil, "authored workload emitted a compiler diagnostic");
                        for (unsigned variant = 0; variant < 2; ++variant) {
                            auto input = SkyrimPhoneWorkloads::makeInput(index);
                            if (variant) for (auto& word : input) word ^= 0x01010101u;
                            NSMutableDictionary* sample = [phoneWorkloadInfo(index, hash) mutableCopy];
                            sample[@"input_variant"] = @(variant);
                            sample[@"input_sha256"] = phoneSourceHash(phoneWordData(input));
                            sample[@"render_oracle_sha256"] = phoneSourceHash(phoneWordData(SkyrimPhoneWorkloads::oracle(index, input, SkyrimPhoneWorkloads::RenderTag)));
                            sample[@"compute_oracle_sha256"] = phoneSourceHash(phoneWordData(SkyrimPhoneWorkloads::oracle(index, input, SkyrimPhoneWorkloads::ComputeTag)));
                            sample[@"status"] = @"running";
                            [samples addObject:sample];
                            runPhoneWorkloadGPU(device, queue, library, index, input, sample);
                            sample[@"status"] = @"passed";
                        }
                    }
                }
                report[@"status"] = @"passed";
                report[@"mac_legacy_gpu_verified"] = @YES;
                std::puts("PHONE_WORKLOAD_MAC_LEGACY families=3 cases=12 input_variants=2 readbacks=24 physical_iphone_verified=0 metal4_verified=0");
                result = 0;
            } catch (const std::exception& e) {
                report[@"failure"] = [NSString stringWithUTF8String:e.what()];
            }
        } @catch (NSException* exception) {
            report[@"failure"] = exception.reason ?: exception.name;
        }
        if (result) std::fprintf(stderr, "%s\n", [report[@"failure"] UTF8String] ?: "native workload control failed");
        if (argc == 3) {
            NSError* error = nil;
            NSData* json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
            if (!json || ![json writeToFile:[NSString stringWithUTF8String:argv[2]] options:NSDataWritingAtomic error:&error])
                return 1;
        }
        return result;
    }
}
