#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <AVFoundation/AVFoundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <TargetConditionals.h>
#import <os/proc.h>
#import <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#import <simd/simd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <cmath>
#include <atomic>
#include <memory>
#include <new>
#include "guest_memory.h"

static const uint64_t MaxFile = 256ULL * 1024 * 1024;
static const uint64_t MaxImport = 512ULL * 1024 * 1024;
static const unsigned MaxEntries = 4096;
static const size_t CopyBufferSize = 64 * 1024;
struct CopyBudget {
    uint64_t bytes = 0;
    unsigned entries = 0;
    std::atomic_bool *cancel = nullptr;
    // Exactly one heap buffer per import, shared by every recursion level.
    std::unique_ptr<unsigned char[]> buffer{new (std::nothrow) unsigned char[CopyBufferSize]};
};
static NSString *PosixFailure(NSString *stage) { return [NSString stringWithFormat:@"%@: errno %d", stage, errno]; }

// Build provenance, not a device/user identifier. Read the actual loaded main image.
static NSString *MainExecutableUUID(void) {
    const struct mach_header *header = _dyld_get_image_header(0);
    if (!header || header->magic != MH_MAGIC_64) return @"unavailable (not Mach-O 64)";
    const struct mach_header_64 *header64 = (const struct mach_header_64 *)header;
    size_t length = header64->sizeofcmds;
    if (length > 1024 * 1024 || header64->ncmds > length / sizeof(struct load_command)) return @"unavailable (invalid command bounds)";
    const unsigned char *commands = (const unsigned char *)(header64 + 1);
    size_t offset = 0;
    for (uint32_t i = 0; i < header64->ncmds; ++i) {
        if (offset > length || length - offset < sizeof(struct load_command)) return @"unavailable (truncated command)";
        const struct load_command *command = (const struct load_command *)(commands + offset);
        if (command->cmdsize < sizeof(*command) || command->cmdsize > length - offset || command->cmdsize % 8) return @"unavailable (invalid command size)";
        if (command->cmd == LC_UUID) {
            if (command->cmdsize < sizeof(struct uuid_command)) return @"unavailable (truncated UUID)";
            const struct uuid_command *uuidCommand = (const struct uuid_command *)command;
            return [[NSUUID alloc] initWithUUIDBytes:uuidCommand->uuid].UUIDString;
        }
        offset += command->cmdsize;
    }
    return @"unavailable (LC_UUID absent)";
}

// openat + O_NOFOLLOW on every child prevents symlink traversal, including races
// between enumeration and opening. Destinations are exclusively created under a UUID.
static bool CopyFD(int source, int destination, unsigned depth, CopyBudget &budget, NSString **error) {
    if (!budget.buffer) { *error = @"Import buffer allocation failed."; return false; }
    if (budget.cancel && budget.cancel->load()) { *error = @"Import cancelled when background time expired."; return false; }
    struct stat info;
    if (fstat(source, &info) != 0) { *error = PosixFailure(@"stat"); return false; }
    if (S_ISREG(info.st_mode)) {
        if (info.st_size < 0 || (uint64_t)info.st_size > MaxFile) { *error = @"Unsupported: file exceeds 256 MiB."; return false; }
        if ((uint64_t)info.st_size > MaxImport - budget.bytes) { *error = @"Unsupported: import exceeds 512 MiB."; return false; }
        uint64_t fileBytes = 0;
        for (;;) {
            if (budget.cancel && budget.cancel->load()) { *error = @"Import cancelled when background time expired."; return false; }
            ssize_t count = read(source, budget.buffer.get(), CopyBufferSize);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { *error = PosixFailure(@"read"); return false; }
            if (!count) break;
            fileBytes += (uint64_t)count; budget.bytes += (uint64_t)count;
            if (fileBytes > MaxFile || budget.bytes > MaxImport) { *error = @"Unsupported: copy exceeds 256 MiB/file or 512 MiB total."; return false; }
            ssize_t done = 0;
            while (done < count) {
                ssize_t n = write(destination, budget.buffer.get() + done, (size_t)(count - done));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { *error = PosixFailure(@"write"); return false; }
                done += n;
            }
        }
        if (fsync(destination) != 0) { *error = PosixFailure(@"sync"); return false; }
        return true;
    }
    if (!S_ISDIR(info.st_mode) || depth >= 16) { *error = @"Unsupported: special file or directory depth exceeds 16."; return false; }
    int duplicate = dup(source);
    DIR *dir = duplicate < 0 ? NULL : fdopendir(duplicate);
    if (!dir) { if (duplicate >= 0) close(duplicate); *error = PosixFailure(@"directory"); return false; }
    bool ok = true;
    for (;;) {
        if (budget.cancel && budget.cancel->load()) { *error = @"Import cancelled when background time expired."; ok = false; break; }
        errno = 0; struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) { *error = PosixFailure(@"enumerate"); ok = false; } break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++budget.entries > MaxEntries) { *error = @"Unsupported: more than 4096 entries."; ok = false; break; }
        // O_NONBLOCK ensures a malicious FIFO cannot block before fstat rejects it.
        int child = openat(source, entry->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat childInfo;
        if (child < 0 || fstat(child, &childInfo)) { if (child >= 0) close(child); *error = PosixFailure(@"open child (symlinks rejected)"); ok = false; break; }
        bool directory = S_ISDIR(childInfo.st_mode);
        if (!directory && !S_ISREG(childInfo.st_mode)) { close(child); *error = @"Unsupported: symlink or special file."; ok = false; break; }
        if (directory && mkdirat(destination, entry->d_name, 0700)) { close(child); *error = PosixFailure(@"create directory"); ok = false; break; }
        int output = openat(destination, entry->d_name, directory ? (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : (O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC), 0600);
        if (output < 0) { close(child); *error = PosixFailure(@"create exclusive copy"); ok = false; break; }
        ok = CopyFD(child, output, depth + 1, budget, error);
        close(output); close(child); if (!ok) break;
    }
    closedir(dir); return ok;
}

@interface ProbeController : UIViewController <MTKViewDelegate, UIDocumentPickerDelegate>
@property(nonatomic, strong) MTKView *metalView;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipeline;
@property(nonatomic, strong) UITextView *output;
@property(nonatomic, strong) UILabel *stateLabel;
@property(nonatomic, strong) NSURL *rootURL;
@property(nonatomic, strong) NSURL *logURL;
@property(nonatomic, strong) NSURL *previousLogURL;
@property(nonatomic, strong) AVAudioEngine *audioEngine;
@property(nonatomic, strong) AVAudioPlayerNode *audioPlayer;
@property(nonatomic) float moveX;
@property(nonatomic) float moveY;
@property(nonatomic) float yaw;
@property(nonatomic) BOOL importBusy;
@property(nonatomic) BOOL panMoves;
@end

@implementation ProbeController
- (void)log:(NSString *)message {
    NSAssert([NSThread isMainThread], @"Log on main thread");
    NSString *line = [NSString stringWithFormat:@"%@ %@\n", [NSDate date].description, message];
    NSError *error = nil;
    NSFileHandle *handle = [NSFileHandle fileHandleForWritingToURL:self.logURL error:&error];
    if (handle) {
        [handle seekToEndReturningOffset:NULL error:&error];
        if (!error) [handle writeData:[line dataUsingEncoding:NSUTF8StringEncoding] error:&error];
        if (!error) [handle synchronizeAndReturnError:&error];
        [handle closeAndReturnError:NULL];
    }
    NSString *text = [(self.output.text ?: @"") stringByAppendingString:line];
    if (error || !handle) text = [text stringByAppendingString:@"LOG PERSISTENCE FAILED.\n"];
    if (text.length > 24000) text = [text substringFromIndex:text.length - 24000];
    self.output.text = text;
    if (text.length) [self.output scrollRangeToVisible:NSMakeRange(text.length - 1, 1)];
}
- (NSURL *)path:(NSString *)name { return [self.rootURL URLByAppendingPathComponent:name]; }
- (void)marker:(NSString *)state {
    NSDictionary *record = @{@"state":state, @"log":self.logURL.lastPathComponent, @"time":[NSDate date].description};
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:record options:0 error:&error];
    if (!data || ![data writeToURL:[self path:@"run-marker.json"] options:NSDataWritingAtomic error:&error]) [self log:@"Run marker write failed."];
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor = UIColor.systemBackgroundColor;
    NSURL *support = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    self.rootURL = [support URLByAppendingPathComponent:@"SkyrimProbe" isDirectory:YES];
    NSError *setupError = nil;
    [[NSFileManager defaultManager] createDirectoryAtURL:self.rootURL withIntermediateDirectories:YES attributes:nil error:&setupError];
    self.logURL = [self path:[NSString stringWithFormat:@"session-%@.log", NSUUID.UUID.UUIDString]];
    [[NSData data] writeToURL:self.logURL options:NSDataWritingWithoutOverwriting error:&setupError];
    self.output = [UITextView new]; self.output.editable = NO; self.output.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    self.output.accessibilityIdentifier = @"probe.log";
    UILabel *title = [UILabel new]; title.text = @"Skyrim iOS feasibility probe"; title.font = [UIFont boldSystemFontOfSize:21]; title.adjustsFontSizeToFitWidth = YES;
    UILabel *disclaimer = [UILabel new]; disclaimer.text = @"DIAGNOSTIC ONLY • No Skyrim runtime/gameplay\nDrag left: move triangle • Drag right: rotate view state"; disclaimer.numberOfLines = 0; disclaimer.font = [UIFont systemFontOfSize:12];
    self.stateLabel = [UILabel new]; self.stateLabel.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    self.stateLabel.accessibilityIdentifier = @"probe.state";
    self.metalView = [[MTKView alloc] initWithFrame:CGRectZero device:MTLCreateSystemDefaultDevice()];
    self.metalView.isAccessibilityElement = YES; self.metalView.accessibilityIdentifier = @"probe.viewport"; self.metalView.accessibilityLabel = @"Metal probe viewport";
    self.metalView.clearColor = MTLClearColorMake(0.025, 0.04, 0.08, 1); self.metalView.delegate = self;
    self.metalView.preferredFramesPerSecond = 60;
    [self.metalView.heightAnchor constraintEqualToConstant:190].active = YES;
    [self.metalView addGestureRecognizer:[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(pan:)]];
    UIStackView *first = [self buttons:@[@"4 GiB VA", @"Tone", @"Save", @"Load"] actions:@[NSStringFromSelector(@selector(memoryProbe)), NSStringFromSelector(@selector(tone)), NSStringFromSelector(@selector(save)), NSStringFromSelector(@selector(load))]];
    UIStackView *second = [self buttons:@[@"Import file", @"Import folder", @"Share logs"] actions:@[NSStringFromSelector(@selector(importFile)), NSStringFromSelector(@selector(importFolder)), NSStringFromSelector(@selector(share:))]];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, disclaimer, self.metalView, self.stateLabel, first, second, self.output]];
    stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 8; stack.translatesAutoresizingMaskIntoConstraints = NO; [self.view addSubview:stack];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[[stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8], [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-8], [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:12], [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-12]]];
    if (setupError) [self log:@"FAILED: application support/log creation."];
    NSData *prior = [self smallData:[self path:@"run-marker.json"] limit:65536];
    id previous = prior ? [NSJSONSerialization JSONObjectWithData:prior options:0 error:NULL] : nil;
    if ([previous isKindOfClass:NSDictionary.class]) {
        NSString *name = previous[@"log"];
        if ([name isKindOfClass:NSString.class] && [name hasPrefix:@"session-"] && [name hasSuffix:@".log"] && [name.lastPathComponent isEqualToString:name]) self.previousLogURL = [self path:name];
        [self log:[NSString stringWithFormat:@"Previous run marker: %@. An active marker is possible interruption/termination, NOT proof of a crash.", previous[@"state"]]];
    }
    [self marker:@"active"]; [self setupMetal]; [self refreshState]; [self info];
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:self selector:@selector(background:) name:UIApplicationDidEnterBackgroundNotification object:nil];
    [nc addObserver:self selector:@selector(foreground:) name:UIApplicationDidBecomeActiveNotification object:nil];
    [nc addObserver:self selector:@selector(inactive:) name:UIApplicationWillResignActiveNotification object:nil];
    [nc addObserver:self selector:@selector(terminate:) name:UIApplicationWillTerminateNotification object:nil];
    [nc addObserver:self selector:@selector(interruption:) name:AVAudioSessionInterruptionNotification object:nil];
    [nc addObserver:self selector:@selector(audioReset:) name:AVAudioSessionMediaServicesWereResetNotification object:nil];
    [nc addObserver:self selector:@selector(thermal:) name:NSProcessInfoThermalStateDidChangeNotification object:nil];
}
- (UIStackView *)buttons:(NSArray<NSString *> *)names actions:(NSArray<NSString *> *)actions {
    NSMutableArray *buttons = [NSMutableArray array];
    for (NSUInteger i = 0; i < names.count; ++i) { UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem]; [b setTitle:names[i] forState:UIControlStateNormal]; b.accessibilityIdentifier = [@"probe.button." stringByAppendingString:actions[i]]; b.titleLabel.font = [UIFont systemFontOfSize:13]; [b addTarget:self action:NSSelectorFromString(actions[i]) forControlEvents:UIControlEventTouchUpInside]; [buttons addObject:b]; }
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:buttons]; row.distribution = UIStackViewDistributionFillEqually; return row;
}
- (void)setupMetal {
    if (!self.metalView.device) { [self log:@"FAILED: Metal device unavailable."]; return; }
    self.queue = [self.metalView.device newCommandQueue];
    id<MTLLibrary> library = [self.metalView.device newDefaultLibrary];
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = [library newFunctionWithName:@"probe_vertex"]; d.fragmentFunction = [library newFunctionWithName:@"probe_fragment"]; d.colorAttachments[0].pixelFormat = self.metalView.colorPixelFormat;
    NSError *error = nil;
    if (d.vertexFunction && d.fragmentFunction) self.pipeline = [self.metalView.device newRenderPipelineStateWithDescriptor:d error:&error];
    [self log:self.pipeline && self.queue ? @"Metal pipeline ready; triangle is probe geometry only." : [NSString stringWithFormat:@"FAILED: Metal pipeline: %@", error.localizedDescription ?: @"missing compiled shaders/queue"]];
}
- (void)drawInMTKView:(MTKView *)view {
    if (!self.pipeline || !self.queue || !view.currentRenderPassDescriptor) return;
    id<CAMetalDrawable> drawable = view.currentDrawable; if (!drawable) return;
    id<MTLCommandBuffer> command = [self.queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:view.currentRenderPassDescriptor];
    [encoder setRenderPipelineState:self.pipeline];
    vector_float4 state = {self.moveX, self.moveY, self.yaw, (float)(view.drawableSize.width / MAX(view.drawableSize.height, 1))};
    [encoder setVertexBytes:&state length:sizeof(state) atIndex:0]; [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [encoder endEncoding];
    [command addCompletedHandler:^(id<MTLCommandBuffer> buffer) { if (buffer.status == MTLCommandBufferStatusError) dispatch_async(dispatch_get_main_queue(), ^{ self.metalView.paused = YES; [self log:[NSString stringWithFormat:@"FAILED: GPU command: %@", buffer.error.localizedDescription]]; }); }];
    [command presentDrawable:drawable]; [command commit];
}
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size { (void)view; (void)size; }
- (void)pan:(UIPanGestureRecognizer *)gesture {
    if (gesture.state == UIGestureRecognizerStateBegan) self.panMoves = [gesture locationInView:self.metalView].x < self.metalView.bounds.size.width / 2;
    CGPoint delta = [gesture translationInView:self.metalView]; [gesture setTranslation:CGPointZero inView:self.metalView];
    if (self.panMoves) { self.moveX = fmaxf(-0.8f, fminf(0.8f, self.moveX + delta.x / 200)); self.moveY = fmaxf(-0.8f, fminf(0.8f, self.moveY - delta.y / 200)); }
    else self.yaw = remainderf(self.yaw + delta.x / 100, 6.2831853f);
    [self refreshState];
}
- (void)refreshState { self.stateLabel.text = [NSString stringWithFormat:@"Probe state x %.2f  y %.2f  yaw %.2f", self.moveX, self.moveY, self.yaw]; }
- (void)info {
    [self log:[NSString stringWithFormat:@"Main executable Mach-O UUID=%@", MainExecutableUUID()]];
    task_vm_info_data_t vm = {}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count);
    struct utsname hardware = {}; BOOL hardwareOK = uname(&hardware) == 0;
    NSDictionary *bundle = NSBundle.mainBundle.infoDictionary;
    [self log:[NSString stringWithFormat:@"app %@ %@ (%@); %@ %@; hardware=%s; target=%@; thermal=%ld; available=%llu bytes; footprint=%@", NSBundle.mainBundle.bundleIdentifier, bundle[@"CFBundleShortVersionString"], bundle[@"CFBundleVersion"], UIDevice.currentDevice.model, UIDevice.currentDevice.systemVersion, hardwareOK ? hardware.machine : "unavailable",
#if TARGET_OS_SIMULATOR
        @"SIMULATOR (not device evidence)",
#else
        @"iPhoneOS ARM64",
#endif
        (long)NSProcessInfo.processInfo.thermalState, (unsigned long long)os_proc_available_memory(), kr == KERN_SUCCESS ? [NSString stringWithFormat:@"%llu bytes", (unsigned long long)vm.phys_footprint] : @"unavailable"]];
}
- (void)memoryProbe {
    [self info]; gm_report r = {}; int ok = gm_run_probe(&r);
    [self log:[NSString stringWithFormat:@"VA %@: %s; OS error=%d; reserved=%llu bytes; page=%zu; touched=%llu bytes; mapping released unless release failed. No executable memory/JIT.", ok ? @"PASS" : @"FAIL", r.stage, r.os_error, (unsigned long long)r.reserved_bytes, r.page_size, (unsigned long long)r.touched_bytes]]; [self info];
}
- (NSData *)smallData:(NSURL *)url limit:(NSUInteger)limit {
    NSNumber *size = nil; [url getResourceValue:&size forKey:NSURLFileSizeKey error:NULL];
    if (!size || size.unsignedLongLongValue > limit) return nil;
    // App-owned small records only, never imported game data.
    return [NSData dataWithContentsOfURL:url options:0 error:NULL];
}
- (void)save {
    NSDictionary *state = @{@"kind":@"SkyrimProbeState", @"version":@1, @"x":@(self.moveX), @"y":@(self.moveY), @"yaw":@(self.yaw)};
    NSError *error = nil; NSData *data = [NSJSONSerialization dataWithJSONObject:state options:0 error:&error];
    BOOL ok = data && [data writeToURL:[self path:@"probe-state.json"] options:NSDataWritingAtomic error:&error];
    [self log:ok ? @"Saved probe state atomically (not a Skyrim save)." : @"FAILED: probe state save."];
}
- (void)load {
    NSData *data = [self smallData:[self path:@"probe-state.json"] limit:65536];
    id value = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    if (![value isKindOfClass:NSDictionary.class] || ![value[@"kind"] isEqual:@"SkyrimProbeState"] || ![value[@"version"] isEqual:@1] || ![value[@"x"] isKindOfClass:NSNumber.class] || ![value[@"y"] isKindOfClass:NSNumber.class] || ![value[@"yaw"] isKindOfClass:NSNumber.class]) { [self log:@"FAILED: missing/invalid probe state (Skyrim saves unsupported)."]; return; }
    float x = [value[@"x"] floatValue], y = [value[@"y"] floatValue], yaw = [value[@"yaw"] floatValue];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(yaw) || fabsf(x) > 0.8f || fabsf(y) > 0.8f || fabsf(yaw) > 3.142f) { [self log:@"FAILED: probe state bounds."]; return; }
    self.moveX = x; self.moveY = y; self.yaw = yaw; [self refreshState]; [self log:@"Loaded probe state."];
}
- (void)stopAudio { [self.audioPlayer stop]; [self.audioEngine stop]; [AVAudioSession.sharedInstance setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:NULL]; }
- (void)tone {
    if (UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    NSError *error = nil; AVAudioSession *session = AVAudioSession.sharedInstance;
    if (![session setCategory:AVAudioSessionCategoryAmbient error:&error] || ![session setActive:YES error:&error]) { [self log:[NSString stringWithFormat:@"FAILED: audio session %@", error.localizedDescription]]; return; }
    if (!self.audioEngine) {
        self.audioEngine = [AVAudioEngine new]; self.audioPlayer = [AVAudioPlayerNode new]; [self.audioEngine attachNode:self.audioPlayer];
        AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:1]; [self.audioEngine connect:self.audioPlayer to:self.audioEngine.mainMixerNode format:format];
    }
    if (!self.audioEngine.running && ![self.audioEngine startAndReturnError:&error]) { [self log:[NSString stringWithFormat:@"FAILED: audio engine %@", error.localizedDescription]]; return; }
    [self.audioPlayer stop];
    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:[self.audioPlayer outputFormatForBus:0] frameCapacity:12000]; buffer.frameLength = 12000;
    for (unsigned i = 0; i < buffer.frameLength; ++i) { float envelope = fminf(1, fminf(i / 480.0f, (11999 - i) / 480.0f)); buffer.floatChannelData[0][i] = 0.15f * envelope * sinf(6.2831853f * 440 * i / 48000); }
    [self.audioPlayer scheduleBuffer:buffer completionHandler:nil]; [self.audioPlayer play]; [self log:@"Scheduled 250 ms 440 Hz tone; ambient category respects silent switch. Confirm audibility on device."];
}
- (void)choose:(UTType *)type {
    if (self.importBusy) { [self log:@"Import already in progress."]; return; }
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[type] asCopy:NO]; picker.delegate = self; picker.allowsMultipleSelection = NO; [self presentViewController:picker animated:YES completion:nil];
}
- (void)importFile { [self choose:UTTypeItem]; }
- (void)importFolder { [self choose:UTTypeFolder]; }
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller { (void)controller; [self log:@"Import cancelled."]; }
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller; if (!urls.count || self.importBusy) return;
    NSURL *url = urls.firstObject; self.importBusy = YES; [self log:@"Copying selected input. Limits: 256 MiB/file, 512 MiB total, 4096 entries, depth 16. No game execution."];
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
    task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"ProbeImport" expirationHandler:^{
        cancellation->store(true);
        if (task != UIBackgroundTaskInvalid) { [UIApplication.sharedApplication endBackgroundTask:task]; task = UIBackgroundTaskInvalid; }
    }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        BOOL scoped = [url startAccessingSecurityScopedResource];
        NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        __block NSString *failure = nil; __block uint64_t copied = 0; __block BOOL success = NO;
        __block NSURL *destination = nil; NSError *coordError = nil;
        [coordinator coordinateReadingItemAtURL:url options:0 error:&coordError byAccessor:^(NSURL *readURL) {
            destination = [self path:[NSString stringWithFormat:@"import-%@", NSUUID.UUID.UUIDString]];
            NSError *error = nil;
            if (![[NSFileManager defaultManager] createDirectoryAtURL:destination withIntermediateDirectories:NO attributes:nil error:&error]) { failure = @"Cannot create unique import directory."; return; }
            [@"Incomplete import. Not usable.\n" writeToURL:[destination URLByAppendingPathComponent:@"pending.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
            int source = open(readURL.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC); struct stat info;
            if (source < 0 || fstat(source, &info)) { if (source >= 0) close(source); failure = PosixFailure(@"open selection"); return; }
            BOOL directory = S_ISDIR(info.st_mode);
            if (!directory && !S_ISREG(info.st_mode)) { close(source); failure = @"Unsupported: symlink or special file."; return; }
            NSURL *payload = [destination URLByAppendingPathComponent:@"payload"];
            if (directory && mkdir(payload.fileSystemRepresentation, 0700)) { close(source); failure = PosixFailure(@"mkdir payload"); return; }
            int output = open(payload.fileSystemRepresentation, directory ? (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : (O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC), 0600);
            if (output < 0) { close(source); failure = PosixFailure(@"create payload"); return; }
            CopyBudget budget; budget.cancel = cancellation.get(); success = CopyFD(source, output, 0, budget, &failure); copied = budget.bytes; close(output); close(source);
            if (success) {
                // Preserve pending evidence; complete.txt is the explicit successful-copy marker.
                success = [@"Bounded copy completed. Runtime unsupported; not executed.\n" writeToURL:[destination URLByAppendingPathComponent:@"complete.txt"] atomically:YES encoding:NSUTF8StringEncoding error:&error];
                if (!success) failure = @"Completion record could not be written.";
            }
        }];
        if (scoped) [url stopAccessingSecurityScopedResource];
        if (coordError) failure = @"Files provider coordination failed.";
        dispatch_async(dispatch_get_main_queue(), ^{
            self.importBusy = NO;
            [self log:success && !coordError ? [NSString stringWithFormat:@"Imported %llu bytes into %@. Original untouched. Skyrim runtime remains UNSUPPORTED.", (unsigned long long)copied, destination.lastPathComponent] : [NSString stringWithFormat:@"IMPORT FAILED: %@ Partial copy retained, not usable.", failure ?: @"unknown error"]];
            if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task];
        });
    });
}
- (void)share:(UIButton *)sender {
    [self info]; [self log:@"Sharing current and available previous session logs (no source files/device identifiers)."];
    NSMutableArray *items = [NSMutableArray arrayWithObject:self.logURL];
    if (self.previousLogURL && [[NSFileManager defaultManager] fileExistsAtPath:self.previousLogURL.path]) [items addObject:self.previousLogURL];
    UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:items applicationActivities:nil]; share.popoverPresentationController.sourceView = sender; [self presentViewController:share animated:YES completion:nil];
}
- (void)inactive:(NSNotification *)notification { (void)notification; self.metalView.paused = YES; [self stopAudio]; [self marker:@"inactive"]; [self log:@"Inactive: renderer/audio paused."]; }
- (void)background:(NSNotification *)notification { (void)notification; [self save]; [self marker:@"background"]; [self log:@"Background: state saved. OS may terminate without callback."]; }
- (void)foreground:(NSNotification *)notification { (void)notification; self.metalView.paused = NO; [self marker:@"active"]; [self log:@"Active: renderer resumed; tone requires tap."]; [self info]; }
- (void)terminate:(NSNotification *)notification { (void)notification; [self marker:@"termination-callback"]; }
- (void)interruption:(NSNotification *)notification {
    dispatch_async(dispatch_get_main_queue(), ^{ NSUInteger type = [notification.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue]; [self stopAudio]; [self log:type == AVAudioSessionInterruptionTypeBegan ? @"Audio interrupted; stopped." : @"Audio interruption ended; tap Tone to resume."]; });
}
- (void)audioReset:(NSNotification *)notification { (void)notification; dispatch_async(dispatch_get_main_queue(), ^{ [self stopAudio]; self.audioEngine = nil; self.audioPlayer = nil; [self log:@"Audio media services reset; next Tone recreates engine."]; }); }
- (void)thermal:(NSNotification *)notification { (void)notification; dispatch_async(dispatch_get_main_queue(), ^{ [self info]; }); }
- (void)didReceiveMemoryWarning { [super didReceiveMemoryWarning]; [self log:@"MEMORY WARNING received."]; [self info]; }
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options; self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds]; self.window.rootViewController = [ProbeController new]; [self.window makeKeyAndVisible]; return YES;
}
@end
int main(int argc, char *argv[]) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class)); } }
