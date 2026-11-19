/* SPDX-License-Identifier: GPL-3.0-only */
#import "classic_probe_ui.h"
#include "classic_plugin_probe.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *text(NSString *en, NSString *ja) {
    NSDictionary *settings = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"mobile-settings-v1"];
    NSNumber *language = settings[@"language"];
    BOOL japanese = [language isKindOfClass:NSNumber.class] ? language.intValue == 1 :
                    [NSLocale.preferredLanguages.firstObject hasPrefix:@"ja"];
    return japanese ? ja : en;
}

/* Both identities come from independent runs of the authored Windows/AOT sample.
 * The UI accepts only the measured input and shares only the measured output. */
enum { FixtureBytes = 83 };
static NSString *const previousResultKey = @"classic-lab-windows-roundtrip-result-v1";
static const unsigned char inputDigest[CC_SHA256_DIGEST_LENGTH] = {
    0x54,0xaf,0x4d,0xbf,0xbd,0xbb,0x19,0xa7,0xf8,0xb0,0x51,0x6e,0x5b,0x97,0xf7,0x2e,
    0xb1,0xa7,0xda,0xb6,0x86,0x48,0x4b,0x56,0x22,0x1d,0xce,0x3e,0xfe,0x9c,0x8a,0x36
};
static const unsigned char outputDigest[CC_SHA256_DIGEST_LENGTH] = {
    0xe0,0x3a,0xad,0xa2,0xcf,0x67,0xb7,0x14,0xb2,0x9a,0x0c,0x24,0x98,0x9d,0x21,0x58,
    0x79,0xaa,0xd6,0x87,0x63,0xac,0x45,0x4f,0xc1,0x3b,0xc0,0x28,0xc1,0x50,0x54,0x9e
};
static BOOL matches(NSData *data, const unsigned char *expected) {
    if (data.length != FixtureBytes) return NO;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    return !memcmp(digest, expected, sizeof digest);
}
static NSURL *scratchFolder(void) {
    NSURL *base = [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];
    NSURL *folder = [base URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    return [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:NO
                                                  attributes:nil error:NULL] ? folder : nil;
}

static NSString *hexDigest(const unsigned char *bytes) {
    NSMutableString *hex = [NSMutableString stringWithCapacity:64];
    for (NSUInteger i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) [hex appendFormat:@"%02x", bytes[i]];
    return hex;
}
static NSData *executionReport(SkyrimClassicFixtureResult result) {
#if TARGET_OS_SIMULATOR
    NSString *platform = @"ios_simulator";
#else
    NSString *platform = @"ios_device";
#endif
    NSBundle *bundle = NSBundle.mainBundle;
    NSDictionary *record = @{
        @"schema":@1, @"run_id":NSUUID.UUID.UUIDString, @"platform":platform,
        @"completed_at":[[NSISO8601DateFormatter new] stringFromDate:NSDate.date],
        @"os_version":UIDevice.currentDevice.systemVersion, @"bundle_id":bundle.bundleIdentifier ?: @"",
        @"build":[bundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"",
        @"input_value":@(result.input_value), @"first_save_after_revert":@(result.reverted_value),
        @"output_value":@(result.output_value), @"records_read":@(result.records_read), @"callback_mask":@(result.callback_mask),
        @"input_sha256":hexDigest(inputDigest), @"output_sha256":hexDigest(outputDigest)
    };
    return [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingPrettyPrinted error:NULL];
}

static BOOL savedReportIsValid(NSData *data) {
    if (![data isKindOfClass:NSData.class] || !data.length || data.length > 4096) return NO;
    NSDictionary *record = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (![record isKindOfClass:NSDictionary.class]) return NO;
    return [record[@"schema"] isEqual:@1] && [record[@"bundle_id"] isEqual:NSBundle.mainBundle.bundleIdentifier] &&
        [record[@"input_value"] isEqual:@16] && [record[@"first_save_after_revert"] isEqual:@5] &&
        [record[@"output_value"] isEqual:@21] && [record[@"records_read"] isEqual:@1] && [record[@"callback_mask"] isEqual:@7] &&
        [record[@"input_sha256"] isEqual:hexDigest(inputDigest)] && [record[@"output_sha256"] isEqual:hexDigest(outputDigest)];
}

/* Reading at most 84 bytes bounds provider input even if its reported size
 * changes. A large save is rejected from its metadata before reading payload. */
static NSData *sampleAt(NSURL *url, const unsigned char *expected, SkyrimClassicFixtureStatus mismatch,
                        SkyrimClassicFixtureStatus *status) {
    if (!url.isFileURL || [url.pathExtension caseInsensitiveCompare:@"skse"] != NSOrderedSame) {
        *status = mismatch; return nil;
    }
    int fd = open(url.fileSystemRepresentation, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { *status = SKYRIM_CLASSIC_FIXTURE_IO; return nil; }
    struct stat st;
    unsigned char bytes[FixtureBytes + 1];
    size_t used = 0;
    *status = mismatch;
    if (fstat(fd, &st)) *status = SKYRIM_CLASSIC_FIXTURE_IO;
    else if (S_ISREG(st.st_mode) && st.st_size == FixtureBytes) {
        while (used < sizeof bytes) {
            ssize_t n = read(fd, bytes + used, sizeof bytes - used);
            if (n > 0) used += (size_t)n;
            else if (!n) break;
            else if (errno != EINTR) { *status = SKYRIM_CLASSIC_FIXTURE_IO; break; }
        }
    }
    close(fd);
    if (used != FixtureBytes || *status == SKYRIM_CLASSIC_FIXTURE_IO) return nil;
    NSData *data = [NSData dataWithBytes:bytes length:used];
    if (!matches(data, expected)) return nil;
    *status = SKYRIM_CLASSIC_FIXTURE_OK;
    return data;
}

static NSData *runSelected(NSURL *url, SkyrimClassicFixtureStatus *status, SkyrimClassicFixtureResult *result) {
    if (!url.isFileURL || [url.pathExtension caseInsensitiveCompare:@"skse"] != NSOrderedSame) {
        *status = SKYRIM_CLASSIC_FIXTURE_INPUT; return nil;
    }
    BOOL access = [url startAccessingSecurityScopedResource];
    NSNumber *reportedSize = nil;
    if ([url getResourceValue:&reportedSize forKey:NSURLFileSizeKey error:NULL] &&
        [reportedSize isKindOfClass:NSNumber.class] && reportedSize.unsignedLongLongValue != FixtureBytes) {
        if (access) [url stopAccessingSecurityScopedResource];
        *status = SKYRIM_CLASSIC_FIXTURE_INPUT; return nil;
    }
    __block NSData *input = nil;
    __block SkyrimClassicFixtureStatus selected = SKYRIM_CLASSIC_FIXTURE_IO;
    NSError *error = nil;
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateReadingItemAtURL:url options:NSFileCoordinatorReadingWithoutChanges error:&error
                                 byAccessor:^(NSURL *item) {
        input = sampleAt(item, inputDigest, SKYRIM_CLASSIC_FIXTURE_INPUT, &selected);
    }];
    if (access) [url stopAccessingSecurityScopedResource];
    if (error || !input) { *status = error ? SKYRIM_CLASSIC_FIXTURE_IO : selected; return nil; }
    NSURL *folder = scratchFolder();
    if (!folder) { *status = SKYRIM_CLASSIC_FIXTURE_IO; return nil; }
    NSURL *source = [folder URLByAppendingPathComponent:@"windows.skse"];
    NSURL *target = [folder URLByAppendingPathComponent:@"translated.skse"];
    NSData *answer = nil;
    *status = SKYRIM_CLASSIC_FIXTURE_IO;
    if ([input writeToURL:source options:NSDataWritingWithoutOverwriting error:NULL]) {
        *status = skyrim_classic_fixture_run(source.fileSystemRepresentation, target.fileSystemRepresentation, result);
        if (*status == SKYRIM_CLASSIC_FIXTURE_OK)
            answer = sampleAt(target, outputDigest, SKYRIM_CLASSIC_FIXTURE_OUTPUT, status);
    }
    [NSFileManager.defaultManager removeItemAtURL:folder error:NULL];
    return answer;
}

static NSString *failure(SkyrimClassicFixtureStatus status) {
    switch (status) {
    case SKYRIM_CLASSIC_FIXTURE_INPUT: case SKYRIM_CLASSIC_FIXTURE_ARGUMENT:
        return text(@"Choose the original windows.skse supplied for this test.", @"この検査用にWindowsで作成した元の windows.skse を選んでください。");
    case SKYRIM_CLASSIC_FIXTURE_BUSY:
        return text(@"A test is already running. Try again after it finishes.", @"別の検査が実行中です。終了後にもう一度選んでください。");
    case SKYRIM_CLASSIC_FIXTURE_EXISTS:
        return text(@"The output file already exists. Choose the Windows sample to start a fresh run.",
                    @"出力先にファイルがあります。Windowsのサンプルを選び直して、新しい検査を開始してください。");
    case SKYRIM_CLASSIC_FIXTURE_MEMORY:
        return text(@"The test could not allocate enough memory.", @"検査に必要なメモリを確保できませんでした。");
    case SKYRIM_CLASSIC_FIXTURE_CALLBACK:
        return text(@"The test DLL reported an unexpected callback result.", @"テストDLLの読み込み・保存処理が予想と異なる結果を返しました。");
    case SKYRIM_CLASSIC_FIXTURE_OUTPUT:
        return text(@"The output differs from the expected 83-byte result.", @"出力が基準の83バイトと一致しませんでした。");
    default:
        return text(@"Could not read or write the test file. Make the Windows file available in Files and try again.",
                    @"検査ファイルを読み書きできませんでした。「ファイル」でWindowsのサンプルが開ける状態にして、もう一度選んでください。");
    }
}

@interface SKClassicProbeController : UITableViewController <UIDocumentPickerDelegate>
@property(nonatomic) BOOL busy;
@property(nonatomic, copy) NSString *status;
@property(nonatomic, strong) NSData *lastOutput;
@property(nonatomic, strong) NSData *lastReport;
@end
@implementation SKClassicProbeController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = text(@"Test DLL co-save", @"テストDLLの追加セーブ");
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 68;
    NSDictionary *saved = [NSUserDefaults.standardUserDefaults dictionaryForKey:previousResultKey];
    NSData *output = saved[@"output"], *report = saved[@"report"];
    if ([output isKindOfClass:NSData.class] && matches(output, outputDigest) && savedReportIsValid(report)) {
        self.lastOutput = output; self.lastReport = report;
    }
    self.status = self.lastOutput ? text(@"Previous run completed. Windows input 16, saved result 21.",
        @"前回の検査は完了しました。Windowsから読み込んだ値は16、保存した値は21です。") :
        text(@"No test has run yet.", @"まだ検査していません。");
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)table { (void)table; return 2; }
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section {
    (void)table; return section == 1 && self.lastOutput ? 2 : 1;
}
- (NSString *)tableView:(UITableView *)table titleForHeaderInSection:(NSInteger)section {
    (void)table; return section == 0 ? text(@"Windows sample", @"Windowsのサンプル") : text(@"Result", @"結果");
}
- (NSString *)tableView:(UITableView *)table titleForFooterInSection:(NSInteger)section {
    (void)table;
    if (section == 0) return text(@"Choose the 83-byte windows.skse created by the test DLL on Windows. This runs the sample independently of Skyrim and its saves.",
        @"WindowsのテストDLLが作った83バイトの windows.skse を選んでください。ゲームや普段のセーブから独立して、このサンプルだけを検査します。");
    return self.lastOutput ? text(@"Send translated.skse and the test report back to the PC. Save to Files is available in the share sheet.",
                                   @"translated.skse と検査記録をPCへ戻してください。共有画面の「“ファイル”に保存」も使えます。") : nil;
}
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)p {
    (void)table;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    UIListContentConfiguration *content = [cell defaultContentConfiguration];
    content.textProperties.numberOfLines = 0;
    if (p.section == 0) {
        content.text = self.busy ? text(@"Checking…", @"検査中…") : text(@"Choose Windows sample", @"Windowsのサンプルを選ぶ");
        cell.accessibilityIdentifier = @"classic-probe-select";
    } else if (p.row == 0) {
        content.text = self.status;
        cell.accessibilityIdentifier = @"classic-probe-result";
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.contentConfiguration = content;
        return cell;
    } else {
        content.text = text(@"Share last successful result", @"最後に成功した結果を共有");
        cell.accessibilityIdentifier = @"classic-probe-share";
    }
    cell.userInteractionEnabled = !self.busy;
    content.textProperties.color = self.busy ? UIColor.secondaryLabelColor : self.view.tintColor;
    cell.contentConfiguration = content;
    return cell;
}
- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)p {
    [table deselectRowAtIndexPath:p animated:YES];
    if (self.busy) return;
    if (p.section == 0) {
        // Some Files providers classify .skse as generic data. The import
        // itself verifies the extension, size and exact Windows sample bytes.
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
        picker.delegate = self; picker.allowsMultipleSelection = NO;
        [self presentViewController:picker animated:YES completion:nil];
    } else if (p.row == 1 && matches(self.lastOutput, outputDigest) && self.lastReport.length) {
        NSURL *folder = scratchFolder();
        NSURL *item = [folder URLByAppendingPathComponent:@"translated.skse"];
        NSURL *report = [folder URLByAppendingPathComponent:@"classic-lab-result.json"];
        if (!item || ![self.lastOutput writeToURL:item options:NSDataWritingWithoutOverwriting error:NULL] ||
            ![self.lastReport writeToURL:report options:NSDataWritingWithoutOverwriting error:NULL]) {
            if (folder) [NSFileManager.defaultManager removeItemAtURL:folder error:NULL];
            self.status = failure(SKYRIM_CLASSIC_FIXTURE_IO);
            [self.tableView reloadData];
            return;
        }
        UIActivityViewController *sheet = [[UIActivityViewController alloc] initWithActivityItems:@[item, report] applicationActivities:nil];
        UIView *anchor = [table cellForRowAtIndexPath:p];
        sheet.popoverPresentationController.sourceView = anchor;
        sheet.popoverPresentationController.sourceRect = anchor.bounds;
        sheet.completionWithItemsHandler = ^(UIActivityType type, BOOL completed, NSArray *returned, NSError *error) {
            (void)type; (void)completed; (void)returned;
            [NSFileManager.defaultManager removeItemAtURL:folder error:NULL];
            if (error) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    self.status = text(@"Sharing failed. You can share the saved result again.", @"共有できませんでした。保存済みの結果をもう一度共有できます。");
                    [self.tableView reloadData];
                });
            }
        };
        [self presentViewController:sheet animated:YES completion:nil];
    }
}
- (void)documentPicker:(UIDocumentPickerViewController *)picker didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)picker;
    NSURL *url = urls.count == 1 ? urls.firstObject : nil;
    if (!url || self.busy) return;
    self.busy = YES;
    self.status = text(@"Checking the selected file…", @"選んだファイルを検査しています…");
    [self.tableView reloadData];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        SkyrimClassicFixtureResult result = {0};
        SkyrimClassicFixtureStatus status;
        NSData *output = runSelected(url, &status, &result);
        dispatch_async(dispatch_get_main_queue(), ^{
            self.busy = NO;
            NSData *report = output ? executionReport(result) : nil;
            if (output && report) {
                self.lastOutput = output; self.lastReport = report;
                [NSUserDefaults.standardUserDefaults setObject:@{@"output":output, @"report":report} forKey:previousResultKey];
                self.status = [NSString stringWithFormat:text(@"Read %u from Windows. Saved %u after reset, then %u after loading the sample.",
                    @"Windowsから%uを読み込みました。リセット後の保存値は%u、サンプルを読み直した後の保存値は%uです。"),
                    result.input_value, result.reverted_value, result.output_value];
            } else self.status = failure(output ? SKYRIM_CLASSIC_FIXTURE_OUTPUT : status);
            [self.tableView reloadData];
        });
    });
}
@end

UIViewController *skyrim_classic_probe_controller(void) {
    return [[SKClassicProbeController alloc] initWithStyle:UITableViewStyleInsetGrouped];
}
