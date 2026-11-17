#import <Foundation/Foundation.h>
#import "ProbeRunStore.h"
#include <cstdio>
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
            check([store writeSnapshot:snapshot toURL:first finished:NO error:&error], "progress save");
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
            check(![store writeSnapshot:invalid toURL:first finished:YES error:&error], "invalid JSON accepted");
            check([initialBytes isEqual:bytes(first)], "invalid JSON damaged prior evidence");
            NSString *large = [@"x" stringByPaddingToLength:2 * 1024 * 1024 + 1 withString:@"x" startingAtIndex:0];
            check(![store writeSnapshot:@{@"status": @"failed", @"large": large} toURL:first finished:YES error:&error], "oversized record accepted");
            check([initialBytes isEqual:bytes(first)], "oversized record damaged prior evidence");
            check(![store writeSnapshot:@{@"status": @"running"} toURL:first finished:YES error:&error], "nonterminal result finished");
            check([initialBytes isEqual:bytes(first)], "invalid terminal status damaged evidence");

            check([store writeSnapshot:@{@"status": @"passed", @"samples": @[@1]} toURL:first finished:YES error:&error], "final save");
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
            std::puts("METAL_PROBE_STORE cases=10 foundation_verified=1 metal_api_verified=0 physical_iphone_verified=0");
            return 0;
        } catch (const std::exception &error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
