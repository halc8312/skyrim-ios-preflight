/* SPDX-License-Identifier: GPL-3.0-only
 * Manual, bounded numeric diagnostic transfer. No save/game files are read,
 * no raw guest output is exported, and no work is added to the frame loop. */
#import "diagnostic_upload.h"
#include "diagnostic_schema.h"
#import <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const NSUInteger kBodyLimit = SKYRIM_DIAGNOSTIC_MAX_BODY_BYTES;
static const NSUInteger kSessionLimit = SKYRIM_DIAGNOSTIC_MAX_SESSION_BYTES;
static NSString *currentLog;

static BOOL completeMatch(NSRegularExpression *rule, NSString *line) {
    NSTextCheckingResult *match = [rule firstMatchInString:line options:0 range:NSMakeRange(0, line.length)];
    return match && NSEqualRanges(match.range, NSMakeRange(0, line.length));
}
static BOOL matches(NSString *pattern, NSString *line) {
    if (![line isKindOfClass:NSString.class]) return NO;
    return completeMatch([NSRegularExpression regularExpressionWithPattern:pattern options:0 error:NULL], line);
}
static NSError *captureError(void) {
    /* Never include a source path or raw line in user-facing or upload errors. */
    return [NSError errorWithDomain:@"SkyrimDiagnostics" code:1 userInfo:nil];
}
BOOL skyrim_diagnostics_valid_pairing(NSDictionary *pairing) {
    if (![pairing isKindOfClass:NSDictionary.class] || pairing.count != 2) return NO;
    NSString *url = pairing[@"endpoint"], *token = pairing[@"token"];
    if (![url isKindOfClass:NSString.class] || !matches(@"[A-Za-z0-9_-]{43,128}", token)) return NO;
    NSURLComponents *parts = [NSURLComponents componentsWithString:url];
    return [parts.scheme isEqualToString:@"https"] && !parts.user && !parts.password &&
        !parts.query && !parts.fragment && !parts.port &&
        matches(@"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\\.ts\\.net", parts.host) &&
        [parts.percentEncodedPath isEqualToString:@"/skyrim-diagnostics/v1/reports"] &&
        [parts.URL.absoluteString isEqualToString:url];
}
static NSArray<NSRegularExpression *> *rules(void) {
    static NSArray *result;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableArray *all = [NSMutableArray array];
#define DIAG_RULE(prefix, pattern) do { \
        NSRegularExpression *r = [NSRegularExpression regularExpressionWithPattern:@pattern options:0 error:NULL]; \
        if (!r) abort(); [all addObject:r]; \
    } while (0);
        SKYRIM_DIAG_RULES(DIAG_RULE)
#undef DIAG_RULE
        result = [all copy];
    });
    return result;
}
static BOOL allowed(NSString *line) {
    if (!line || [line lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > SKYRIM_DIAGNOSTIC_MAX_LINE ||
        [line rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound) return NO;
    for (NSRegularExpression *r in rules()) if (completeMatch(r, line)) return YES;
    return NO;
}
static NSString *numericStaticError(NSString *line) {
    if (![line hasPrefix:@"FATAL iOS startup: "]) return nil;
    NSString *message = [line substringFromIndex:19];
#define DIAG_ERROR(text, code) if ([message isEqualToString:@text]) return [NSString stringWithFormat:@"IOS_DIAG_ERROR code=%u", (unsigned)(code)];
    SKYRIM_DIAG_STATIC_ERRORS(DIAG_ERROR)
#undef DIAG_ERROR
    return nil;
}
static const off_t kStartupLogLimit = 256 * 1024;
static const NSUInteger kStartupSearchLimit = 32;
static NSString *const kRecoveryPrefix = @"IOS_DIAG_RECOVERY ";
static NSString *const kBuild28Header = @"SkyrimIOS PC2011 build=28; session gameplay requires observation";

/* Restrict addresses to the pinned LE executable's .text section. */
static uint32_t guestTextAddress(NSString *line, NSRange range) {
    uint32_t value = (uint32_t)strtoul([line substringWithRange:range].UTF8String, NULL, 16);
    return value >= 0x00401000 && value < 0x0106a31a ? value : 0;
}

typedef NS_ENUM(unsigned, SKStartupEnd) {
    SKStartupNoRecord, SKStartupOtherFatal, SKStartupMissingCode, SKStartupMissingImport,
    SKStartupInstruction, SKStartupBreakpoint, SKStartupException, SKStartupExit, SKStartupReturn
};

static SKStartupEnd startupEnd(NSString *line, uint32_t *target, uint32_t *caller) {
    if ([line hasPrefix:@"FATAL: "]) {
        static NSArray<NSRegularExpression *> *dispatchRules;
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            dispatchRules = @[
                [NSRegularExpression regularExpressionWithPattern:@"FATAL: no recompiled code at ([0-9a-fA-F]{8}) \\(return address ([0-9a-fA-F]{8})\\)" options:0 error:NULL],
                [NSRegularExpression regularExpressionWithPattern:@"FATAL: unimplemented import [^\\r\\n]{1,512} \\(called from ([0-9a-fA-F]{8})\\)" options:0 error:NULL],
                [NSRegularExpression regularExpressionWithPattern:@"FATAL: unimplemented instruction '[^\\r\\n]{1,256}' at ([0-9a-fA-F]{8})" options:0 error:NULL],
                [NSRegularExpression regularExpressionWithPattern:@"FATAL: int3 at ([0-9a-fA-F]{8})" options:0 error:NULL]
            ];
        });
        for (NSUInteger n = 0; n < dispatchRules.count; ++n) {
            NSTextCheckingResult *match = [dispatchRules[n] firstMatchInString:line options:0 range:NSMakeRange(0, line.length)];
            if (!match || !NSEqualRanges(match.range, NSMakeRange(0, line.length))) continue;
            const uint32_t address = guestTextAddress(line, [match rangeAtIndex:1]);
            if (n == 1) *caller = address;
            else *target = address;
            if (n == 0) *caller = guestTextAddress(line, [match rangeAtIndex:2]);
            return (SKStartupEnd)(SKStartupMissingCode + n);
        }
        if (matches(@"FATAL: (?:RaiseException [0-9a-fA-F]{8}|C\\+\\+ exception thrown(?: in recompiled DLL)? \\(object [0-9a-fA-F]{8}, (?:type info|info) [0-9a-fA-F]{8}\\)|__CxxFrameHandler invoked)", line))
            return SKStartupException;
        return SKStartupOtherFatal;
    }
    if ([line hasPrefix:@"ExitProcess("] || [line hasPrefix:@"TerminateProcess("] || [line hasPrefix:@"msvcrt exit("])
        return matches(@"(?:ExitProcess|TerminateProcess|msvcrt exit)\\(-?[0-9]{1,10}\\)", line) ? SKStartupExit : SKStartupNoRecord;
    if ([line hasPrefix:@"Guest entry returned "])
        return matches(@"Guest entry returned -?[0-9]{1,10}; no title success was inferred", line) ? SKStartupReturn : SKStartupNoRecord;
    return SKStartupNoRecord;
}

/* This is a separate, short backward scan. Detailed logs from more recent
 * successful launches keep their usual current and previous slots. */
static NSData *readStartupLog(NSString *path, NSTimeInterval deadline) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return nil;
    struct stat before, after;
    const BOOL ready = !fstat(fd, &before) && S_ISREG(before.st_mode) &&
        before.st_size > 0 && before.st_size <= kStartupLogLimit;
    NSMutableData *raw = ready ? [NSMutableData dataWithLength:(NSUInteger)before.st_size] : nil;
    uint8_t *bytes = raw.mutableBytes;
    size_t offset = 0;
    while (raw && offset < raw.length) {
        if (NSProcessInfo.processInfo.systemUptime >= deadline) break;
        const ssize_t count = read(fd, bytes + offset, raw.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += (size_t)count;
    }
    const BOOL stable = raw && offset == raw.length && !fstat(fd, &after) &&
        before.st_size == after.st_size && before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
        before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec;
    close(fd);
    return stable ? raw : nil;
}

static NSString *startupRecoveryRecord(NSData *raw) {
    if (!raw.length) return nil;
    const char *cursor = raw.bytes, *end = cursor + raw.length, *lf = NULL;
    const char family[] = "SkyrimIOS PC2011 build=";
    BOOL identified = NO;
    /* NSLog or an early system message can precede the explicit header. */
    for (NSUInteger i = 0; i < 24 && cursor < end; ++i) {
        lf = memchr(cursor, '\n', (size_t)(end - cursor));
        if (!lf) break;
        const size_t len = (size_t)(lf - cursor);
        if (len >= sizeof family - 1 && !memcmp(cursor, family, sizeof family - 1)) {
            identified = len == kBuild28Header.length && !memcmp(cursor, kBuild28Header.UTF8String, len);
            break;
        }
        cursor = lf + 1;
    }
    if (!identified) return nil;

    BOOL guestEntry = NO, graphics = NO;
    SKStartupEnd reason = SKStartupNoRecord;
    uint32_t target = 0, caller = 0;
    while ((cursor = lf + 1) < end && (lf = memchr(cursor, '\n', (size_t)(end - cursor)))) {
        const size_t len = (size_t)(lf - cursor);
        if (!len || len > SKYRIM_DIAGNOSTIC_MAX_LINE) continue;
        @autoreleasepool {
            NSString *line = [[NSString alloc] initWithBytes:cursor length:len encoding:NSUTF8StringEncoding];
            if ([line hasPrefix:@"IOS_STAGE first-successful-present-title-not-verified "] && allowed(line))
                return nil;
            if ([line hasPrefix:@"IOS_STAGE guest-entry "] && allowed(line)) guestEntry = YES;
            if (!guestEntry) continue;
            if ([line hasPrefix:@"IOS_GRAPHICS_POLICY "] && allowed(line)) graphics = YES;
            uint32_t observedTarget = 0, observedCaller = 0;
            const SKStartupEnd observed = startupEnd(line, &observedTarget, &observedCaller);
            if (observed != SKStartupNoRecord && (reason == SKStartupNoRecord || observed != SKStartupOtherFatal)) {
                reason = observed;
                target = observedTarget;
                caller = observedCaller;
            }
        }
    }
    if (!guestEntry) return nil;
    return [NSString stringWithFormat:@"%@schema=1 build=28 guest_entry=1 graphics=%u reason=%u target=%08x caller=%08x",
        kRecoveryPrefix, (unsigned)graphics, (unsigned)reason, (unsigned)target, (unsigned)caller];
}

static void appendStartupRecovery(NSMutableDictionary *now, NSArray<NSDictionary *> *candidates) {
    const NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 3;
    NSArray<NSDictionary *> *newest = [candidates sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        NSComparisonResult result = [b[@"seconds"] compare:a[@"seconds"]];
        if (result == NSOrderedSame) result = [b[@"nanos"] compare:a[@"nanos"]];
        return result == NSOrderedSame ? [a[@"path"] compare:b[@"path"]] : result;
    }];
    const NSUInteger limit = MIN(newest.count, kStartupSearchLimit);
    for (NSUInteger i = 0; i < limit && NSProcessInfo.processInfo.systemUptime < deadline; ++i) {
        @autoreleasepool {
            NSString *record = startupRecoveryRecord(readStartupLog(newest[i][@"path"], deadline));
            if (!record) continue;
            NSArray<NSString *> *lines = now[@"lines"];
            if (lines.count >= SKYRIM_DIAGNOSTIC_MAX_SESSION_LINES) return;
            NSUInteger bytes = record.length + 1; // the record is ASCII
            for (NSString *line in lines) bytes += [line lengthOfBytesUsingEncoding:NSUTF8StringEncoding] + 1;
            if (bytes <= kSessionLimit) now[@"lines"] = [lines arrayByAddingObject:record];
            return;
        }
    }
}

static NSDictionary *session(NSString *path, BOOL live, NSError **error) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { if (error) *error = captureError(); return nil; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > (64LL << 20)) {
        close(fd); if (error) *error = captureError(); return nil;
    }
    const off_t end = st.st_size;
    const NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 10;
    off_t readBytes = 0;
    NSUInteger omitted = 0, keptBytes = 0, build = 0, len = 0;
    BOOL oversized = NO, failed = NO;
    char block[65536], line[32768];
    NSMutableArray *lines = [NSMutableArray array];
    NSRegularExpression *header = [NSRegularExpression regularExpressionWithPattern:
        @"SkyrimIOS PC2011 build=([1-9][0-9]{0,5}); session gameplay requires observation" options:0 error:NULL];
    while (readBytes < end && !failed) {
        if (NSProcessInfo.processInfo.systemUptime >= deadline) { failed = YES; break; }
        ssize_t count = read(fd, block, (size_t)MIN((off_t)sizeof block, end-readBytes));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { failed = YES; break; }
        readBytes += count;
        for (ssize_t i = 0; i < count; ++i) {
            if (block[i] != '\n') {
                if (len < sizeof line) line[len++] = block[i]; else oversized = YES;
                continue;
            }
            @autoreleasepool {
                NSString *text = oversized ? nil : [[NSString alloc] initWithBytes:line length:len encoding:NSUTF8StringEncoding];
                NSTextCheckingResult *h = text ? [header firstMatchInString:text options:0 range:NSMakeRange(0, text.length)] : nil;
                if (h && NSEqualRanges(h.range, NSMakeRange(0, text.length))) {
                    NSUInteger found = [[text substringWithRange:[h rangeAtIndex:1]] integerValue];
                    if (build && found != build) failed = YES;
                    build = found;
                }
                NSString *errorCode = numericStaticError(text);
                if (errorCode) text = errorCode;
                if (![text hasPrefix:kRecoveryPrefix] && allowed(text)) {
                    keptBytes += [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding] + 1;
                    if (keptBytes > kSessionLimit || lines.count >= SKYRIM_DIAGNOSTIC_MAX_SESSION_LINES) failed = YES;
                    else [lines addObject:text];
                } else ++omitted;
            }
            len = 0; oversized = NO;
            if (failed) break;
        }
    }
    if (len || oversized) ++omitted; // An incomplete final line is never exported.
    struct stat after;
    if (fstat(fd, &after) || after.st_size < end) failed = YES;
    close(fd);
    if (failed || !build || !lines.count) { if (error) *error = captureError(); return nil; }
    return @{@"slot": live ? @"current" : @"previous", @"build": @(build),
        @"capture": live ? @"live_prefix" : @"closed", @"snapshot_bytes": @(end),
        @"excluded_lines": @(omitted), @"lines": lines};
}
NSDictionary *skyrim_diagnostics_capture(NSString *directory, NSString *current,
                                        NSUInteger build, NSError **error) {
    if (error) *error = nil;
    if (!build || build > 999999 || ![[current stringByDeletingLastPathComponent] isEqualToString:directory]) {
        if (error) *error = captureError(); return nil;
    }
    NSDictionary *now = session(current, YES, error);
    if (!now || [now[@"build"] unsignedIntegerValue] != build) {
        if (error) *error = captureError(); return nil;
    }
    NSArray *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:error];
    if (!names || names.count > 10000) { if (error) *error = captureError(); return nil; }
    NSString *previous;
    NSMutableArray<NSDictionary *> *startupLogs = [NSMutableArray array];
    struct timespec latest = {0, 0};
    for (NSString *name in names) {
        if (![name hasPrefix:@"run-"] || ![name hasSuffix:@".log"]) continue;
        NSString *path = [directory stringByAppendingPathComponent:name];
        if ([path isEqualToString:current]) continue;
        struct stat s;
        if (lstat(path.fileSystemRepresentation, &s) || !S_ISREG(s.st_mode)) continue;
        struct timespec date = s.st_mtimespec;
        if (s.st_size > 0 && s.st_size <= kStartupLogLimit)
            [startupLogs addObject:@{@"path":path, @"seconds":@(date.tv_sec), @"nanos":@(date.tv_nsec)}];
        if (!previous || date.tv_sec > latest.tv_sec || (date.tv_sec == latest.tv_sec && date.tv_nsec > latest.tv_nsec)) {
            previous = path; latest = date;
        }
    }
    NSMutableDictionary *live = [now mutableCopy];
    appendStartupRecovery(live, startupLogs);
    NSMutableArray *sessions = [NSMutableArray arrayWithObject:live];
    if (previous) {
        NSDictionary *last = session(previous, NO, error);
        if (!last) return nil; // Explicit failure, never silently claim both were sent.
        [sessions addObject:last];
    }
    NSString *identifier = [[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString];
    return @{@"schema": @1, @"report_id": identifier, @"app_build": @(build), @"sessions": sessions};
}
NSData *skyrim_diagnostics_encode(NSDictionary *report, NSError **error) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:error];
    if (data.length > kBodyLimit) { if (error) *error = captureError(); return nil; }
    return data;
}
static NSString *digest(NSData *body) {
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(body.bytes, (CC_LONG)body.length, bytes);
    NSMutableString *hex = [NSMutableString stringWithCapacity:64];
    for (NSUInteger i = 0; i < sizeof bytes; ++i) [hex appendFormat:@"%02x", bytes[i]];
    return hex;
}
static BOOL exactInteger(id value, uint64_t expected) {
    return [value isKindOfClass:NSNumber.class] &&
        CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID() &&
        !CFNumberIsFloatType((__bridge CFNumberRef)value) &&
        [value unsignedLongLongValue] == expected;
}

@interface SKDiagnosticTransfer : NSObject <NSURLSessionDataDelegate>
@property(nonatomic, strong) NSData *body;
@property(nonatomic, strong) NSString *identifier, *contentSHA256;
@property(nonatomic, strong) NSMutableData *receipt;
@property(nonatomic, strong) NSURLSession *connection;
@property(nonatomic, copy) void (^finished)(SkyrimDiagnosticStatus, NSString *);
@property(nonatomic) NSInteger responseCode;
@property(nonatomic) BOOL busy, invalidResponse;
@end
@implementation SKDiagnosticTransfer
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t willPerformHTTPRedirection:(NSHTTPURLResponse *)r newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))completion {
    self.invalidResponse = YES; completion(nil); // Never send the bearer to another URL.
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    if (![response isKindOfClass:NSHTTPURLResponse.class]) {
        self.invalidResponse = YES; completion(NSURLSessionResponseCancel); return;
    }
    self.responseCode = [(NSHTTPURLResponse *)response statusCode];
    if (![response.MIMEType isEqualToString:@"application/json"] || response.expectedContentLength > 4096) {
        self.invalidResponse = YES; completion(NSURLSessionResponseCancel); return;
    }
    completion(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveData:(NSData *)data {
    if (self.receipt.length + data.length > 4096) { self.invalidResponse = YES; [t cancel]; }
    else [self.receipt appendData:data];
}
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t didCompleteWithError:(NSError *)error {
    SkyrimDiagnosticStatus status = self.invalidResponse ? SkyrimDiagnosticInvalidReceipt :
        error ? SkyrimDiagnosticConnectionFailed : SkyrimDiagnosticRejected;
    if (!error && !self.invalidResponse && (self.responseCode == 200 || self.responseCode == 201)) {
        id receipt = [NSJSONSerialization JSONObjectWithData:self.receipt options:0 error:NULL];
        BOOL valid = [receipt isKindOfClass:NSDictionary.class] && [receipt count] == 4 &&
            exactInteger(receipt[@"schema"], 1) &&
            [receipt[@"report_id"] isEqual:self.identifier] && [receipt[@"sha256"] isEqual:self.contentSHA256] &&
            exactInteger(receipt[@"bytes"], self.body.length);
        status = valid ? SkyrimDiagnosticSent : SkyrimDiagnosticInvalidReceipt;
    }
    NSString *identifier = status == SkyrimDiagnosticSent ? self.identifier : nil;
    [s finishTasksAndInvalidate]; self.connection = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        /* Keep failed immutable bytes for an idempotent manual retry. */
        if (status == SkyrimDiagnosticSent) { self.body = nil; self.identifier = nil; self.contentSHA256 = nil; }
        self.busy = NO;
        void (^done)(SkyrimDiagnosticStatus, NSString *) = self.finished; self.finished = nil;
        if (done) done(status, identifier);
    });
}
@end
static SKDiagnosticTransfer *transfer(void) {
    static SKDiagnosticTransfer *instance;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ instance = [SKDiagnosticTransfer new]; });
    return instance;
}
void skyrim_diagnostics_set_log_path(const char *path) {
    currentLog = path ? [NSString stringWithUTF8String:path] : nil;
}
BOOL skyrim_diagnostics_available(void) {
    return skyrim_diagnostics_valid_pairing([NSBundle.mainBundle objectForInfoDictionaryKey:@"PcDiagnostics"]);
}
void skyrim_diagnostics_send(void (^completion)(SkyrimDiagnosticStatus, NSString *)) {
    NSCAssert(NSThread.isMainThread, @"diagnostic requests run on the main thread");
    SKDiagnosticTransfer *sender = transfer();
    NSDictionary *pairing = [NSBundle.mainBundle objectForInfoDictionaryKey:@"PcDiagnostics"];
    if (!skyrim_diagnostics_valid_pairing(pairing)) { completion(SkyrimDiagnosticNotConfigured, nil); return; }
    if (sender.busy) { completion(SkyrimDiagnosticBusy, nil); return; }
    sender.busy = YES; sender.finished = completion;
    NSString *path = currentLog;
    NSUInteger build = [[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] integerValue];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            if (!sender.body) {
                NSDictionary *report = skyrim_diagnostics_capture(path.stringByDeletingLastPathComponent, path, build, NULL);
                sender.body = report ? skyrim_diagnostics_encode(report, NULL) : nil;
                if (sender.body) { sender.identifier = report[@"report_id"]; sender.contentSHA256 = digest(sender.body); }
            }
            if (!sender.body) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    sender.busy = NO; sender.finished = nil; completion(SkyrimDiagnosticCaptureFailed, nil);
                }); return;
            }
            sender.receipt = [NSMutableData data]; sender.invalidResponse = NO; sender.responseCode = 0;
            NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
            config.URLCache = nil; config.HTTPCookieStorage = nil; config.URLCredentialStorage = nil;
            config.HTTPShouldSetCookies = NO; config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            config.timeoutIntervalForRequest = 30; config.timeoutIntervalForResource = 90;
            config.waitsForConnectivity = NO; config.HTTPMaximumConnectionsPerHost = 1;
            NSOperationQueue *callbacks = [NSOperationQueue new]; callbacks.maxConcurrentOperationCount = 1;
            sender.connection = [NSURLSession sessionWithConfiguration:config delegate:sender delegateQueue:callbacks];
            NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:pairing[@"endpoint"]]];
            request.HTTPMethod = @"POST"; request.HTTPBody = sender.body;
            [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
            [request setValue:[NSString stringWithFormat:@"Bearer %@", pairing[@"token"]] forHTTPHeaderField:@"Authorization"];
            [request setValue:sender.contentSHA256 forHTTPHeaderField:@"X-Content-SHA256"];
            [[sender.connection dataTaskWithRequest:request] resume];
        }
    });
}
