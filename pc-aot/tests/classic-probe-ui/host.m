/* SPDX-License-Identifier: GPL-3.0-only
 * Simulator-only integration test. Use the production controller, real UIKit
 * sheets and AOT sample without bringing up game or profile services. */
#import <UIKit/UIKit.h>
#import "classic_probe_ui.h"

typedef NS_ENUM(NSInteger, Stage) { PresentPicker, PickInvalid, PickValid, RetryInvalid, Reopen, Share, InspectShare };

@interface ProbeUIScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) UITableViewController *probe;
@property(nonatomic, strong) UINavigationController *navigation;
@property(nonatomic, strong) NSURL *documents;
@property(nonatomic, strong) NSURL *validInput;
@property(nonatomic, strong) NSURL *invalidInput;
@property(nonatomic) NSTimeInterval deadline;
@property(nonatomic) Stage stage;
@end

@implementation ProbeUIScene
- (void)finish:(NSString *)outcome {
    NSDictionary *state = @{@"outcome":outcome, @"stage":@(self.stage)};
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:state options:0 error:NULL];
    [bytes writeToURL:[self.documents URLByAppendingPathComponent:@"ui-check.json"] atomically:YES];
}
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    self.documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *sample = [NSBundle.mainBundle URLForResource:@"windows" withExtension:@"skse"];
    self.validInput = [self.documents URLByAppendingPathComponent:@"windows.skse"];
    self.invalidInput = [self.documents URLByAppendingPathComponent:@"other.skse"];
    NSMutableData *altered = [[NSData dataWithContentsOfURL:sample] mutableCopy];
    if (altered.length != 83 || ![NSFileManager.defaultManager copyItemAtURL:sample toURL:self.validInput error:NULL]) {
        [self finish:@"test-input-setup"]; return;
    }
    ((uint8_t *)altered.mutableBytes)[40] ^= 1;
    if (![altered writeToURL:self.invalidInput atomically:YES]) { [self finish:@"test-input-setup"]; return; }
    [NSUserDefaults.standardUserDefaults setObject:@{@"language":@0} forKey:@"mobile-settings-v1"];
    self.probe = (UITableViewController *)skyrim_classic_probe_controller();
    self.navigation = [[UINavigationController alloc] initWithRootViewController:self.probe];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = self.navigation;
    [self.window makeKeyAndVisible];
    self.deadline = NSProcessInfo.processInfo.systemUptime + 90.0;
    [self schedule];
}
- (void)schedule {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4), dispatch_get_main_queue(), ^{ [self tick]; });
}
- (void)selectSection:(NSInteger)section row:(NSInteger)row {
    [self.probe.tableView.delegate tableView:self.probe.tableView didSelectRowAtIndexPath:[NSIndexPath indexPathForRow:row inSection:section]];
}
- (NSString *)status {
    UITableViewCell *cell = [self.probe.tableView.dataSource tableView:self.probe.tableView
                                                 cellForRowAtIndexPath:[NSIndexPath indexPathForRow:0 inSection:1]];
    return cell.textLabel.text;
}
- (void)deliver:(NSURL *)url picker:(UIDocumentPickerViewController *)picker {
    id<UIDocumentPickerDelegate> recipient = (id<UIDocumentPickerDelegate>)self.probe;
    [recipient documentPicker:picker didPickDocumentsAtURLs:@[url]];
}

// Inspect the files while the real activity sheet is visible. Only this new
// test container is walked, and both artifacts are copied out for the driver.
- (BOOL)copyEvidence {
    NSFileManager *files = NSFileManager.defaultManager;
    NSURL *tmp = [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];
    NSArray *folders = [files contentsOfDirectoryAtURL:tmp includingPropertiesForKeys:nil options:0 error:NULL];
    for (NSURL *folder in folders) {
        NSURL *sample = [folder URLByAppendingPathComponent:@"translated.skse"];
        NSURL *report = [folder URLByAppendingPathComponent:@"classic-lab-result.json"];
        if (![files fileExistsAtPath:sample.path] || ![files fileExistsAtPath:report.path]) continue;
        return [files copyItemAtURL:sample toURL:[self.documents URLByAppendingPathComponent:sample.lastPathComponent] error:NULL] &&
               [files copyItemAtURL:report toURL:[self.documents URLByAppendingPathComponent:report.lastPathComponent] error:NULL];
    }
    return NO;
}
- (void)tick {
    if (NSProcessInfo.processInfo.systemUptime > self.deadline) { [self finish:@"timed-out"]; return; }
    switch (self.stage) {
    case PresentPicker:
        if (!self.probe.view.window) break;
        if (![[self status] containsString:@"No test has run"] || [self.probe.tableView numberOfRowsInSection:1] != 1) {
            [self finish:@"unexpected-initial-screen"]; return;
        }
        [self selectSection:0 row:0]; self.stage = PickInvalid; break;
    case PickInvalid: {
        UIDocumentPickerViewController *picker = (UIDocumentPickerViewController *)self.probe.presentedViewController;
        if (!picker || picker.isBeingPresented) break;
        if (![picker isKindOfClass:UIDocumentPickerViewController.class] || picker.delegate != (id)self.probe || picker.allowsMultipleSelection) {
            [self finish:@"unexpected-picker"]; return;
        }
        // The interactive Files provider is out of process. Deliver the URL
        // to its actual configured delegate in the same form as a selection.
        [self.probe dismissViewControllerAnimated:NO completion:^{ [self deliver:self.invalidInput picker:picker]; }];
        self.stage = PickValid; break;
    }
    case PickValid:
        if (![[self status] containsString:@"Choose the original"]) break;
        if ([self.probe.tableView numberOfRowsInSection:1] != 1) { [self finish:@"invalid-result-shareable"]; return; }
        [self deliver:self.validInput picker:nil]; self.stage = RetryInvalid; break;
    case RetryInvalid:
        if (![[self status] containsString:@"Read 16 from Windows. Saved 5 after reset, then 21"]) break;
        [self deliver:self.invalidInput picker:nil]; self.stage = Reopen; break;
    case Reopen:
        if (![[self status] containsString:@"Choose the original"]) break;
        if ([self.probe.tableView numberOfRowsInSection:1] != 2) { [self finish:@"lost-previous-result"]; return; }
        self.probe = (UITableViewController *)skyrim_classic_probe_controller();
        [self.navigation setViewControllers:@[self.probe] animated:NO]; self.stage = Share; break;
    case Share:
        if (!self.probe.view.window) break;
        if (![[self status] containsString:@"Previous run completed"] || [self.probe.tableView numberOfRowsInSection:1] != 2) {
            [self finish:@"did-not-restore-last-success"]; return;
        }
        [self selectSection:1 row:1]; self.stage = InspectShare; break;
    case InspectShare:
        if (!self.probe.presentedViewController || self.probe.presentedViewController.isBeingPresented) break;
        if (![self.probe.presentedViewController isKindOfClass:UIActivityViewController.class] || ![self copyEvidence]) {
            [self finish:@"missing-share-sheet-or-files"]; return;
        }
        [self finish:@"passed"]; return;
    }
    [self schedule];
}
@end

@interface ProbeUIApp : UIResponder <UIApplicationDelegate> @end
@implementation ProbeUIApp @end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProbeUIApp.class)); }
}
