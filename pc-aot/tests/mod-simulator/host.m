/* SPDX-License-Identifier: GPL-3.0-only
 * Simulator-only host for the shipping Settings and MOD controllers.
 * Only synthetic data in this dedicated bundle's sandbox is used. */
#import <UIKit/UIKit.h>
#include "mobile_ui.h"
#include "mod_manager.h"

static NSURL *directory(NSSearchPathDirectory kind) {
    return [NSFileManager.defaultManager URLsForDirectory:kind inDomains:NSUserDomainMask].firstObject;
}

static void prepareDirectory(NSURL *url) {
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:&error]) {
        [NSException raise:NSInternalInconsistencyException format:@"Simulator fixture directory: %@", error];
    }
}

static void copyFixture(NSURL *source, NSURL *destination) {
    NSFileManager *manager = NSFileManager.defaultManager;
    if ([manager fileExistsAtPath:destination.path]) return;
    prepareDirectory(destination.URLByDeletingLastPathComponent);
    NSError *error = nil;
    if (![manager copyItemAtURL:source toURL:destination error:&error]) {
        [NSException raise:NSInternalInconsistencyException format:@"Simulator fixture copy: %@", error];
    }
}

@interface SKFixtureScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation SKFixtureScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    NSURL *fixtures = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"Fixtures"];
    NSURL *docs = directory(NSDocumentDirectory);
    NSURL *game = [docs URLByAppendingPathComponent:@"Skyrim"];
    NSURL *profile = [directory(NSApplicationSupportDirectory) URLByAppendingPathComponent:@"SkyrimIOS/Profile"];
    NSURL *inputs = [docs URLByAppendingPathComponent:@"UI Samples"];
    NSURL *plugins = [profile URLByAppendingPathComponent:@"Documents/Skyrim/plugins.txt"];
    NSURL *baseline = [fixtures URLByAppendingPathComponent:@"initial-plugins.txt"];
    BOOL reimport = [NSProcessInfo.processInfo.arguments containsObject:@"-ReimportAuthoredExport"];
    if (!reimport) {
        copyFixture([fixtures URLByAppendingPathComponent:@"Imports"], inputs);
        copyFixture([fixtures URLByAppendingPathComponent:@"Original"], game);
        copyFixture([fixtures URLByAppendingPathComponent:@"Authored Save.ess"],
            [profile URLByAppendingPathComponent:@"Documents/My Games/Skyrim/Saves/Authored Save.ess"]);
        copyFixture(baseline, plugins);
    }
    // Capture the real export for a later picker round trip. This copies only a
    // test-created JSON after the original UIActivityViewController presented it.
    if (reimport) {
        NSString *filename = @"Trial Two.skyrimios-modlist.json";
        NSURL *exported = [[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES] URLByAppendingPathComponent:filename];
        copyFixture(exported, [inputs URLByAppendingPathComponent:filename]);
    }
    NSString *active = skyrim_mods_select_profile(profile.path);
    if (!active) [NSException raise:NSInternalInconsistencyException format:@"Simulator profile selection failed"];
    skyrim_mobile_initialize(active.fileSystemRepresentation);
    NSString *view = skyrim_mods_prepare_game(game.path);
    if (!view || !skyrim_mobile_prepare_mods(view.fileSystemRepresentation)) {
        [NSException raise:NSInternalInconsistencyException format:@"Simulator MOD startup failed"];
    }
    if ((!reimport && (![view isEqual:game.path] || ![active isEqual:profile.path])) ||
        (reimport && ([view isEqual:game.path] || [active isEqual:profile.path])) ||
        ![[NSData dataWithContentsOfURL:plugins] isEqual:[NSData dataWithContentsOfURL:baseline]]) {
        [NSException raise:NSInternalInconsistencyException format:@"Unexpected game, profile or baseline plugin list"];
    }

    // A HUD and a guest render loop are outside this target. Start directly
    // on the shipping Settings table, whose real selection opens the MOD hub.
    UITableViewController *settings = [[NSClassFromString(@"SKSettings") alloc] initWithStyle:UITableViewStyleInsetGrouped];
    if (!settings) [NSException raise:NSInternalInconsistencyException format:@"Missing production Settings class"];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [[UINavigationController alloc] initWithRootViewController:settings];
    [self.window makeKeyAndVisible];
}
@end

@interface SKFixtureDelegate : UIResponder <UIApplicationDelegate> @end
@implementation SKFixtureDelegate @end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(SKFixtureDelegate.class)); }
}
