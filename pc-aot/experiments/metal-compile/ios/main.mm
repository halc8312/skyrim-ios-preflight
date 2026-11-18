// Authored Metal comparison UI. No Skyrim files, account or network access.
#import <UIKit/UIKit.h>
#import <TargetConditionals.h>
#import "../runtime_host.h"
#import "ProbeRunStore.h"
#include <atomic>
#include <memory>

static NSString* apiName(NSString* api) {
    if (![api isKindOfClass:NSString.class]) return @"準備中";
    NSDictionary* names = @{@"legacy_source": @"従来方式", @"metal4_source": @"Metal 4",
                            @"legacy_async_source": @"従来方式・非同期", @"metal4_async_source": @"Metal 4・非同期"};
    return names[api] ?: @"準備中";
}

static NSString* caseName(NSString* caseID) {
    if (![caseID isKindOfClass:NSString.class]) return @"テスト用の負荷";
    NSDictionary* names = @{@"branch_mix": @"分岐と演算", @"table_permute": @"表参照と並べ替え",
                            @"coupled_rounds": @"連続する演算", @"control_fixture": @"基本テスト"};
    return names[caseID] ?: @"追加の負荷";
}

static UILabel* makeLabel(NSString* text, UIFontTextStyle style) {
    UILabel* label = [UILabel new];
    label.text = text;
    label.font = [UIFont preferredFontForTextStyle:style];
    label.adjustsFontForContentSizeCategory = YES;
    label.numberOfLines = 0;
    return label;
}

static UIButton* makeButton(NSString* title, BOOL filled) {
    UIButtonConfiguration* configuration = filled ? [UIButtonConfiguration filledButtonConfiguration]
                                                    : [UIButtonConfiguration tintedButtonConfiguration];
    configuration.title = title;
    configuration.cornerStyle = UIButtonConfigurationCornerStyleLarge;
    configuration.contentInsets = NSDirectionalEdgeInsetsMake(15, 16, 15, 16);
    UIButton* button = [UIButton buttonWithConfiguration:configuration primaryAction:nil];
    button.titleLabel.adjustsFontForContentSizeCategory = YES;
    button.accessibilityLabel = title;
    return button;
}

@interface MetalProbeController : UIViewController {
    std::shared_ptr<std::atomic<bool>> _continueRun;
    BOOL _running;
#if TARGET_OS_SIMULATOR
    BOOL _previewStarted;
#endif
}
@property(nonatomic, strong) ProbeRunStore* store;
@property(nonatomic, strong) NSURL* reportURL;
@property(nonatomic, strong) UILabel* stateLabel;
@property(nonatomic, strong) UILabel* detailLabel;
@property(nonatomic, strong) UILabel* resultLabel;
@property(nonatomic, strong) UILabel* savedLabel;
@property(nonatomic, strong) UISegmentedControl* orderControl;
@property(nonatomic, strong) UIProgressView* progressView;
@property(nonatomic, strong) UIButton* startButton;
@property(nonatomic, strong) UIButton* stopButton;
@property(nonatomic, strong) UIButton* shareButton;
@end

@implementation MetalProbeController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Metal 比較";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.navigationController.navigationBar.prefersLargeTitles = YES;
    UIScrollView* scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    UIStackView* content = [UIStackView new];
    content.axis = UILayoutConstraintAxisVertical;
    content.spacing = 18;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [content.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:20],
        [content.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-28],
        [content.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:20],
        [content.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-20],
        [content.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-40]
    ]];
    [content addArrangedSubview:makeLabel(@"3種類の負荷で4方式を比較", UIFontTextStyleTitle2)];
    UILabel* intro = makeLabel(@"自作のテストで、描画の準備時間と結果を比べます。測定中は画面を開いたままお待ちください。", UIFontTextStyleBody);
    intro.textColor = UIColor.secondaryLabelColor;
    [content addArrangedSubview:intro];
    [content addArrangedSubview:makeLabel(@"最初に試す方式", UIFontTextStyleHeadline)];
    self.orderControl = [[UISegmentedControl alloc] initWithItems:@[@"従来方式", @"Metal 4"]];
    self.orderControl.selectedSegmentIndex = 0;
    self.orderControl.accessibilityLabel = @"最初に試す方式";
    [self.orderControl.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [content addArrangedSubview:self.orderControl];
    UILabel* orderHint = makeLabel(@"各負荷で順番を入れ替え、4方式を4回ずつ測ります（合計48回）。開始方式は自由に選べます。", UIFontTextStyleSubheadline);
    orderHint.textColor = UIColor.secondaryLabelColor;
    [content addArrangedSubview:orderHint];
    self.stateLabel = makeLabel(@"準備できています", UIFontTextStyleHeadline);
    self.stateLabel.accessibilityIdentifier = @"probe.status";
    [content addArrangedSubview:self.stateLabel];
    self.detailLabel = makeLabel(@"開始すると、対応状況を確認して比較を進めます。", UIFontTextStyleSubheadline);
    self.detailLabel.textColor = UIColor.secondaryLabelColor;
    [content addArrangedSubview:self.detailLabel];
    self.progressView = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    self.progressView.accessibilityLabel = @"測定の進み具合";
    [content addArrangedSubview:self.progressView];
    self.startButton = makeButton(@"比較を開始", YES);
    self.startButton.accessibilityIdentifier = @"probe.start";
    [self.startButton addTarget:self action:@selector(startRun) forControlEvents:UIControlEventTouchUpInside];
    [content addArrangedSubview:self.startButton];
    self.stopButton = makeButton(@"停止", NO);
    self.stopButton.enabled = NO;
    self.stopButton.accessibilityIdentifier = @"probe.stop";
    [self.stopButton addTarget:self action:@selector(stopRun) forControlEvents:UIControlEventTouchUpInside];
    [content addArrangedSubview:self.stopButton];
    [content addArrangedSubview:makeLabel(@"今回の結果", UIFontTextStyleHeadline)];
    self.resultLabel = makeLabel(@"まだ測定していません", UIFontTextStyleBody);
    self.resultLabel.accessibilityIdentifier = @"probe.result";
    [content addArrangedSubview:self.resultLabel];
    self.savedLabel = makeLabel(@"結果はこのアプリ内に保存されます。", UIFontTextStyleFootnote);
    self.savedLabel.textColor = UIColor.secondaryLabelColor;
    [content addArrangedSubview:self.savedLabel];
    self.shareButton = makeButton(@"結果を共有", NO);
    self.shareButton.enabled = NO;
    self.shareButton.accessibilityIdentifier = @"probe.share";
    [self.shareButton addTarget:self action:@selector(shareReport) forControlEvents:UIControlEventTouchUpInside];
    [content addArrangedSubview:self.shareButton];
    UILabel* scope = makeLabel(@"これはSkyrimの動作速度を測るテストではありません。ゲーム本体やセーブにはアクセスしません。", UIFontTextStyleFootnote);
    scope.textColor = UIColor.secondaryLabelColor;
    [content addArrangedSubview:scope];

    NSError* error = nil;
    NSURL* support = [NSFileManager.defaultManager URLForDirectory:NSApplicationSupportDirectory
                                                        inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:&error];
    if (support) self.store = [[ProbeRunStore alloc] initWithRootURL:[support URLByAppendingPathComponent:@"MetalProbe" isDirectory:YES] error:&error];
    if (!self.store) {
        self.stateLabel.text = @"保存先を用意できませんでした";
        self.detailLabel.text = error.localizedDescription ?: @"アプリを起動し直してください。";
        self.startButton.enabled = NO;
    } else {
        self.reportURL = self.store.latestReportURL;
        if (self.reportURL) {
            NSDictionary* record = [self.store readRecordAtURL:self.reportURL error:&error];
            if (record) [self showRecord:record];
            else self.reportURL = nil;
            self.shareButton.enabled = self.reportURL != nil;
        }
    }
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(leavingActive:)
                                               name:UIApplicationWillResignActiveNotification object:nil];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    if (_continueRun) _continueRun->store(false);
}

- (void)startRun {
    if (_running || !self.store || UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    if (NSProcessInfo.processInfo.thermalState >= NSProcessInfoThermalStateSerious) {
        self.stateLabel.text = @"端末が冷めてから始めてください";
        self.detailLabel.text = @"高温時は測定を開始しません。少し時間を置いてお試しください。";
        return;
    }
    NSString* source = [NSBundle.mainBundle pathForResource:@"phone_fixture" ofType:@"metal"];
    if (!source) {
        self.stateLabel.text = @"テストデータが見つかりません";
        self.detailLabel.text = @"検証アプリの構成を確認する必要があります。";
        return;
    }
    const BOOL metal4First = self.orderControl.selectedSegmentIndex == 1;
    NSError* error = nil;
    NSURL* buildURL = [NSBundle.mainBundle URLForResource:@"BuildInfo" withExtension:@"json"];
    NSData* buildBytes = buildURL ? [NSData dataWithContentsOfURL:buildURL options:0 error:&error] : nil;
    NSDictionary* buildInfo = buildBytes ? [NSJSONSerialization JSONObjectWithData:buildBytes options:0 error:&error] : nil;
    if (![buildInfo isKindOfClass:NSDictionary.class] || ![buildInfo[@"source_revision"] isKindOfClass:NSString.class]) {
        self.stateLabel.text = @"アプリの検証情報が見つかりません";
        self.detailLabel.text = @"どの版で測定したか確認できないため、開始を見合わせます。";
        return;
    }
    NSURL* reportURL = [self.store beginRunWithMetal4First:metal4First error:&error];
    if (!reportURL) {
        self.stateLabel.text = @"結果の保存を開始できませんでした";
        self.detailLabel.text = error.localizedDescription;
        return;
    }
    _continueRun = std::make_shared<std::atomic<bool>>(true);
    const auto control = _continueRun;
    _running = YES;
    self.reportURL = reportURL;
    self.startButton.enabled = NO;
    self.orderControl.enabled = NO;
    self.stopButton.enabled = YES;
    self.shareButton.enabled = NO;
    self.progressView.progress = 0;
    self.stateLabel.text = @"対応状況を確認しています";
    self.detailLabel.text = @"画面を開いたままお待ちください。";
    self.resultLabel.text = @"測定中";
    self.savedLabel.text = @"経過を端末に保存しています。";
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    ProbeRunStore* store = self.store;
    // The retained controller/run objects survive callbacks. UI never joins a task.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            __block NSError* saveError = nil;
            NSDictionary* comparison = SkyrimMetalRunPhoneComparison(source, 4, metal4First,
                ^BOOL { return control->load(); }, ^(NSDictionary* snapshot) {
                    NSError* checkpointError = nil;
                    NSMutableDictionary* savedSnapshot = [snapshot mutableCopy];
                    savedSnapshot[@"host_build"] = buildInfo;
                    if (![store writeSnapshot:savedSnapshot toURL:reportURL finished:NO error:&checkpointError]) {
                        saveError = checkpointError;
                        control->store(false);
                    }
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (!self->_running) return;
                        NSUInteger done = [snapshot[@"completed_samples"] unsignedIntegerValue];
                        NSUInteger total = [snapshot[@"total_samples"] unsignedIntegerValue];
                        if (total) self.progressView.progress = float(done) / float(total);
                        if (control->load()) {
                            self.stateLabel.text = @"比較しています";
                            NSUInteger caseIndex = [snapshot[@"active_case_index"] unsignedIntegerValue];
                            NSUInteger totalCases = [snapshot[@"total_cases"] unsignedIntegerValue];
                            NSString* progress = [NSString stringWithFormat:@"%@　%lu / %lu回", apiName(snapshot[@"active_api"]),
                                                  (unsigned long)done, (unsigned long)total];
                            self.detailLabel.text = caseIndex && totalCases
                                ? [NSString stringWithFormat:@"負荷 %lu / %lu　%@\n%@", (unsigned long)caseIndex,
                                   (unsigned long)totalCases, caseName(snapshot[@"active_case_id"]), progress]
                                : progress;
                        }
                    });
                });
            NSError* finalError = nil;
            NSMutableDictionary* finalSnapshot = [comparison mutableCopy];
            finalSnapshot[@"host_build"] = buildInfo;
            if (saveError) {
                finalSnapshot[@"host_save_error"] = @{@"domain": saveError.domain, @"code": @(saveError.code),
                                                       @"description": saveError.localizedDescription ?: @""};
            }
            BOOL stored = [store writeSnapshot:finalSnapshot toURL:reportURL finished:YES error:&finalError];
            NSDictionary* record = stored ? [store readRecordAtURL:reportURL error:&finalError] : nil;
            NSError* preservedError = finalError ?: saveError;
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_running = NO;
                UIApplication.sharedApplication.idleTimerDisabled = NO;
                self.startButton.enabled = YES;
                self.orderControl.enabled = YES;
                self.stopButton.enabled = NO;
                self.shareButton.enabled = YES;
                if (record) [self showRecord:record];
                else {
                    self.stateLabel.text = @"結果を保存できませんでした";
                    self.detailLabel.text = preservedError.localizedDescription ?: @"最後に保存できた経過を共有できます。";
                    self.savedLabel.text = @"共有されるファイルは途中経過です。";
                }
            });
        }
    });
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
#if TARGET_OS_SIMULATOR
    // Preview arguments are compiled out of the physical-device app.
    if (!_previewStarted && [NSProcessInfo.processInfo.arguments containsObject:@"--probe-ui-smoke"]) {
        _previewStarted = YES;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self startRun]; });
    }
    if (!_previewStarted && [NSProcessInfo.processInfo.arguments containsObject:@"--probe-ui-share"] && self.reportURL) {
        _previewStarted = YES;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self shareReport]; });
    }
#endif
}

- (void)stopRun {
    if (!_running || !_continueRun) return;
    _continueRun->store(false);
    self.stopButton.enabled = NO;
    self.stateLabel.text = @"停止を待っています";
    self.detailLabel.text = @"新しい処理を止めました。実行中の処理が戻り次第、結果を保存します。";
}

- (void)leavingActive:(NSNotification*)notification {
    (void)notification;
    [self stopRun];
    UIApplication.sharedApplication.idleTimerDisabled = NO;
}

- (void)showRecord:(NSDictionary*)record {
    NSString* status = record[@"status"];
    NSDictionary* comparison = record[@"comparison"];
    BOOL passed = [status isEqual:@"passed"];
    self.progressView.progress = passed ? 1 : self.progressView.progress;
    if (passed) {
        self.stateLabel.text = @"比較が完了しました";
        NSUInteger totalCases = [comparison[@"total_cases"] unsignedIntegerValue];
        self.detailLabel.text = totalCases > 1
            ? [NSString stringWithFormat:@"%lu種類の負荷で、描画・計算結果の一致を確認しました。", (unsigned long)totalCases]
            : @"テスト用の描画・計算結果は一致しました。";
    } else if ([status isEqual:@"unsupported"]) {
        self.stateLabel.text = @"この環境では比較できません";
        self.detailLabel.text = @"Metal 4の対応条件を満たしていません。対応状況を結果に保存しました。";
    } else if ([status isEqual:@"interrupted"] || [status isEqual:@"running"]) {
        self.stateLabel.text = @"比較は途中で終了しました";
        self.detailLabel.text = @"完了した結果としては扱いません。保存済みの経過を共有できます。";
    } else {
        self.stateLabel.text = @"確認が必要な結果です";
        self.detailLabel.text = @"エラーの内容と途中までの結果を共有してください。";
    }
    if (comparison[@"host_save_error"]) {
        self.stateLabel.text = @"途中経過の保存で問題がありました";
        self.detailLabel.text = @"その時点で測定を止め、保存エラーの情報を最終結果に残しました。";
    }
    NSArray* samples = [comparison[@"samples"] isKindOfClass:NSArray.class] ? comparison[@"samples"] : @[];
    NSMutableArray<NSString*>* caseIDs = [NSMutableArray new];
    for (NSDictionary* sample in samples) {
        NSString* caseID = [sample[@"case_id"] isKindOfClass:NSString.class] ? sample[@"case_id"] : @"";
        if (![caseIDs containsObject:caseID]) [caseIDs addObject:caseID];
    }
    NSMutableArray<NSString*>* groups = [NSMutableArray new];
    for (NSString* caseID in caseIDs) {
        NSMutableArray<NSString*>* lines = [NSMutableArray new];
        for (NSString* api in @[@"legacy_source", @"metal4_source", @"legacy_async_source", @"metal4_async_source"]) {
            NSMutableArray<NSNumber*>* durations = [NSMutableArray new];
            for (NSDictionary* sample in samples) {
                NSString* sampleCase = [sample[@"case_id"] isKindOfClass:NSString.class] ? sample[@"case_id"] : @"";
                if ([sampleCase isEqual:caseID] && [sample[@"status"] isEqual:@"passed"] && [sample[@"api"] isEqual:api]
                    && [sample[@"library_ns"] isKindOfClass:NSNumber.class] && [sample[@"command_status"] isEqual:@"completed"])
                    [durations addObject:sample[@"library_ns"]];
            }
            [durations sortUsingSelector:@selector(compare:)];
            if (durations.count) {
                NSUInteger middle = durations.count / 2;
                double median = durations[middle].doubleValue;
                if (durations.count % 2 == 0) median = (median + durations[middle - 1].doubleValue) / 2;
                median /= 1e6;
                [lines addObject:[NSString stringWithFormat:@"%@　%.2f ms（%lu回）", apiName(api), median, (unsigned long)durations.count]];
            }
        }
        if (lines.count) {
            NSString* values = [lines componentsJoinedByString:@"\n"];
            [groups addObject:caseID.length ? [NSString stringWithFormat:@"%@\n%@", caseName(caseID), values] : values];
        }
    }
    self.resultLabel.text = groups.count ? [[groups componentsJoinedByString:@"\n\n"] stringByAppendingString:@"\n\n各方式のライブラリ準備時間の中央値です。描画性能や初回キャッシュの速さを示すものではありません。"]
                                      : @"比較できた測定値はありません";
    self.savedLabel.text = [record[@"recovered_incomplete"] boolValue]
        ? @"前回の途中経過を表示しています。新しい比較も開始できます。"
        : @"結果を保存しました。共有から、このチャットへ送れます。";
}

- (void)shareReport {
    if (_running || !self.reportURL) return;
    UIActivityViewController* share = [[UIActivityViewController alloc] initWithActivityItems:@[self.reportURL] applicationActivities:nil];
    share.popoverPresentationController.sourceView = self.shareButton;
    share.popoverPresentationController.sourceRect = self.shareButton.bounds;
    [self presentViewController:share animated:YES completion:nil];
}
@end

@interface MetalProbeScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation MetalProbeScene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    MetalProbeController* controller = [MetalProbeController new];
    self.window.rootViewController = [[UINavigationController alloc] initWithRootViewController:controller];
    [self.window makeKeyAndVisible];
}
@end

@interface MetalProbeApp : UIResponder <UIApplicationDelegate>
@end
@implementation MetalProbeApp
- (UISceneConfiguration*)application:(UIApplication*)application configurationForConnectingSceneSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    (void)application; (void)options;
    UISceneConfiguration* configuration = [[UISceneConfiguration alloc] initWithName:@"Metal Probe" sessionRole:session.role];
    configuration.delegateClass = MetalProbeScene.class;
    return configuration;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(MetalProbeApp.class)); }
}
