/* SPDX-License-Identifier: GPL-3.0-only */
#import "mod_dll_probe.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "probe_local_file.h"

static NSString *Text(BOOL japanese, NSString *en, NSString *ja) {
    return japanese ? ja : en;
}

static NSArray<NSString *> *ReportLines(BOOL japanese, SkseProbeFileStatus status,
                                       const SkseLocalDllReport *report) {
    switch (status) {
    case SKSE_PROBE_FILE_UNAVAILABLE:
        return @[Text(japanese, @"The selected file could not be opened.", @"選んだファイルを開けませんでした。")];
    case SKSE_PROBE_FILE_UNSUPPORTED:
        return @[Text(japanese, @"Choose a non-empty DLL file.", @"中身のあるDLLファイルを選んでください。")];
    case SKSE_PROBE_FILE_TOO_LARGE:
        return @[Text(japanese, @"The file exceeds 64 MiB.", @"ファイルが64 MiBを超えています。")];
    case SKSE_PROBE_FILE_NO_MEMORY:
        return @[Text(japanese, @"There was insufficient memory to check this file.", @"ファイルの診断に必要なメモリを確保できませんでした。")];
    case SKSE_PROBE_FILE_INCOMPLETE:
        return @[Text(japanese, @"The entire file could not be read.", @"ファイル全体を読み込めませんでした。")];
    case SKSE_PROBE_FILE_CHECKED:
        break;
    }
    const SkseTrialResult *trial = &report->trial;
    switch (trial->status) {
    case SKSE_TRIAL_COMPLETED:
        return @[Text(japanese, @"SKSE initialization passed in the test environment.",
                                @"診断環境でSKSEの初期化が完了しました。"),
                 [NSString stringWithFormat:@"%s", trial->name]];
    case SKSE_TRIAL_IMAGE_REFUSED: {
        NSString *summary = Text(japanese, @"This DLL could not be loaded for testing.",
                                           @"このDLLは診断用に読み込めませんでした。");
        NSMutableArray<NSString *> *lines = [NSMutableArray arrayWithObjects:summary,
            [NSString stringWithUTF8String:skse_pe_status_text(trial->image_status)], nil];
        const SksePeReport *imports = &report->imports;
        for (uint16_t i = 0; i < imports->library_count && i < 8; ++i)
            [lines addObject:[NSString stringWithUTF8String:imports->libraries[i]]];
        if (imports->library_count > 8)
            [lines addObject:[NSString stringWithFormat:Text(japanese, @"%u other libraries", @"ほか%uライブラリ"),
                              (unsigned)imports->library_count - 8]];
        return lines;
    }
    case SKSE_TRIAL_ATTACH_DECLINED:
    case SKSE_TRIAL_QUERY_DECLINED:
    case SKSE_TRIAL_LOAD_DECLINED:
        return @[Text(japanese, @"The DLL declined initialization in this environment.",
                                @"このDLLは診断環境での初期化を拒否しました。")];
    case SKSE_TRIAL_INVALID_INFO:
        return @[Text(japanese, @"The DLL returned invalid SKSE plugin information.",
                                @"DLLが返したSKSEプラグイン情報を確認できませんでした。")];
    case SKSE_TRIAL_NO_MEMORY:
        return @[Text(japanese, @"There was insufficient memory to run the check.",
                                @"診断に必要なメモリを確保できませんでした。")];
    default: {
        const SkseX86Result *fault = &trial->query;
        if (trial->status == SKSE_TRIAL_ATTACH_FAULT) fault = &trial->attach;
        if (trial->status == SKSE_TRIAL_LOAD_FAULT) fault = &trial->load;
        if (trial->status == SKSE_TRIAL_DETACH_FAULT) fault = &trial->detach;
        return @[Text(japanese, @"The DLL encountered an unsupported operation during initialization.",
                                @"このDLLは初期化中に未対応の処理に入りました。"),
                 [NSString stringWithUTF8String:skse_x86_status_text(fault->status)]];
    }
    }
}

@interface SKModDllProbe : UITableViewController <UIDocumentPickerDelegate>
@property(nonatomic) BOOL japanese;
@property(nonatomic) BOOL busy;
@property(nonatomic, copy) NSArray<NSString *> *report;
@end

@implementation SKModDllProbe
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = Text(self.japanese, @"DLL Compatibility", @"DLL互換性診断");
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 52;
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    return self.report.count ? 2 : 1;
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    return section ? (NSInteger)self.report.count : 1;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    return section == 0 ? Text(self.japanese,
        @"Check one Skyrim LE SKSE plugin on this device. This check never attaches the plugin to the game.",
        @"Skyrim LE用のSKSEプラグインを端末内で1件ずつ診断します。診断したDLLがゲームで有効になることはありません。") : nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"dll-check"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"dll-check"];
    BOOL canPick = path.section == 0 && !self.busy;
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.textColor = canPick ? UIColor.labelColor : UIColor.secondaryLabelColor;
    cell.textLabel.text = path.section ? self.report[path.row] :
        self.busy ? Text(self.japanese, @"Checking…", @"診断中…") :
                    Text(self.japanese, @"Choose a DLL", @"DLLファイルを選ぶ");
    cell.selectionStyle = canPick ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    cell.accessoryType = canPick ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
    cell.accessibilityIdentifier = path.section ? nil : @"choose-skse-dll";
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    if (path.section || self.busy) return;
    UTType *type = [UTType typeWithFilenameExtension:@"dll"] ?: UTTypeData;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[type] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)picker didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)picker;
    NSURL *url = urls.firstObject;
    if (self.busy || !url) return;
    BOOL japanese = self.japanese;
    if (!url.isFileURL || ![url.pathExtension.lowercaseString isEqualToString:@"dll"]) {
        self.report = @[Text(japanese, @"Choose a DLL file.", @"DLLファイルを選んでください。")];
        [self.tableView reloadData];
        return;
    }
    self.report = nil;
    self.busy = YES;
    [self.tableView reloadData];
    __weak SKModDllProbe *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            __block NSArray<NSString *> *lines = nil;
            BOOL scoped = [url startAccessingSecurityScopedResource];
            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            NSError *coordinationError = nil;
            [coordinator coordinateReadingItemAtURL:url options:0 error:&coordinationError byAccessor:^(NSURL *accessibleURL) {
                SkseLocalDllReport report;
                uint64_t ticks = (uint64_t)(NSProcessInfo.processInfo.systemUptime * 1000.0);
                SkseProbeFileStatus status = skse_probe_local_dll(accessibleURL.fileSystemRepresentation, (uint32_t)ticks, &report);
                lines = ReportLines(japanese, status, &report);
            }];
            if (scoped) [url stopAccessingSecurityScopedResource];
            if (coordinationError || !lines) lines = ReportLines(japanese, SKSE_PROBE_FILE_UNAVAILABLE, NULL);
            dispatch_async(dispatch_get_main_queue(), ^{
                SKModDllProbe *controller = weakSelf;
                controller.report = lines;
                controller.busy = NO;
                [controller.tableView reloadData];
            });
        }
    });
}
@end

UIViewController *skyrim_mod_dll_probe_controller(BOOL japanese) {
    SKModDllProbe *controller = [[SKModDllProbe alloc] initWithStyle:UITableViewStyleInsetGrouped];
    controller.japanese = japanese;
    return controller;
}
