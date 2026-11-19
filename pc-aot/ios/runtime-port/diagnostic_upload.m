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
                if (allowed(text)) {
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
    struct timespec latest = {0, 0};
    for (NSString *name in names) {
        if (![name hasPrefix:@"run-"] || ![name hasSuffix:@".log"]) continue;
        NSString *path = [directory stringByAppendingPathComponent:name];
        if ([path isEqualToString:current]) continue;
        struct stat s;
        if (lstat(path.fileSystemRepresentation, &s) || !S_ISREG(s.st_mode)) continue;
        struct timespec date = s.st_mtimespec;
        if (!previous || date.tv_sec > latest.tv_sec || (date.tv_sec == latest.tv_sec && date.tv_nsec > latest.tv_nsec)) {
            previous = path; latest = date;
        }
    }
    NSMutableArray *sessions = [NSMutableArray arrayWithObject:now];
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
#ifdef SKYRIM_IOS_CLASSIC_SKSE
void skyrim_diagnostics_share_classic(void (^completion)(NSString *text)) {
    NSCAssert(NSThread.isMainThread, @"diagnostic requests run on the main thread");
    NSString *path = currentLog;
    NSUInteger build = [[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] integerValue];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            NSDictionary *report = skyrim_diagnostics_capture(path.stringByDeletingLastPathComponent, path, build, NULL);
            NSMutableArray<NSString *> *sections = [NSMutableArray array];
            for (NSDictionary *session in report[@"sessions"]) {
                NSMutableOrderedSet<NSString *> *observed = [NSMutableOrderedSet orderedSet];
                for (NSString *line in session[@"lines"]) {
                    if ([line hasPrefix:@"IOS_CLASSIC_VM "] || [line hasPrefix:@"IOS_CLASSIC_QUERY "])
                        [observed addObject:line];
                }
                if (observed.count) {
                    [sections addObject:[NSString stringWithFormat:@"SkyrimIOS Classic Lab · %@ · build %@\n%@",
                        session[@"slot"], session[@"build"], [observed.array componentsJoinedByString:@"\n"]]];
                }
            }
            NSString *summary = report ? [sections componentsJoinedByString:@"\n\n"] : nil;
            dispatch_async(dispatch_get_main_queue(), ^{ completion(summary); });
        }
    });
}
#endif
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
