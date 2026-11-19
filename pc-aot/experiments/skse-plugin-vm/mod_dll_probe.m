/* SPDX-License-Identifier: GPL-3.0-only */
#import "mod_dll_probe.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "probe_directory.h"
#include "probe_local_file.h"

static NSString *Text(BOOL japanese, NSString *en, NSString *ja) {
    return japanese ? ja : en;
}

static NSString *ImageResult(BOOL japanese, SksePeStatus status) {
    if (!japanese) return [NSString stringWithUTF8String:skse_pe_status_text(status)];
    switch (status) {
    case SKSE_PE_WRONG_MACHINE: return @"Skyrim LEで使う32ビットx86形式が必要です。";
    case SKSE_PE_SIZE_LIMIT: return @"診断で読み取れるサイズや項目数の上限を超えました。";
    case SKSE_PE_BAD_ARGUMENT:
    case SKSE_PE_TRUNCATED:
    case SKSE_PE_BAD_FORMAT: return @"Windows DLLのファイル構造を確認できませんでした。";
    case SKSE_PE_UNSUPPORTED_WX: return @"実行領域への書き込みが必要です。";
    case SKSE_PE_UNSUPPORTED_TLS: return @"Windowsのスレッド専用データ（TLS）が必要です。";
    case SKSE_PE_BAD_TLS: return @"Windowsのスレッド専用データ（TLS）の配置を確認できませんでした。";
    case SKSE_PE_UNSUPPORTED_DELAY_IMPORTS: return @"実行中に追加のDLLを読み込む処理が必要です。";
    case SKSE_PE_UNSUPPORTED_CLR: return @".NETのDLLです。";
    case SKSE_PE_UNAPPROVED_IMPORT:
    case SKSE_PE_UNSUPPORTED_IMPORTS: return @"必要なWindows関数または補助DLLが診断環境で利用できません。";
    case SKSE_PE_ORDINAL_IMPORT: return @"番号を使って呼び出す外部のWindows関数が必要です。";
    case SKSE_PE_BOUND_IMPORT: return @"Windows側で事前に接続されたDLLが必要です。";
    case SKSE_PE_UNSUPPORTED_ENTRY: return @"未対応のWindows DLL初期化を使っています。";
    case SKSE_PE_UNSUPPORTED_CALLING_CONVENTION: return @"未対応のSKSE呼び出し形式を使っています。";
    case SKSE_PE_MISSING_EXPORTS: return @"LE用SKSEプラグインの入口が見つかりませんでした。";
    case SKSE_PE_MISSING_DEPENDENCY_EXPORT: return @"補助DLLに必要な関数が見つかりませんでした。";
    case SKSE_PE_FORWARDED_EXPORT:
    case SKSE_PE_FORWARDED_DEPENDENCY_EXPORT: return @"関数の呼び出しを別のDLLに移す設定が必要です。";
    case SKSE_PE_NO_MEMORY: return @"読み込み用のメモリを確保できませんでした。";
    case SKSE_PE_RELOCATIONS_REQUIRED:
    case SKSE_PE_UNSUPPORTED_RELOCATION:
    case SKSE_PE_BAD_RELOCATION:
    case SKSE_PE_OVERLAPPING_RELOCATION:
    case SKSE_PE_RELOCATION_IAT_CONFLICT: return @"DLLを別の仮想アドレスに配置できませんでした。";
    default: return @"未対応のWindows DLL形式を使っています。";
    }
}

static NSString *CallResult(BOOL japanese, SkseX86Status status) {
    if (!japanese) return [NSString stringWithUTF8String:skse_x86_status_text(status)];
    switch (status) {
    case SKSE_X86_MEMORY_FAULT: return @"DLLが使用できない領域を参照しました。";
    case SKSE_X86_UNSUPPORTED_INSTRUCTION: return @"未対応のx86命令が必要です。";
    case SKSE_X86_INSTRUCTION_LIMIT: return @"1回の診断で実行できる処理量の上限に達しました。";
    case SKSE_X86_BAD_CALL_RETURN: return @"DLLの呼び出しから正常に復帰できませんでした。";
    case SKSE_X86_UNSUPPORTED_API: return @"診断で提供していないWindowsまたはSKSE関数が呼ばれました。";
    case SKSE_X86_DIVIDE_FAULT: return @"DLLの除算で0除算または結果のあふれが発生しました。";
    default: return @"DLLの処理を実行できませんでした。";
    }
}

static NSArray<NSString *> *TrialLines(BOOL japanese, const SkseTrialResult *trial, const SksePeReport *imports) {
    switch (trial->status) {
    case SKSE_TRIAL_COMPLETED: {
        NSMutableArray<NSString *> *lines = [NSMutableArray arrayWithObjects:
            Text(japanese, @"SKSE initialization passed in the test environment.",
                           @"診断環境でSKSEの初期化が完了しました。"),
            [NSString stringWithFormat:@"%s", trial->name], nil];
        if (trial->unavailable_interface) {
            [lines addObject:[NSString stringWithFormat:Text(japanese, @"Also requested unavailable SKSE interface %u",
                                                            @"未対応のSKSEインターフェイス %u も要求されました。"),
                              trial->unavailable_interface]];
        }
        if (trial->serialization_callbacks) {
            [lines addObject:[NSString stringWithFormat:Text(japanese, @"Additional save test · Calls %u · Records %u",
                                                            @"追加セーブの試験　呼び出し %u 回、記録 %u 件"),
                              trial->serialization_callbacks, trial->serialization_records]];
        }
        if (trial->workers_completed) {
            [lines addObject:[NSString stringWithFormat:Text(japanese, @"Worker threads completed · %u",
                                                            @"終了まで確認した補助スレッド　%u 本"),
                              trial->workers_completed]];
        }
        if (trial->tasks_completed[SKSE_X86_GAME_TASK] || trial->tasks_completed[SKSE_X86_UI_TASK]) {
            [lines addObject:[NSString stringWithFormat:Text(japanese, @"Queued task test · Game %u · UI %u",
                                                            @"タスクの試験　ゲーム %u 件、UI %u 件"),
                              trial->tasks_completed[SKSE_X86_GAME_TASK], trial->tasks_completed[SKSE_X86_UI_TASK]]];
        }
        return lines;
    }
    case SKSE_TRIAL_IMAGE_REFUSED:
    case SKSE_TRIAL_DEPENDENCY_REFUSED: {
        NSString *summary = Text(japanese, @"This DLL could not be loaded for testing.",
                                           @"このDLLは診断用に読み込めませんでした。");
        SksePeStatus status = trial->status == SKSE_TRIAL_DEPENDENCY_REFUSED ? trial->dependency_status : trial->image_status;
        NSMutableArray<NSString *> *lines = [NSMutableArray arrayWithObjects:summary, ImageResult(japanese, status), nil];
        const SkseDependencyTrace *dependency = &trial->dependencies;
        if (dependency->import_symbol[0] && dependency->requested[0]) {
            NSString *library = [NSString stringWithUTF8String:dependency->requested];
            NSString *symbol = [NSString stringWithUTF8String:dependency->import_symbol];
            if (library && symbol) [lines addObject:[NSString stringWithFormat:@"%@ · %@", library, symbol]];
            if (!dependency->requester[0] && imports && !skse_trial_system_library(dependency->requested)) {
                [lines addObject:Text(japanese, @"To look up its helper DLLs, choose the whole MOD folder.",
                                              @"補助DLLを探す場合は、MODフォルダごと選んでください。")];
            }
        }
        if (!imports) return lines;
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
    case SKSE_TRIAL_DEPENDENCY_ATTACH_DECLINED:
        return @[Text(japanese, @"The DLL declined initialization in this environment.",
                                @"このDLLは診断環境での初期化を拒否しました。")];
    case SKSE_TRIAL_INVALID_INFO:
        return @[Text(japanese, @"The DLL returned invalid SKSE plugin information.",
                                @"DLLが返したSKSEプラグイン情報を確認できませんでした。")];
    case SKSE_TRIAL_NO_MEMORY:
        return @[Text(japanese, @"There was insufficient memory to run the check.",
                                @"診断に必要なメモリを確保できませんでした。")];
    case SKSE_TRIAL_TLS_ATTACH_FAULT:
    case SKSE_TRIAL_TLS_DETACH_FAULT:
        return @[Text(japanese, @"The DLL stopped while handling its thread-local data.",
                                @"DLLのスレッド専用データを処理する途中で停止しました。"),
                 CallResult(japanese, trial->tls_fault.status)];
    case SKSE_TRIAL_SERIALIZATION_FAULT:
        return @[Text(japanese, @"The DLL stopped during the test of its additional save data.",
                                @"DLLの追加セーブ処理の試験が途中で停止しました。"),
                 CallResult(japanese, trial->serialization_fault.status)];
    case SKSE_TRIAL_WORKER_FAULT:
        return @[Text(japanese, @"The DLL stopped during a worker thread test.",
                                @"DLLの補助スレッドの試験が途中で停止しました。"),
                 CallResult(japanese, trial->worker_fault.status)];
    case SKSE_TRIAL_TASK_FAULT:
        return @[Text(japanese, @"The DLL stopped during a queued SKSE task.",
                                @"後から実行するSKSEタスクの処理が途中で停止しました。"),
                 CallResult(japanese, trial->task_fault.status)];
    default: {
        const SkseX86Result *fault = &trial->query;
        if (trial->status == SKSE_TRIAL_ATTACH_FAULT) fault = &trial->attach;
        if (trial->status == SKSE_TRIAL_LOAD_FAULT) fault = &trial->load;
        if (trial->status == SKSE_TRIAL_DETACH_FAULT) fault = &trial->detach;
        if (trial->status == SKSE_TRIAL_MESSAGE_FAULT) fault = &trial->message;
        if (trial->status == SKSE_TRIAL_DEPENDENCY_ATTACH_FAULT) fault = &trial->dependency_attach;
        if (trial->status == SKSE_TRIAL_DEPENDENCY_DETACH_FAULT) fault = &trial->dependency_detach;
        return @[Text(japanese, @"The DLL encountered an unsupported operation during initialization.",
                                @"このDLLは初期化中に未対応の処理に入りました。"),
                 CallResult(japanese, fault->status)];
    }
    }
}

static NSArray<NSString *> *ReportLines(BOOL japanese, SkseProbeFileStatus status,
                                       const SkseLocalDllReport *report) {
    switch (status) {
    case SKSE_PROBE_FILE_CHECKED:
        return TrialLines(japanese, &report->trial, &report->imports);
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
    }
    return @[Text(japanese, @"The file could not be checked.", @"ファイルを診断できませんでした。")];
}

static NSString *FolderFileResult(BOOL japanese, const SkseProbeDirectoryFile *file) {
    if (file->trial_ran) return [TrialLines(japanese, &file->trial, NULL) componentsJoinedByString:@"\n"];
    switch (file->status) {
    case SKSE_PROBE_DLL_NOT_PLUGIN:
        return ImageResult(japanese, file->export_status);
    case SKSE_PROBE_DLL_NOT_REGULAR:
        return Text(japanese, @"This item is not a regular file.", @"通常のファイルとして開けませんでした。");
    case SKSE_PROBE_DLL_UNREADABLE:
        return Text(japanese, @"The DLL could not be read.", @"DLLを読み込めませんでした。");
    case SKSE_PROBE_DLL_EMPTY:
        return Text(japanese, @"The DLL file is empty.", @"中身が空のDLLです。");
    case SKSE_PROBE_DLL_TOO_LARGE:
        return Text(japanese, @"This DLL exceeds the size limit.", @"DLLが診断のサイズ上限を超えています。");
    case SKSE_PROBE_DLL_DEPENDENCY_MISSING:
        return Text(japanese, @"A required DLL is missing from this folder.", @"同じフォルダに必要な補助DLLがありません。");
    case SKSE_PROBE_DLL_DEPENDENCY_AMBIGUOUS:
        return Text(japanese, @"More than one file matches the required DLL.", @"同名の補助DLLが複数見つかりました。");
    case SKSE_PROBE_DLL_DEPENDENCY_MULTIPLE:
        return Text(japanese, @"This plugin requires multiple other DLLs.", @"複数の補助DLLを必要とするプラグインです。");
    case SKSE_PROBE_DLL_DEPENDENCY_CHAIN:
        return Text(japanese, @"The helper requires another DLL.", @"補助DLLがさらに別のDLLを必要としています。");
    case SKSE_PROBE_DLL_DEPENDENCY_CYCLE:
        return Text(japanese, @"The DLLs in this folder depend on each other in a loop.", @"フォルダ内のDLLが循環して依存しています。");
    case SKSE_PROBE_DLL_DEPENDENCY_LIMIT:
        return Text(japanese, @"The required DLLs exceed the diagnostic limits.", @"必要な補助DLLが診断の上限を超えています。");
    case SKSE_PROBE_DLL_DEPENDENCY_SELF:
    case SKSE_PROBE_DLL_DEPENDENCY_INVALID:
    case SKSE_PROBE_DLL_DEPENDENCY_UNREADABLE:
        return Text(japanese, @"A required DLL could not be loaded.", @"必要な補助DLLを読み込めませんでした。");
    default:
        return Text(japanese, @"This DLL could not be checked.", @"このDLLを診断できませんでした。");
    }
}

static NSArray<NSString *> *FolderReportLines(BOOL japanese, SkseProbeDirectoryStatus status,
                                             const SkseProbeDirectoryReport *report) {
    switch (status) {
    case SKSE_PROBE_DIRECTORY_UNAVAILABLE:
        return @[Text(japanese, @"The selected folder could not be opened.", @"選んだフォルダを開けませんでした。")];
    case SKSE_PROBE_DIRECTORY_AMBIGUOUS_LAYOUT:
        return @[Text(japanese, @"Multiple plugin folders were found. Choose the intended folder.", @"DLLを含むフォルダが複数あります。診断したいフォルダを選んでください。")];
    case SKSE_PROBE_DIRECTORY_FILE_LIMIT:
    case SKSE_PROBE_DIRECTORY_ENTRY_LIMIT:
        return @[Text(japanese, @"The folder contains too many files. Choose a smaller folder.", @"フォルダ内の項目が多すぎます。より小さいフォルダを選んでください。")];
    case SKSE_PROBE_DIRECTORY_SIZE_LIMIT:
        return @[Text(japanese, @"The combined DLL data exceeds 64 MiB.", @"DLLの合計サイズが64 MiBを超えています。")];
    case SKSE_PROBE_DIRECTORY_INCOMPLETE:
        return @[Text(japanese, @"The folder could not be read in full.", @"フォルダの内容を最後まで読み込めませんでした。")];
    case SKSE_PROBE_DIRECTORY_NO_MEMORY:
        return @[Text(japanese, @"There was insufficient memory to check this folder.", @"フォルダを診断するためのメモリを確保できませんでした。")];
    case SKSE_PROBE_DIRECTORY_CHECKED:
    case SKSE_PROBE_DIRECTORY_NO_PLUGINS:
        break;
    }
    NSString *summary = report->plugin_count
        ? [NSString stringWithFormat:Text(japanese, @"Found %u SKSE DLLs in this folder.", @"SKSE用DLLを%u個見つけました。"), report->plugin_count]
        : Text(japanese, @"No Skyrim LE SKSE plugins were found.", @"Skyrim LE用のSKSEプラグインが見つかりませんでした。");
    NSMutableArray<NSString *> *lines = [NSMutableArray arrayWithObject:summary];
    for (unsigned i = 0; i < report->file_count; ++i) {
        const SkseProbeDirectoryFile *file = &report->files[i];
        NSString *name = [NSString stringWithUTF8String:file->relative_path] ?: @"DLL";
        NSString *result = FolderFileResult(japanese, file);
        NSMutableString *detail = [NSMutableString stringWithFormat:@"%@\n%@", name, result];
        const SkseDependencyTrace *dependencies = &file->trial.dependencies;
        if (file->trial_ran && dependencies->status == SKSE_DEPENDENCY_READY && dependencies->helper_count) {
            NSMutableArray<NSString *> *helpers = [NSMutableArray array];
            for (unsigned helper = 0; helper < dependencies->helper_count; ++helper) {
                [helpers addObject:[NSString stringWithUTF8String:dependencies->load_order[helper]] ?: @"DLL"];
            }
            [detail appendFormat:Text(japanese, @"\nHelper DLL · %@", @"\n補助DLL　%@"),
                [helpers componentsJoinedByString:@", "]];
        } else if (file->dependency_name[0]) {
            NSString *dependency = [NSString stringWithUTF8String:file->dependency_name] ?: @"DLL";
            [detail appendFormat:Text(japanese, @"\nHelper DLL · %@", @"\n補助DLL　%@"), dependency];
            if (file->other_dependency_name[0]) {
                NSString *requester = [NSString stringWithUTF8String:file->other_dependency_name] ?: @"DLL";
                [detail appendFormat:Text(japanese, @" (required by %@)", @"（%@ が使用）"), requester];
            }
        }
        [lines addObject:detail];
    }
    return lines;
}

@interface SKModDllProbe : UITableViewController <UIDocumentPickerDelegate>
@property(nonatomic) BOOL japanese;
@property(nonatomic) BOOL busy;
@property(nonatomic, copy) NSArray<NSString *> *report;
@property(nonatomic, strong) NSURL *initialFolder;
- (void)diagnoseURL:(NSURL *)url;
@end

@implementation SKModDllProbe
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = Text(self.japanese, @"DLL Compatibility", @"DLL互換性診断");
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 52;
    if (self.initialFolder) {
        NSURL *folder = self.initialFolder;
        self.initialFolder = nil;
        [self diagnoseURL:folder];
    }
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    return self.report.count ? 2 : 1;
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    return section ? (NSInteger)self.report.count : 2;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    return section == 0 ? Text(self.japanese,
        @"Choose an LE SKSE DLL or its mod folder. Results describe the isolated initialization test.",
        @"LE用SKSEのDLLか、展開済みのMODフォルダを選びます。診断結果は専用環境での初期化を示します。") : nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"dll-check"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"dll-check"];
    BOOL canPick = path.section == 0 && !self.busy;
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.textColor = canPick ? UIColor.labelColor : UIColor.secondaryLabelColor;
    if (path.section) cell.textLabel.text = self.report[path.row];
    else if (path.row) cell.textLabel.text = Text(self.japanese, @"Choose a mod folder", @"MODフォルダを選ぶ");
    else cell.textLabel.text = self.busy ? Text(self.japanese, @"Checking…", @"診断中…") :
                                          Text(self.japanese, @"Choose a DLL", @"DLLファイルを選ぶ");
    cell.selectionStyle = canPick ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    cell.accessoryType = canPick ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
    cell.accessibilityIdentifier = path.section ? nil : (path.row ? @"choose-skse-dll-folder" : @"choose-skse-dll");
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    if (path.section || self.busy) return;
    UTType *type = path.row ? UTTypeFolder : ([UTType typeWithFilenameExtension:@"dll"] ?: UTTypeData);
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[type] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)picker didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)picker;
    [self diagnoseURL:urls.firstObject];
}
- (void)diagnoseURL:(NSURL *)url {
    if (self.busy || !url) return;
    BOOL japanese = self.japanese;
    if (!url.isFileURL) {
        self.report = @[Text(japanese, @"Choose a DLL file or mod folder.", @"DLLファイルかMODフォルダを選んでください。")];
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
                uint64_t ticks = (uint64_t)(NSProcessInfo.processInfo.systemUptime * 1000.0);
                NSNumber *directory = nil;
                [accessibleURL getResourceValue:&directory forKey:NSURLIsDirectoryKey error:NULL];
                if (directory.boolValue) {
                    SkseProbeDirectoryReport report;
                    SkseProbeDirectoryStatus status = skse_probe_local_directory(accessibleURL.fileSystemRepresentation, (uint32_t)ticks, &report);
                    lines = FolderReportLines(japanese, status, &report);
                } else if ([accessibleURL.pathExtension.lowercaseString isEqualToString:@"dll"]) {
                    SkseLocalDllReport report;
                    SkseProbeFileStatus status = skse_probe_local_dll(accessibleURL.fileSystemRepresentation, (uint32_t)ticks, &report);
                    lines = ReportLines(japanese, status, &report);
                } else {
                    lines = ReportLines(japanese, SKSE_PROBE_FILE_UNSUPPORTED, NULL);
                }
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

UIViewController *skyrim_mod_dll_probe_controller(BOOL japanese, NSURL *initialFolder) {
    SKModDllProbe *controller = [[SKModDllProbe alloc] initWithStyle:UITableViewStyleInsetGrouped];
    controller.japanese = japanese;
    controller.initialFolder = initialFolder;
    return controller;
}
