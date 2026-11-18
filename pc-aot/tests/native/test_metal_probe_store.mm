#import <Foundation/Foundation.h>
#import "ProbeRunStore.h"
#include <cstdio>
#include <chrono>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

static NSData *bytes(NSURL *url) { return [NSData dataWithContentsOfURL:url]; }

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        try {
            check(argc == 2, "fresh output root argument required");
            NSURL *root = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES];
            check(![NSFileManager.defaultManager fileExistsAtPath:root.path], "output root already exists");
            NSError *error = nil;
            ProbeRunStore *store = [[ProbeRunStore alloc] initWithRootURL:root error:&error];
            check(store != nil, "store initialization");
            NSURL *first = [store beginRunWithMetal4First:NO error:&error];
            check(first != nil, "first run");
            NSDictionary *record = [store readRecordAtURL:first error:&error];
            check([record[@"status"] isEqual:@"running"] && ![record[@"finished"] boolValue], "active run state");

            NSMutableArray *samples = [NSMutableArray arrayWithObject:@{@"library_ns": @123}];
            NSMutableDictionary *snapshot = [@{@"status": @"running", @"samples": samples} mutableCopy];
            uint64_t measured = UINT64_MAX;
            auto began = std::chrono::steady_clock::now();
            check([store writeSnapshot:snapshot toURL:first finished:NO elapsedNanoseconds:&measured error:&error], "timed progress save");
            auto outerNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count();
            check(measured != UINT64_MAX && measured <= static_cast<uint64_t>(outerNs), "progress duration is not inside outer wall interval");
            [samples addObject:@{@"library_ns": @999}];
            record = [store readRecordAtURL:first error:&error];
            check([record[@"comparison"][@"samples"] count] == 1, "mutable caller data was retained");

            NSData *initialBytes = bytes(first);
            ProbeRunStore *reopened = [[ProbeRunStore alloc] initWithRootURL:root error:&error];
            check([reopened.latestReportURL isEqual:first], "latest discovery");
            record = [reopened readRecordAtURL:first error:&error];
            check([record[@"status"] isEqual:@"interrupted"] && [record[@"recovered_incomplete"] boolValue], "unfinished run recovery");
            check([initialBytes isEqual:bytes(first)], "recovery rewrote original evidence");
            check(![reopened writeSnapshot:@{@"status": @"passed"} toURL:first finished:YES error:&error], "reopened run became writable");

            NSDictionary *invalid = @{@"status": @"failed", @"invalid": [NSSet setWithObject:@1]};
            measured = UINT64_MAX;
            began = std::chrono::steady_clock::now();
            check(![store writeSnapshot:invalid toURL:first finished:YES elapsedNanoseconds:&measured error:&error], "invalid JSON accepted");
            outerNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count();
            check(error != nil && measured != UINT64_MAX && measured <= static_cast<uint64_t>(outerNs), "rejected write duration missing or outside outer interval");
            check([initialBytes isEqual:bytes(first)], "invalid JSON damaged prior evidence");
            NSString *large = [@"x" stringByPaddingToLength:2 * 1024 * 1024 + 1 withString:@"x" startingAtIndex:0];
            check(![store writeSnapshot:@{@"status": @"failed", @"large": large} toURL:first finished:YES error:&error], "oversized record accepted");
            check([initialBytes isEqual:bytes(first)], "oversized record damaged prior evidence");
            check(![store writeSnapshot:@{@"status": @"running"} toURL:first finished:YES error:&error], "nonterminal result finished");
            check([initialBytes isEqual:bytes(first)], "invalid terminal status damaged evidence");

            check([store writeSnapshot:@{@"status": @"passed", @"samples": @[@1]} toURL:first finished:YES elapsedNanoseconds:nullptr error:&error], "unmeasured final save through timed entry");
            NSData *finalBytes = bytes(first);
            check(![store writeSnapshot:@{@"status": @"failed"} toURL:first finished:YES error:&error], "finished run overwritten");
            record = [store readRecordAtURL:first error:&error];
            check([record[@"status"] isEqual:@"passed"] && [record[@"finished"] boolValue], "terminal read");
            NSURL *second = [store beginRunWithMetal4First:YES error:&error];
            check(second != nil && ![second isEqual:first], "second run identity");
            check([finalBytes isEqual:bytes(first)], "older run changed");
            check([store.latestReportURL isEqual:second], "newest run tracking");

            NSURL *outside = [root URLByAppendingPathExtension:@"outside.json"];
            NSData *sentinel = [@"outside sentinel" dataUsingEncoding:NSUTF8StringEncoding];
            check([sentinel writeToURL:outside options:NSDataWritingWithoutOverwriting error:&error], "outside sentinel");
            check(![store readRecordAtURL:outside error:&error], "outside read accepted");
            check(![store writeSnapshot:@{@"status": @"failed"} toURL:outside finished:YES error:&error], "outside write accepted");
            check([sentinel isEqual:bytes(outside)], "outside sentinel changed");

            NSURL *preserved = [second URLByAppendingPathExtension:@"preserved"];
            check([NSFileManager.defaultManager moveItemAtURL:second toURL:preserved error:&error], "preserve file for link test");
            check(symlink(outside.path.fileSystemRepresentation, second.path.fileSystemRepresentation) == 0, "file link setup");
            check(![store readRecordAtURL:second error:&error], "file symlink read accepted");
            check(![store writeSnapshot:@{@"status": @"failed"} toURL:second finished:YES error:&error], "file symlink write accepted");
            check([sentinel isEqual:bytes(outside)], "file symlink changed target");

            NSURL *third = [store beginRunWithMetal4First:NO error:&error];
            check(third != nil, "third run");
            NSURL *directory = third.URLByDeletingLastPathComponent;
            NSURL *held = [directory URLByAppendingPathExtension:@"preserved"];
            check([NSFileManager.defaultManager moveItemAtURL:directory toURL:held error:&error], "preserve directory for link test");
            check(symlink(held.path.fileSystemRepresentation, directory.path.fileSystemRepresentation) == 0, "directory link setup");
            check(![store readRecordAtURL:third error:&error], "directory symlink read accepted");
            check(![store writeSnapshot:@{@"status": @"failed"} toURL:third finished:YES error:&error], "directory symlink write accepted");

            NSURL *rootLink = [root URLByAppendingPathExtension:@"link"];
            check(symlink(root.path.fileSystemRepresentation, rootLink.path.fileSystemRepresentation) == 0, "root link setup");
            check(![[ProbeRunStore alloc] initWithRootURL:rootLink error:&error], "root symlink accepted");

            ProbeRunStore *recovered = [[ProbeRunStore alloc] initWithRootURL:root error:&error];
            check(recovered != nil, "discovery failed on invalid linked evidence");
            check([recovered.latestReportURL isEqual:first], "unsafe linked evidence was discovered as latest");
            check([finalBytes isEqual:bytes(first)], "discovery damaged older run");

            // Synthetic dense data tests the actual Foundation encoding and
            // persisted size limit; it is not a Metal sample or GPU claim.
            NSURL *compactRoot = [root URLByAppendingPathComponent:@"schema4-controls" isDirectory:YES];
            ProbeRunStore *compactStore = [[ProbeRunStore alloc] initWithRootURL:compactRoot error:&error];
            check(compactStore != nil, "compact store initialization");
            NSURL *compactURL = [compactStore beginRunWithMetal4First:NO error:&error];
            check(compactURL != nil, "compact run");
            NSData *compactInitial = bytes(compactURL);
            NSMutableArray *values = [NSMutableArray arrayWithCapacity:192000];
            for (NSUInteger i = 0; i < 192000; ++i) [values addObject:@0];
            NSMutableDictionary *dense = [@{ @"schema": @3, @"status": @"running",
                @"synthetic": @{ @"rows": @{ @"values": values } } } mutableCopy];
            check(![compactStore writeSnapshot:dense toURL:compactURL finished:NO error:&error],
                  "legacy schema unexpectedly switched to compact encoding");
            check([compactInitial isEqual:bytes(compactURL)], "legacy oversized write damaged prior evidence");
            dense[@"schema"] = @4;
            check([compactStore writeSnapshot:dense toURL:compactURL finished:NO error:&error],
                  "schema4 compact record below byte limit was refused");
            NSDictionary *compactRecord = [compactStore readRecordAtURL:compactURL error:&error];
            check([compactRecord[@"comparison"] isEqual:dense], "compact encoding changed recorded content");
            NSData *compactBytes = bytes(compactURL);
            NSData *prettyBytes = [NSJSONSerialization dataWithJSONObject:compactRecord
                options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:&error];
            NSData *sortedCompact = [NSJSONSerialization dataWithJSONObject:compactRecord
                options:NSJSONWritingSortedKeys error:&error];
            check(prettyBytes.length > 2 * 1024 * 1024 && compactBytes.length <= 2 * 1024 * 1024,
                  "synthetic record did not straddle the formatting size limit");
            check([sortedCompact isEqual:compactBytes], "compact record changed sorted key serialization");

            check(![compactStore writeSnapshot:@{ @"schema": @4, @"status": @"failed", @"large": large }
                                       toURL:compactURL finished:YES error:&error], "oversized compact record accepted");
            check([compactBytes isEqual:bytes(compactURL)], "oversized compact write damaged prior evidence");
            dense[@"schema"] = @5;
            check([compactStore writeSnapshot:dense toURL:compactURL finished:NO error:&error],
                  "schema5 compact record below byte limit was refused");
            compactRecord = [compactStore readRecordAtURL:compactURL error:&error];
            compactBytes = bytes(compactURL);
            sortedCompact = [NSJSONSerialization dataWithJSONObject:compactRecord options:NSJSONWritingSortedKeys error:&error];
            check([compactRecord[@"comparison"] isEqual:dense] && [sortedCompact isEqual:compactBytes],
                  "schema5 changed content or compact sorted encoding");
            check(![compactStore writeSnapshot:@{ @"schema": @5, @"status": @"failed", @"large": large }
                                       toURL:compactURL finished:YES error:&error], "oversized schema5 record accepted");
            check([compactBytes isEqual:bytes(compactURL)], "oversized schema5 write damaged prior evidence");
            dense[@"status"] = @"passed";
            check([compactStore writeSnapshot:dense toURL:compactURL finished:YES error:&error], "compact terminal save");
            ProbeRunStore *compactReopened = [[ProbeRunStore alloc] initWithRootURL:compactRoot error:&error];
            NSDictionary *compactRestored = [compactReopened readRecordAtURL:compactURL error:&error];
            check([compactReopened.latestReportURL isEqual:compactURL] &&
                  [compactRestored[@"comparison"] isEqual:dense] && [compactRestored[@"finished"] boolValue],
                  "compact final record did not survive reopening");
            check([finalBytes isEqual:bytes(first)] && [recovered readRecordAtURL:first error:&error] != nil,
                  "compact save affected legacy record readability");
            std::puts("METAL_PROBE_STORE cases=14 foundation_verified=1 metal_api_verified=0 physical_iphone_verified=0");
            return 0;
        } catch (const std::exception &error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
