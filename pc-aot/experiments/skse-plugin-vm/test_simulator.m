/* SPDX-License-Identifier: GPL-3.0-only */
#import "mod_dll_probe.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface SKDllFixtureScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) UITableViewController<UIDocumentPickerDelegate> *probe;
@end

@implementation SKDllFixtureScene
- (void)checkReport:(NSString *)expected launchNext:(void (^)(void))next attempts:(NSUInteger)attempts {
    UITableView *table = self.probe.tableView;
    if ([self.probe numberOfSectionsInTableView:table] == 2) {
        UITableViewCell *row = [self.probe tableView:table cellForRowAtIndexPath:[NSIndexPath indexPathForRow:0 inSection:1]];
        if (![row.textLabel.text isEqualToString:expected])
            [NSException raise:NSInternalInconsistencyException format:@"Unexpected DLL result: %@", row.textLabel.text];
        next();
    } else if (attempts) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            [self checkReport:expected launchNext:next attempts:attempts - 1];
        });
    } else {
        [NSException raise:NSInternalInconsistencyException format:@"DLL diagnosis did not return"];
    }
}
- (void)diagnose:(NSString *)name extension:(NSString *)extension {
    NSURL *url = [NSBundle.mainBundle URLForResource:name withExtension:extension];
    if (!url) [NSException raise:NSInternalInconsistencyException format:@"Missing DLL fixture %@", name];
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
    [self.probe documentPicker:picker didPickDocumentsAtURLs:@[url]];
}
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session;
    (void)options;
    BOOL japanese = [NSProcessInfo.processInfo.arguments containsObject:@"-TestJapanese"];
    self.probe = (id)skyrim_mod_dll_probe_controller(japanese, nil);
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [[UINavigationController alloc] initWithRootViewController:self.probe];
    [self.window makeKeyAndVisible];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self diagnose:@"unsupported" extension:@"dll"];
        [self checkReport:japanese ? @"このDLLは診断用に読み込めませんでした。" : @"This DLL could not be loaded for testing."
               launchNext:^{
            [self diagnose:@"supported" extension:@"dll"];
            [self checkReport:japanese ? @"診断環境でSKSEの初期化が完了しました。" : @"SKSE initialization passed in the test environment."
                   launchNext:^{
                [self diagnose:@"ProbePackage" extension:nil];
                [self checkReport:japanese ? @"SKSE用DLLを3個見つけました。" : @"Found 3 SKSE DLLs in this folder."
                       launchNext:^{
                    UITableView *table = self.probe.tableView;
                    NSInteger rows = [self.probe tableView:table numberOfRowsInSection:1];
                    BOOL verifiedPair = NO;
                    for (NSInteger i = 1; i < rows; ++i) {
                        UITableViewCell *cell = [self.probe tableView:table
                                                cellForRowAtIndexPath:[NSIndexPath indexPathForRow:i inSection:1]];
                        NSString *text = cell.textLabel.text;
                        if ([text hasPrefix:@"Data/SKSE/Plugins/Paired.dll\n"] &&
                            [text containsString:@"OwnedWorker.dll"] &&
                            [text containsString:japanese ? @"診断環境でSKSEの初期化が完了しました。"
                                                        : @"SKSE initialization passed in the test environment."])
                            verifiedPair = YES;
                    }
                    if (!verifiedPair)
                        [NSException raise:NSInternalInconsistencyException format:@"Could not verify the paired DLL result"];
                    NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
                    NSURL *evidence = [documents URLByAppendingPathComponent:japanese ? @"japanese.txt" : @"english.txt"];
                    NSError *error = nil;
                    if (![@"refused,passed,folder\n" writeToURL:evidence atomically:YES encoding:NSUTF8StringEncoding error:&error])
                        [NSException raise:NSInternalInconsistencyException format:@"Could not write fixture result: %@", error];
                } attempts:200];
            } attempts:200];
        } attempts:200];
    });
}
@end

@interface SKDllFixtureApp : UIResponder <UIApplicationDelegate> @end
@implementation SKDllFixtureApp
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session
                                      options:(UISceneConnectionOptions *)options {
    (void)application;
    (void)options;
    UISceneConfiguration *configuration = [[UISceneConfiguration alloc] initWithName:@"DLL Fixture" sessionRole:session.role];
    configuration.delegateClass = SKDllFixtureScene.class;
    return configuration;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(SKDllFixtureApp.class));
    }
}
