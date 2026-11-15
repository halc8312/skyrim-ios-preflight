/* SPDX-License-Identifier: GPL-3.0-only
 * Authored logs only. Actual Foundation capture/sender and NSURLSession delegate;
 * transport is intercepted by NSURLProtocol, with no network connection. */
#import <Foundation/Foundation.h>
#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#import "../../ios/runtime-port/diagnostic_upload.h"

static NSDictionary *fixturePairing;
static NSLock *requestLock;
static NSMutableArray<NSDictionary *> *requests;
static atomic_int responseMode;

@interface FixtureDiagnosticProtocol : NSURLProtocol
@property(atomic) BOOL stopped;
- (void)reply:(NSData *)body mode:(int)mode;
@end

/* These are dependency substitutions, not copied sender implementations. The
 * production file below still owns parsing, requests, retry state and delegate. */
@interface NSBundle (DiagnosticFixture)
- (id)fixture_infoForKey:(NSString *)key;
@end
@implementation NSBundle (DiagnosticFixture)
- (id)fixture_infoForKey:(NSString *)key {
    if ([key isEqualToString:@"PcDiagnostics"]) return fixturePairing;
    if ([key isEqualToString:@"CFBundleVersion"]) return @"12";
    return [self objectForInfoDictionaryKey:key];
}
@end
@interface NSURLSessionConfiguration (DiagnosticFixture)
+ (NSURLSessionConfiguration *)fixture_ephemeralConfiguration;
@end
@implementation NSURLSessionConfiguration (DiagnosticFixture)
+ (NSURLSessionConfiguration *)fixture_ephemeralConfiguration {
    NSURLSessionConfiguration *configuration = [self ephemeralSessionConfiguration];
    configuration.protocolClasses = @[FixtureDiagnosticProtocol.class];
    return configuration;
}
@end
#define objectForInfoDictionaryKey fixture_infoForKey
#define ephemeralSessionConfiguration fixture_ephemeralConfiguration
#include "../../ios/runtime-port/diagnostic_upload.m"
#undef ephemeralSessionConfiguration
#undef objectForInfoDictionaryKey

enum {
    ReplyCreated, ReplyDuplicate, ReplyOffline, ReplyWrongHash, ReplyFractionalBytes,
    ReplyWrongType, ReplyLargeLength, ReplyLargeBody, ReplyForbidden, ReplyBadSchema,
    ReplyDelayed, ReplyBooleanSchema, ReplyMalformedJSON, ReplyExtraField
};
static NSData *requestBody(NSURLRequest *request) {
    if (request.HTTPBody) return request.HTTPBody;
    NSInputStream *stream = request.HTTPBodyStream;
    assert(stream);
    NSMutableData *data = [NSMutableData data];
    [stream open];
    uint8_t block[4096];
    NSInteger n;
    while ((n = [stream read:block maxLength:sizeof block]) > 0) {
        [data appendBytes:block length:(NSUInteger)n];
        assert(data.length <= SKYRIM_DIAGNOSTIC_MAX_BODY_BYTES);
    }
    assert(n == 0);
    [stream close];
    return data;
}
@implementation FixtureDiagnosticProtocol
+ (BOOL)canInitWithRequest:(NSURLRequest *)request {
    return [request.URL.scheme isEqualToString:@"https"];
}
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request { return request; }
- (void)startLoading {
    NSData *body = requestBody(self.request);
    assert(body.length);
    assert([self.request.HTTPMethod isEqualToString:@"POST"]);
    assert([self.request.URL.absoluteString isEqual:fixturePairing[@"endpoint"]]);
    assert([[self.request valueForHTTPHeaderField:@"Content-Type"] isEqualToString:@"application/json"]);
    assert([[self.request valueForHTTPHeaderField:@"Authorization"] isEqual:
        [@"Bearer " stringByAppendingString:fixturePairing[@"token"]]]);
    assert([[self.request valueForHTTPHeaderField:@"X-Content-SHA256"] isEqual:digest(body)]);
    NSDictionary *snapshot = @{@"body":[body copy], @"hash":digest(body)};
    [requestLock lock]; [requests addObject:snapshot]; [requestLock unlock];
    const int mode = atomic_load(&responseMode);
    if (mode == ReplyOffline) {
        [self.client URLProtocol:self didFailWithError:
            [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorNotConnectedToInternet userInfo:nil]];
        return;
    }
    if (mode == ReplyDelayed) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
            dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ [self reply:body mode:mode]; });
    } else [self reply:body mode:mode];
}
- (void)reply:(NSData *)body mode:(int)mode {
    if (self.stopped) return;
    NSDictionary *report = [NSJSONSerialization JSONObjectWithData:body options:0 error:NULL];
    assert([report isKindOfClass:NSDictionary.class]);
    NSMutableDictionary *receipt = [@{@"schema":@1, @"report_id":report[@"report_id"],
        @"sha256":digest(body), @"bytes":@(body.length)} mutableCopy];
    if (mode == ReplyWrongHash) receipt[@"sha256"] = [@"" stringByPaddingToLength:64 withString:@"0" startingAtIndex:0];
    if (mode == ReplyFractionalBytes) receipt[@"bytes"] = @((double)body.length + 0.5);
    if (mode == ReplyBadSchema) receipt[@"schema"] = @2;
    if (mode == ReplyBooleanSchema) receipt[@"schema"] = @YES;
    if (mode == ReplyExtraField) receipt[@"unexpected"] = @1;
    NSData *wire = [NSJSONSerialization dataWithJSONObject:receipt options:0 error:NULL];
    if (mode == ReplyLargeBody) wire = [[@"" stringByPaddingToLength:5000 withString:@"0" startingAtIndex:0] dataUsingEncoding:NSUTF8StringEncoding];
    if (mode == ReplyMalformedJSON) wire = [@"not-json" dataUsingEncoding:NSUTF8StringEncoding];
    NSString *mime = mode == ReplyWrongType ? @"text/plain" : @"application/json";
    NSMutableDictionary *headers = [@{@"Content-Type":mime} mutableCopy];
    if (mode == ReplyLargeLength) headers[@"Content-Length"] = @"5000";
    NSInteger code = mode == ReplyDuplicate ? 200 : mode == ReplyForbidden ? 403 : 201;
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:self.request.URL
        statusCode:code HTTPVersion:@"HTTP/1.1" headerFields:headers];
    [self.client URLProtocol:self didReceiveResponse:response cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    if (!self.stopped) [self.client URLProtocol:self didLoadData:wire];
    if (!self.stopped) [self.client URLProtocolDidFinishLoading:self];
}
- (void)stopLoading { self.stopped = YES; }
@end

static NSString *directory(NSString *root, NSString *name) {
    NSString *path = [root stringByAppendingPathComponent:name];
    assert([NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:NULL]);
    return path;
}
static void writeData(NSString *path, NSData *data) {
    assert([data writeToFile:path options:0 error:NULL]);
}
static void writeText(NSString *path, NSString *text) {
    writeData(path, [text dataUsingEncoding:NSUTF8StringEncoding]);
}
static NSString *header(NSUInteger build) {
    return [NSString stringWithFormat:@"SkyrimIOS PC2011 build=%lu; session gameplay requires observation\n", (unsigned long)build];
}
static NSString *simpleLog(NSUInteger build) {
    return [header(build) stringByAppendingString:@"IOS_DIAG_SETTING key=0 value=60\nIOS_SHADER_OPTION option=0 enabled=0\n"];
}
static void checkError(NSError *error) {
    assert(error && [error.domain isEqualToString:@"SkyrimDiagnostics"] && error.code == 1);
    assert(error.userInfo.count == 0);
}
static void pairingTests(void) {
    assert(skyrim_diagnostics_valid_pairing(fixturePairing));
    NSString *endpoint = fixturePairing[@"endpoint"], *token = fixturePairing[@"token"];
    NSArray *badEndpoints = @[
        @"http://fixture.tail0.ts.net/skyrim-diagnostics/v1/reports",
        @"https://fixture.example.invalid/skyrim-diagnostics/v1/reports",
        @"https://fixture.tail0.ts.net:443/skyrim-diagnostics/v1/reports",
        @"https://u:p@fixture.tail0.ts.net/skyrim-diagnostics/v1/reports",
        @"https://fixture.tail0.ts.net/skyrim-diagnostics/v1/reports?x=1",
        @"https://fixture.tail0.ts.net/skyrim-diagnostics/v1/reports#fragment",
        @"https://fixture.tail0.ts.net/skyrim-diagnostics/v1/reports/",
        @"https://fixture.tail0.ts.net/skyrim-diagnostics/v%31/reports",
        @"https://fixture.tail0.ts.net/skyrim-diagnostics/v1/other"
    ];
    for (NSString *url in badEndpoints) {
        NSDictionary *candidate = @{@"endpoint":url, @"token":token};
        assert(!skyrim_diagnostics_valid_pairing(candidate));
    }
    NSArray *badTokens = @[
        [@"" stringByPaddingToLength:42 withString:@"a" startingAtIndex:0],
        [@"" stringByPaddingToLength:129 withString:@"a" startingAtIndex:0], @"unsafe\nsecret", @42, NSNull.null
    ];
    for (id value in badTokens) {
        NSDictionary *candidate = @{@"endpoint":endpoint, @"token":value};
        assert(!skyrim_diagnostics_valid_pairing(candidate));
    }
    NSDictionary *extra = @{@"endpoint":endpoint, @"token":token, @"extra":@1};
    assert(!skyrim_diagnostics_valid_pairing(extra));
    assert(!skyrim_diagnostics_valid_pairing(nil));
    puts("pairing exact HTTPS endpoint and token PASS");
}
static void captureTests(NSString *root, NSDictionary *fixture) {
    NSString *folder = directory(root, @"capture");
    NSString *current = [folder stringByAppendingPathComponent:@"run-current.log"];
    NSString *previous = [folder stringByAppendingPathComponent:@"run-previous.log"];
    NSMutableString *raw = [simpleLog(12) mutableCopy];
    NSMutableArray *expected = [@[@"IOS_DIAG_SETTING key=0 value=60", @"IOS_SHADER_OPTION option=0 enabled=0"] mutableCopy];
    for (NSString *line in fixture[@"allowed"]) {
        [raw appendFormat:@"%@\n", line]; [expected addObject:line];
    }
    for (NSString *line in fixture[@"excluded"]) [raw appendFormat:@"%@\n", line];
#define CHECK_STATIC_ERROR(message, code) do { \
    [raw appendFormat:@"FATAL iOS startup: %s\n", message]; \
    [expected addObject:[NSString stringWithFormat:@"IOS_DIAG_ERROR code=%u", (unsigned)(code)]]; \
} while (0);
    SKYRIM_DIAG_STATIC_ERRORS(CHECK_STATIC_ERROR)
#undef CHECK_STATIC_ERROR
    [raw appendString:@"FATAL iOS startup: Unrecognized AUTHORED_PRIVATE_ERROR /private/data\n"];
    // Matches the broad numeric prefix grammar but exceeds the shared wire bound.
    [raw appendString:@"IOS_STALL schema=1 threshold_ns=50000000 count=1 dropped_busy=0 dropped_full=0 fields=kind,thread,start_ns,end_ns,failed,estimated_end spans="];
    [raw appendString:[@"" stringByPaddingToLength:9000 withString:@"1" startingAtIndex:0]];
    [raw appendString:@"\n"];
    NSMutableData *bytes = [[raw dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
    const uint8_t malformed[] = {0xff, 0xc3, 0x28, '\n'};
    [bytes appendBytes:malformed length:sizeof malformed];
    [bytes appendData:[@"IOS_SHADER_OPTION option=4 enabled=1" dataUsingEncoding:NSUTF8StringEncoding]]; // no LF
    writeData(current, bytes);
    writeText(previous, [header(8) stringByAppendingString:@"IOS_DIAG_SETTING key=0 value=30\nIOS_SHADER_OPTION option=1 enabled=1\n"]);
    NSString *ignored = [folder stringByAppendingPathComponent:@"run-symlink.log"];
    assert(symlink(previous.fileSystemRepresentation, ignored.fileSystemRepresentation) == 0);
    NSError *error = nil;
    NSDictionary *report = skyrim_diagnostics_capture(folder, current, 12, &error);
    assert(report && !error);
    NSArray *sessions = report[@"sessions"];
    assert(sessions.count == 2);
    NSDictionary *now = sessions[0], *last = sessions[1];
    assert([now[@"build"] isEqual:@12] && [last[@"build"] isEqual:@8]);
    assert([now[@"capture"] isEqual:@"live_prefix"] && [last[@"capture"] isEqual:@"closed"]);
    assert([now[@"slot"] isEqual:@"current"] && [last[@"slot"] isEqual:@"previous"]);
    assert([now[@"lines"] isEqual:expected]);
    assert([now[@"snapshot_bytes"] unsignedLongLongValue] == bytes.length);
    NSArray *past = @[@"IOS_DIAG_SETTING key=0 value=30", @"IOS_SHADER_OPTION option=1 enabled=1"];
    assert([last[@"lines"] isEqual:past]);
    NSData *encoded = skyrim_diagnostics_encode(report, &error);
    assert(encoded && !error);
    assert([[NSData dataWithContentsOfFile:current] isEqualToData:bytes]); // live file is never rewritten
    NSString *safe = [[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding];
    assert(![safe containsString:root]);
    for (NSString *secret in @[@"AUTHORED_", @"/private/", @"person@example", @"Bearer", @"日本語の秘密"]) {
        assert(![safe containsString:secret]);
    }
    writeData([root stringByAppendingPathComponent:@"captured-report.json"], encoded);
    assert(!skyrim_diagnostics_capture(folder, current, 11, &error)); checkError(error);
    assert(!skyrim_diagnostics_capture(root, current, 12, &error)); checkError(error);
    puts("actual capture allowlist partial UTF8 prior-build settings PASS");
}
static void captureLimits(NSString *root) {
    NSError *error = nil;
    NSString *folder = directory(root, @"symlink");
    NSString *source = [folder stringByAppendingPathComponent:@"authored.txt"];
    NSString *current = [folder stringByAppendingPathComponent:@"run-current.log"];
    writeText(source, simpleLog(12));
    assert(symlink(source.fileSystemRepresentation, current.fileSystemRepresentation) == 0);
    assert(!skyrim_diagnostics_capture(folder, current, 12, &error)); checkError(error);

    folder = directory(root, @"fifo");
    current = [folder stringByAppendingPathComponent:@"run-current.log"];
    assert(mkfifo(current.fileSystemRepresentation, 0600) == 0);
    // No writer exists: blocking open-before-fstat would hang this fixture.
    assert(!skyrim_diagnostics_capture(folder, current, 12, &error)); checkError(error);

    folder = directory(root, @"sparse");
    current = [folder stringByAppendingPathComponent:@"run-current.log"];
    int fd = open(current.fileSystemRepresentation, O_CREAT | O_WRONLY | O_EXCL, 0600);
    assert(fd >= 0 && ftruncate(fd, (64LL << 20) + 1) == 0); assert(close(fd) == 0);
    assert(!skyrim_diagnostics_capture(folder, current, 12, &error)); checkError(error);

    folder = directory(root, @"conflicting");
    current = [folder stringByAppendingPathComponent:@"run-current.log"];
    writeText(current, [simpleLog(12) stringByAppendingString:simpleLog(8)]);
    assert(!skyrim_diagnostics_capture(folder, current, 12, &error)); checkError(error);

    folder = directory(root, @"line-limit");
    current = [folder stringByAppendingPathComponent:@"run-current.log"];
    NSMutableString *many = [header(12) mutableCopy];
    for (NSUInteger n = 0; n <= SKYRIM_DIAGNOSTIC_MAX_SESSION_LINES; ++n)
        [many appendString:@"IOS_SHADER_OPTION option=0 enabled=0\n"];
    writeText(current, many);
    assert(!skyrim_diagnostics_capture(folder, current, 12, &error)); checkError(error);

    folder = directory(root, @"byte-limit");
    current = [folder stringByAppendingPathComponent:@"run-current.log"];
    NSMutableString *wide = [@"IOS_STALL schema=1 threshold_ns=50000000 count=100 dropped_busy=0 dropped_full=0 fields=kind,thread,start_ns,end_ns,failed,estimated_end spans=" mutableCopy];
    for (unsigned n = 0; n < 100; ++n) [wide appendString:n ? @";0,1,1000000000,1150000000,0,0" : @"0,1,1000000000,1150000000,0,0"];
    assert(wide.length < SKYRIM_DIAGNOSTIC_MAX_LINE);
    many = [header(12) mutableCopy];
    while (many.length <= SKYRIM_DIAGNOSTIC_MAX_SESSION_BYTES + wide.length) [many appendFormat:@"%@\n", wide];
    writeText(current, many);
    assert(!skyrim_diagnostics_capture(folder, current, 12, &error)); checkError(error);

    NSString *huge = [@"" stringByPaddingToLength:SKYRIM_DIAGNOSTIC_MAX_BODY_BYTES + 1 withString:@"a" startingAtIndex:0];
    NSDictionary *oversized = @{@"oversized":huge};
    assert(!skyrim_diagnostics_encode(oversized, &error)); checkError(error);
    puts("read session line byte encode bounds and symlink rejection PASS");
}
static void pumpUntil(BOOL (^done)(void)) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10];
    while (!done() && deadline.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    assert(done());
}
static NSString *sendAndWait(int mode, SkyrimDiagnosticStatus expected) {
    atomic_store(&responseMode, mode);
    __block BOOL done = NO;
    __block NSString *identifier;
    skyrim_diagnostics_send(^(SkyrimDiagnosticStatus status, NSString *value) {
        assert(NSThread.isMainThread && status == expected);
        assert((status == SkyrimDiagnosticSent) == (value != nil));
        identifier = value; done = YES;
    });
    pumpUntil(^BOOL { return done; });
    return identifier;
}
static NSDictionary *lastRequest(void) {
    [requestLock lock]; NSDictionary *last = requests.lastObject; [requestLock unlock];
    return last;
}
static void transportTests(NSString *root) {
    NSString *folder = directory(root, @"transport");
    NSString *current = [folder stringByAppendingPathComponent:@"run-current.log"];
    writeText(current, simpleLog(12));
    skyrim_diagnostics_set_log_path(current.fileSystemRepresentation);
    assert(skyrim_diagnostics_available());
    sendAndWait(ReplyOffline, SkyrimDiagnosticConnectionFailed);
    NSData *failedBody = lastRequest()[@"body"];
    writeText(current, [simpleLog(12) stringByAppendingString:@"IOS_SHADER_OPTION option=4 enabled=1\n"]);
    NSString *retryID = sendAndWait(ReplyDuplicate, SkyrimDiagnosticSent);
    assert([lastRequest()[@"body"] isEqualToData:failedBody]); // immutable idempotent retry
    NSString *newID = sendAndWait(ReplyCreated, SkyrimDiagnosticSent);
    assert(![newID isEqual:retryID] && ![lastRequest()[@"body"] isEqualToData:failedBody]);

    int invalid[] = {ReplyWrongHash, ReplyFractionalBytes, ReplyWrongType, ReplyLargeLength,
        ReplyLargeBody, ReplyBadSchema, ReplyBooleanSchema, ReplyMalformedJSON, ReplyExtraField};
    for (NSUInteger n = 0; n < sizeof invalid / sizeof invalid[0]; ++n) {
        sendAndWait(invalid[n], SkyrimDiagnosticInvalidReceipt);
        sendAndWait(ReplyCreated, SkyrimDiagnosticSent); // completes the same pending immutable report
    }
    sendAndWait(ReplyForbidden, SkyrimDiagnosticRejected);
    sendAndWait(ReplyCreated, SkyrimDiagnosticSent);

    atomic_store(&responseMode, ReplyDelayed);
    __block BOOL finished = NO;
    skyrim_diagnostics_send(^(SkyrimDiagnosticStatus status, NSString *identifier) {
        assert(NSThread.isMainThread && status == SkyrimDiagnosticSent && identifier); finished = YES;
    });
    __block BOOL busy = NO;
    skyrim_diagnostics_send(^(SkyrimDiagnosticStatus status, NSString *identifier) {
        assert(NSThread.isMainThread && status == SkyrimDiagnosticBusy && !identifier); busy = YES;
    });
    assert(busy); pumpUntil(^BOOL { return finished; });

    // Exercise the production delegate's redirect refusal without invoking a
    // network redirect (the bearer must never leave the approved request URL).
    NSURL *approvedURL = [NSURL URLWithString:fixturePairing[@"endpoint"]];
    NSURLSession *unusedSession = [NSURLSession sessionWithConfiguration:
        [NSURLSessionConfiguration fixture_ephemeralConfiguration]];
    NSURLSessionDataTask *unusedTask = [unusedSession dataTaskWithURL:approvedURL]; // never resumed
    NSHTTPURLResponse *redirect = [[NSHTTPURLResponse alloc] initWithURL:approvedURL
        statusCode:302 HTTPVersion:@"HTTP/1.1" headerFields:@{}];
    SKDiagnosticTransfer *isolated = [SKDiagnosticTransfer new];
    __block BOOL refused = NO;
    [isolated URLSession:unusedSession task:unusedTask willPerformHTTPRedirection:redirect newRequest:
        [NSURLRequest requestWithURL:[NSURL URLWithString:@"https://other.example.invalid/"]]
        completionHandler:^(NSURLRequest *request) { assert(!request); refused = YES; }];
    assert(refused && isolated.invalidResponse);
    isolated = [SKDiagnosticTransfer new];
    NSURLResponse *nonHTTP = [[NSURLResponse alloc] initWithURL:approvedURL
        MIMEType:@"application/json" expectedContentLength:0 textEncodingName:nil];
    __block BOOL rejected = NO;
    [isolated URLSession:unusedSession dataTask:unusedTask didReceiveResponse:nonHTTP
        completionHandler:^(NSURLSessionResponseDisposition disposition) {
            assert(disposition == NSURLSessionResponseCancel); rejected = YES;
        }];
    assert(rejected && isolated.invalidResponse);
    [unusedSession invalidateAndCancel];

    NSDictionary *savedPairing = fixturePairing; fixturePairing = @{};
    sendAndWait(ReplyCreated, SkyrimDiagnosticNotConfigured);
    fixturePairing = savedPairing;
    skyrim_diagnostics_set_log_path(NULL);
    sendAndWait(ReplyCreated, SkyrimDiagnosticCaptureFailed);
    puts("actual NSURLSession sender receipt busy immutable retry main-completion PASS");
}
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 3);
    NSString *root = [NSString stringWithUTF8String:argv[1]];
    NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]];
    NSDictionary *fixture = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    assert(fixture);
    fixturePairing = @{@"endpoint":@"https://fixture.tail0.ts.net/skyrim-diagnostics/v1/reports",
        @"token":[@"" stringByPaddingToLength:43 withString:@"a" startingAtIndex:0]};
    requestLock = [NSLock new]; requests = [NSMutableArray array];
    pairingTests(); captureTests(root, fixture); captureLimits(root); transportTests(root);
    puts("DIAGNOSTIC_UPLOAD actual Foundation and mocked transport PASS");
} }
