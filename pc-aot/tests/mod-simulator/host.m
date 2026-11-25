/* SPDX-License-Identifier: GPL-3.0-only
 * Simulator-only host for the shipping Settings and MOD controllers.
 * Only synthetic data in this dedicated bundle's sandbox is used. */
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <errno.h>
#include "mobile_ui.h"
#include "mod_manager.h"
#ifdef SKYRIM_IOS_CLASSIC_SKSE
#include "PcGuestSkseTrial.h"

// The fixture has no paired game framework or SkyUI plugin. Enabling the
// setting in UIKit may persist, but starting the runtime in this mode cannot.
int pc_guest_select_classic_skse(int enabled) { return enabled ? -1 : 0; }
#endif

static NSURL *directory(NSSearchPathDirectory kind) {
    return [NSFileManager.defaultManager URLsForDirectory:kind inDomains:NSUserDomainMask].firstObject;
}

// The shipping selector must enter Swift and return a main-thread result. Apple supports
// downloading models and translating on physical devices, so the simulator returns nil.
static void checkTranslationBridge(UITableViewController *settings) {
    Class translator = NSClassFromString(@"SKTranslator");
    SEL selector = NSSelectorFromString(@"translate:in:completion:");
    if (![translator respondsToSelector:selector])
        [NSException raise:NSInternalInconsistencyException format:@"Missing Swift MOD translation bridge"];
    typedef void (*Translate)(id, SEL, NSArray *, UIViewController *, void (^)(NSArray *));
    ((Translate)objc_msgSend)(translator, selector, @[@"Choose a colour for this installer."], settings, ^(NSArray *result) {
        if (!NSThread.isMainThread || result != nil)
            [NSException raise:NSInternalInconsistencyException format:@"Unexpected simulator translation result"];
        settings.tableView.accessibilityIdentifier = @"mod-translation-bridge-checked";
    });
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

// Observe the real top-level navigation in the opt-in live test. The native
// reload item's test identifier carries the result outside the web subtree,
// so only page content can satisfy the separate visible-listing assertion.
static void configureLiveNexusObservation(void) {
    Class browser = NSClassFromString(@"SKModBrowser");
    void (^committed)(UIViewController *, WKWebView *, WKNavigation *) =
        ^(UIViewController *owner, WKWebView *web, WKNavigation *navigation) {
            (void)navigation;
            NSURL *url = web.URL;
            BOOL listing = [url.scheme.lowercaseString isEqualToString:@"https"] &&
                [@[@"nexusmods.com", @"www.nexusmods.com"] containsObject:url.host.lowercaseString ?: @""] &&
                [@[@"/skyrim/mods", @"/skyrim/mods/", @"/games/skyrim/mods", @"/games/skyrim/mods/"]
                    containsObject:url.path ?: @""];
            owner.navigationItem.rightBarButtonItems.firstObject.accessibilityIdentifier =
                listing ? @"nexus-live-listing-url" : @"nexus-live-other-url";
        };
    void (^failed)(UIViewController *, WKWebView *, WKNavigation *, NSError *) =
        ^(UIViewController *owner, WKWebView *web, WKNavigation *navigation, NSError *error) {
            (void)web; (void)navigation;
            BOOL transport = [error.domain isEqualToString:NSURLErrorDomain];
            if (transport && error.code == NSURLErrorCancelled) return;
            owner.navigationItem.rightBarButtonItems.firstObject.accessibilityIdentifier =
                transport ? @"nexus-live-network-error" : @"nexus-live-navigation-error";
        };
    SEL selectors[] = { @selector(webView:didCommitNavigation:), @selector(webView:didFailProvisionalNavigation:withError:),
                        @selector(webView:didFailNavigation:withError:) };
    IMP observers[] = { imp_implementationWithBlock(committed), imp_implementationWithBlock(failed), imp_implementationWithBlock(failed) };
    for (NSUInteger i = 0; i < sizeof(selectors)/sizeof(selectors[0]); ++i) {
        struct objc_method_description method = protocol_getMethodDescription(@protocol(WKNavigationDelegate), selectors[i], NO, YES);
        if (!method.types || !class_addMethod(browser, selectors[i], observers[i], method.types))
            [NSException raise:NSInternalInconsistencyException format:@"Cannot attach passive Nexus navigation observation"];
    }
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
#ifdef SKYRIM_IOS_CLASSIC_SKSE
        copyFixture([fixtures URLByAppendingPathComponent:@"Authored Save.skse"],
            [profile URLByAppendingPathComponent:@"Documents/My Games/Skyrim/Saves/Authored Save.skse"]);
#endif
        copyFixture(baseline, plugins);
        copyFixture([fixtures URLByAppendingPathComponent:@"initial-mod-state.json"],
            [profile.URLByDeletingLastPathComponent URLByAppendingPathComponent:@"mods-state.json"]);
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
    NSString *view = skyrim_mods_prepare_game(game.path, NO);
    if (!view || !skyrim_mobile_prepare_mods(view.fileSystemRepresentation)) {
        [NSException raise:NSInternalInconsistencyException format:@"Simulator MOD startup failed"];
    }
#if defined(SKYRIM_IOS_CLASSIC_SKSE) && defined(SKYRIM_IOS_SKSE_DLL_PROBE)
    // These authored profiles have not enabled the Classic trial. The
    // acquisition gate must refuse before preparing a private settings store.
    SkyrimPapyrusInputs papyrusInputs;
    int acquisition = skyrim_mods_acquire_papyrus_inputs(&papyrusInputs);
    BOOL hasInputs = papyrusInputs.settings_fd >= 0 || papyrusInputs.game_fd >= 0 ||
                     papyrusInputs.dll || papyrusInputs.pex;
    skyrim_mods_release_papyrus_inputs(&papyrusInputs);
    if (acquisition != ENOENT || hasInputs)
        [NSException raise:NSInternalInconsistencyException format:@"An ordinary launch obtained Papyrus settings inputs"];
#endif
    if ((!reimport && (![view isEqual:game.path] || ![active isEqual:profile.path])) ||
        (reimport && ([view isEqual:game.path] || [active isEqual:profile.path])) ||
        ![[NSData dataWithContentsOfURL:plugins] isEqual:[NSData dataWithContentsOfURL:baseline]]) {
        [NSException raise:NSInternalInconsistencyException format:@"Unexpected game, profile or baseline plugin list"];
    }

    if (reimport) {
        // A legacy sidecar can belong to an earlier generation. The UI must
        // attribute conflicts using the package order captured at launch.
        NSString *legacy = [view stringByAppendingString:@".packages.json"];
        NSError *error = nil;
        if (![@"[]" writeToFile:legacy atomically:YES encoding:NSUTF8StringEncoding error:&error])
            [NSException raise:NSInternalInconsistencyException format:@"Could not stage legacy conflict owners: %@", error];
    }

    // A HUD and a guest render loop are outside this target. Start directly
    // on the shipping Settings table, whose real selection opens the MOD hub.
    UITableViewController *settings = [[NSClassFromString(@"SKSettings") alloc] initWithStyle:UITableViewStyleInsetGrouped];
    if (!settings) [NSException raise:NSInternalInconsistencyException format:@"Missing production Settings class"];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [[UINavigationController alloc] initWithRootViewController:settings];
    [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    UINavigationController *navigation = (UINavigationController *)self.window.rootViewController;
    UITableViewController *settings = (UITableViewController *)navigation.viewControllers.firstObject;
    if (settings.viewIfLoaded.window && ![settings.tableView.accessibilityIdentifier isEqual:@"mod-translation-bridge-checked"])
        checkTranslationBridge(settings);
}
@end

@interface SKFixtureDelegate : UIResponder <UIApplicationDelegate> @end
@implementation SKFixtureDelegate @end
int main(int argc, char **argv) {
    @autoreleasepool {
        configureFixturePicker();
        if ([NSProcessInfo.processInfo.arguments containsObject:@"-ObserveNexusLiveNavigation"])
            configureLiveNexusObservation();
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(SKFixtureDelegate.class));
    }
}
