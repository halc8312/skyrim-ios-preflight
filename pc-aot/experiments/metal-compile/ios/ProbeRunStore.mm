#import "ProbeRunStore.h"
#include <chrono>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const NSUInteger kRecordLimit = 2 * 1024 * 1024;
static NSString *const kRecordScope = @"authored-metal-phone-comparison-record";

static BOOL StoreError(NSError **out, NSInteger code, NSString *description) {
    if (out) *out = [NSError errorWithDomain:@"MetalProbe.ResultStore" code:code
                                  userInfo:@{NSLocalizedDescriptionKey: description}];
    return NO;
}

static BOOL IOError(NSError **out, NSString *operation) {
    const int saved = errno;
    if (out) *out = [NSError errorWithDomain:NSPOSIXErrorDomain code:saved
                                  userInfo:@{NSLocalizedDescriptionKey: operation}];
    return NO;
}

static NSString *Now(void) {
    NSISO8601DateFormatter *formatter = [NSISO8601DateFormatter new];
    formatter.formatOptions = NSISO8601DateFormatWithInternetDateTime |
                              NSISO8601DateFormatWithFractionalSeconds;
    return [formatter stringFromDate:NSDate.date];
}

static BOOL IsTerminal(NSString *status) {
    return [@[@"passed", @"failed", @"unsupported", @"interrupted"] containsObject:status];
}

static NSData *Encode(NSDictionary *record, NSError **error) {
    @try {
        if (![NSJSONSerialization isValidJSONObject:record]) {
            StoreError(error, 3, @"結果に保存できない値が含まれています。");
            return nil;
        }
        NSData *data = [NSJSONSerialization dataWithJSONObject:record
                              options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:error];
        if (data.length > kRecordLimit) {
            StoreError(error, 3, @"結果ファイルの上限を超えました。");
            return nil;
        }
        return data;
    } @catch (NSException *exception) {
        (void)exception;
        StoreError(error, 3, @"結果を保存用の形式に変換できませんでした。");
        return nil;
    }
}

@interface ProbeRunStore () {
    int _rootFD;
    NSMutableDictionary<NSString *, NSDictionary *> *_active;
    NSURL *_latestReportURL;
    NSURL *_rootURL;
}
@end

@implementation ProbeRunStore

- (nullable instancetype)initWithRootURL:(NSURL *)rootURL error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    _rootFD = -1;
    if (!rootURL.isFileURL) {
        StoreError(error, 1, @"結果の保存先が正しくありません。");
        return nil;
    }
    NSURL *standard = rootURL.URLByStandardizingPath;
    struct stat st{};
    if (lstat(standard.path.fileSystemRepresentation, &st) == 0 && S_ISLNK(st.st_mode)) {
        StoreError(error, 1, @"リンクを結果の保存先には使えません。");
        return nil;
    }
    NSFileManager *files = NSFileManager.defaultManager;
    if (![files createDirectoryAtURL:standard withIntermediateDirectories:YES attributes:nil error:error])
        return nil;
    // System parents such as /var may themselves be aliases. Bind the chosen
    // real root once, then address only children through its descriptor.
    _rootURL = standard.URLByResolvingSymlinksInPath;
    _rootFD = open(_rootURL.path.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (_rootFD < 0) { IOError(error, @"結果の保存先を開けませんでした。"); return nil; }
    _active = [NSMutableDictionary new];
    NSString *latestDate = @"";
    NSArray<NSURL *> *directories = [files contentsOfDirectoryAtURL:_rootURL
                     includingPropertiesForKeys:nil options:0 error:error];
    if (!directories) return nil;
    for (NSURL *directory in directories) {
        NSString *runID = directory.lastPathComponent;
        if (![[[NSUUID alloc] initWithUUIDString:runID].UUIDString isEqual:runID]) continue;
        // Do not follow links while discovering prior evidence.
        if (fstatat(_rootFD, runID.fileSystemRepresentation, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(st.st_mode)) continue;
        NSArray<NSURL *> *entries = [files contentsOfDirectoryAtURL:directory
                     includingPropertiesForKeys:nil options:0 error:NULL];
        for (NSURL *entry in entries) {
            NSError *ignored = nil;
            NSDictionary *record = [self readRecordAtURL:entry error:&ignored];
            if (!record) continue;
            NSString *date = record[@"created_at"];
            if ([date compare:latestDate] != NSOrderedAscending) {
                latestDate = date;
                _latestReportURL = entry;
            }
        }
    }
    return self;
}

- (void)dealloc { if (_rootFD >= 0) close(_rootFD); }
- (NSURL *)rootURL { return _rootURL; }
- (NSURL *)latestReportURL { @synchronized(self) { return _latestReportURL; } }

- (BOOL)partsForURL:(NSURL *)url run:(NSString **)run file:(NSString **)file error:(NSError **)error {
    if (!url.isFileURL) return StoreError(error, 1, @"結果ファイルの場所が正しくありません。");
    NSArray<NSString *> *parts = url.URLByStandardizingPath.path.pathComponents;
    NSArray<NSString *> *base = _rootURL.path.pathComponents;
    if (parts.count != base.count + 2 ||
        ![[parts subarrayWithRange:NSMakeRange(0, base.count)] isEqual:base])
        return StoreError(error, 1, @"結果ファイルが保存先の外にあります。");
    NSString *runID = parts[base.count], *filename = parts.lastObject;
    if (![[[NSUUID alloc] initWithUUIDString:runID].UUIDString isEqual:runID] ||
        ![filename hasPrefix:@"Metal比較結果_"] || ![filename hasSuffix:@".json"] ||
        filename.length != @"Metal比較結果_20261118_073000.json".length)
        return StoreError(error, 1, @"結果ファイルの名前が正しくありません。");
    NSString *date = [filename substringWithRange:NSMakeRange(@"Metal比較結果_".length, 15)];
    NSCharacterSet *invalid = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789_"] invertedSet];
    if ([date rangeOfCharacterFromSet:invalid].location != NSNotFound || [date characterAtIndex:8] != '_')
        return StoreError(error, 1, @"結果ファイルの日付が正しくありません。");
    *run = runID; *file = filename;
    return YES;
}

- (BOOL)saveData:(NSData *)data toURL:(NSURL *)url initial:(BOOL)initial error:(NSError **)error {
    NSString *run = nil, *file = nil;
    if (![self partsForURL:url run:&run file:&file error:error]) return NO;
    int directory = openat(_rootFD, run.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return IOError(error, @"結果のフォルダを開けませんでした。");
    struct stat st{};
    int found = fstatat(directory, file.fileSystemRepresentation, &st, AT_SYMLINK_NOFOLLOW);
    if ((initial && (found == 0 || errno != ENOENT)) ||
        (!initial && (found != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1))) {
        close(directory);
        return StoreError(error, 2, @"結果ファイルの状態が変わったため保存を中止しました。");
    }
    NSString *temporary = [@".pending-" stringByAppendingString:NSUUID.UUID.UUIDString];
    int fd = openat(directory, temporary.fileSystemRepresentation,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { IOError(error, @"一時保存を開始できませんでした。"); close(directory); return NO; }
    const uint8_t *bytes = (const uint8_t *)data.bytes;
    NSUInteger written = 0;
    BOOL ok = YES;
    while (written < data.length) {
        ssize_t count = write(fd, bytes + written, data.length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count == 0) errno = EIO; ok = IOError(error, @"結果を書き込めませんでした。"); break; }
        written += (NSUInteger)count;
    }
    if (ok && fsync(fd) != 0) ok = IOError(error, @"結果の書き込みを確定できませんでした。");
    if (close(fd) != 0 && ok) ok = IOError(error, @"結果ファイルを閉じられませんでした。");
    if (ok && renameat(directory, temporary.fileSystemRepresentation, directory, file.fileSystemRepresentation) != 0)
        ok = IOError(error, @"結果ファイルを置き換えられませんでした。");
    close(directory);
    // An unsuccessful pending file is retained; prior published evidence remains.
    return ok;
}

- (nullable NSURL *)beginRunWithMetal4First:(BOOL)metal4First error:(NSError **)error {
    @synchronized(self) {
        NSString *runID = NSUUID.UUID.UUIDString;
        if (mkdirat(_rootFD, runID.fileSystemRepresentation, 0700) != 0) {
            IOError(error, @"測定結果のフォルダを作れませんでした。"); return nil;
        }
        NSDateFormatter *formatter = [NSDateFormatter new];
        formatter.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
        formatter.timeZone = NSTimeZone.localTimeZone;
        formatter.dateFormat = @"yyyyMMdd_HHmmss";
        NSString *filename = [NSString stringWithFormat:@"Metal比較結果_%@.json", [formatter stringFromDate:NSDate.date]];
        NSURL *url = [[_rootURL URLByAppendingPathComponent:runID isDirectory:YES] URLByAppendingPathComponent:filename];
        NSString *now = Now();
        NSDictionary *record = @{ @"schema": @1, @"scope": kRecordScope, @"status": @"running",
            @"finished": @NO, @"created_at": now, @"updated_at": now, @"run_id": runID,
            @"metal4_first": @(metal4First), @"comparison": @{}, @"recovered_incomplete": @NO };
        NSData *data = Encode(record, error);
        if (!data || ![self saveData:data toURL:url initial:YES error:error]) return nil;
        _active[url.path] = record;
        _latestReportURL = url;
        return url;
    }
}

- (BOOL)writeSnapshot:(NSDictionary *)snapshot toURL:(NSURL *)url finished:(BOOL)finished
  elapsedNanoseconds:(uint64_t *)elapsed error:(NSError **)error {
    const auto started = std::chrono::steady_clock::now();
    @try {
        return [self writeSnapshot:snapshot toURL:url finished:finished error:error];
    } @finally {
        if (elapsed) *elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    }
}

- (BOOL)writeSnapshot:(NSDictionary *)snapshot toURL:(NSURL *)url finished:(BOOL)finished error:(NSError **)error {
    @synchronized(self) {
        NSDictionary *prior = _active[url.path];
        if (!prior) return StoreError(error, 4, @"この測定の結果は変更できません。");
        if (![snapshot isKindOfClass:NSDictionary.class] ||
            (finished && !IsTerminal(snapshot[@"status"])))
            return StoreError(error, 3, @"測定結果の状態が正しくありません。");
        NSMutableDictionary *record = [prior mutableCopy];
        record[@"comparison"] = snapshot;
        record[@"finished"] = @(finished);
        record[@"status"] = finished ? snapshot[@"status"] : @"running";
        record[@"updated_at"] = Now();
        NSData *data = Encode(record, error);
        if (!data || ![self saveData:data toURL:url initial:NO error:error]) return NO;
        // Keep an immutable deep copy; callers may reuse mutable progress objects.
        NSDictionary *frozen = [NSJSONSerialization JSONObjectWithData:data options:0 error:error];
        if (!frozen) return NO;
        if (finished) [_active removeObjectForKey:url.path]; else _active[url.path] = frozen;
        _latestReportURL = url;
        return YES;
    }
}

- (nullable NSDictionary *)readRecordAtURL:(NSURL *)url error:(NSError **)error {
    @synchronized(self) {
        NSString *run = nil, *file = nil;
        if (![self partsForURL:url run:&run file:&file error:error]) return nil;
        int directory = openat(_rootFD, run.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory < 0) { IOError(error, @"結果のフォルダを開けませんでした。"); return nil; }
        int fd = openat(directory, file.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        close(directory);
        if (fd < 0) { IOError(error, @"結果ファイルを開けませんでした。"); return nil; }
        struct stat st{};
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size <= 0 ||
            (uint64_t)st.st_size > kRecordLimit) {
            close(fd); StoreError(error, 2, @"結果ファイルを読み取れません。"); return nil;
        }
        NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)st.st_size];
        NSUInteger received = 0;
        while (received < data.length) {
            ssize_t count = read(fd, (uint8_t *)data.mutableBytes + received, data.length - received);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { close(fd); StoreError(error, 2, @"結果ファイルが途中で変わりました。"); return nil; }
            received += (NSUInteger)count;
        }
        close(fd);
        id record = [NSJSONSerialization JSONObjectWithData:data options:0 error:error];
        if (![record isKindOfClass:NSDictionary.class] || ![record[@"schema"] isEqual:@1] ||
            ![record[@"scope"] isEqual:kRecordScope] || ![record[@"run_id"] isEqual:run] ||
            ![record[@"created_at"] isKindOfClass:NSString.class] ||
            ![record[@"updated_at"] isKindOfClass:NSString.class] ||
            ![record[@"comparison"] isKindOfClass:NSDictionary.class] ||
            ![record[@"finished"] isKindOfClass:NSNumber.class] ||
            ([record[@"finished"] boolValue] ? (!IsTerminal(record[@"status"]) ||
               ![record[@"status"] isEqual:record[@"comparison"][@"status"]]) : ![record[@"status"] isEqual:@"running"])) {
            StoreError(error, 3, @"結果ファイルの形式が正しくありません。"); return nil;
        }
        if (![record[@"finished"] boolValue] && !_active[url.path]) {
            NSMutableDictionary *recovered = [record mutableCopy];
            recovered[@"status"] = @"interrupted";
            recovered[@"recovered_incomplete"] = @YES;
            return [recovered copy];
        }
        return record;
    }
}
@end
