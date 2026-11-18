/* SPDX-License-Identifier: GPL-3.0-only
 * Simulator-only host for the shipping Settings and MOD controllers.
 * Only synthetic data in this dedicated bundle's sandbox is used. */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include "mobile_ui.h"
#include "mod_manager.h"

static NSURL *directory(NSSearchPathDirectory kind) {
    return [NSFileManager.defaultManager URLsForDirectory:kind inDomains:NSUserDomainMask].firstObject;
}

// Supply a reproducible starting directory to the real Files picker. Only this
// test bundle's MOD hub gets the override; its original picker, delegate and
// presentation still handle every selection, security-scoped URL and import.
static void configureFixturePicker(void) {
    Class hub = NSClassFromString(@"SKModHub");
    SEL action = @selector(presentViewController:animated:completion:);
    Method inherited = class_getInstanceMethod(hub, action);
    if (!inherited) [NSException raise:NSInternalInconsistencyException format:@"Missing MOD hub presentation"];
    typedef void (*Present)(id, SEL, UIViewController *, BOOL, void (^)(void));
    Present present = (Present)method_getImplementation(inherited);
    IMP withDirectory = imp_implementationWithBlock(^(UIViewController *host, UIViewController *controller,
                                                       BOOL animated, void (^completion)(void)) {
        if ([controller isKindOfClass:UIDocumentPickerViewController.class]) {
            UIDocumentPickerViewController *picker = (UIDocumentPickerViewController *)controller;
            picker.directoryURL = [directory(NSDocumentDirectory) URLByAppendingPathComponent:@"UI Samples" isDirectory:YES];
        }
        present(host, action, controller, animated, completion);
    });
    if (!class_addMethod(hub, action, withDirectory, method_getTypeEncoding(inherited)))
        [NSException raise:NSInternalInconsistencyException format:@"MOD hub now implements its own presentation"];
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
    @autoreleasepool {
        configureFixturePicker();
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(SKFixtureDelegate.class));
    }
}
