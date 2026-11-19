/* SPDX-License-Identifier: GPL-3.0-only
 * MOD packages, profiles, launch view, safe mode and sharing.
 *
 * Layout under Application Support/SkyrimIOS:
 *   Mods/<id>/Data/...        immutable imported package files
 *   Mods/<id>/package.json    name, source, digest, plugins, reports
 *   Profile/                  default runtime profile (existing saves)
 *   Profiles/<id>/            other runtime profiles (own saves and INIs)
 *   Views/<profile>/          launch view: clone of the game + packages
 *   mods-state.json           profiles, selections, safe-mode flags
 *   launch.json               last launch record for safe mode
 * No game-memory hooks; the guest only sees a different game folder. */
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include "mod_manager.h"
#ifdef SKYRIM_IOS_CLASSIC_SKSE
#include "PcGuestSkseTrial.h"
#endif
#include "mod_archive.h"
#include "mod_deploy.h"
#include "mod_plugins.h"
#include "mod_analyze.h"
#include "mod_optimize.h"
#include "mod_install.h"
#ifdef SKYRIM_HAVE_LIBARCHIVE
#include "mod_libarchive.h"
#endif

/* Labels and background import notices share the current app language. */
static atomic_int languageCache;
static void RefreshLanguage(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    NSDictionary *s = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"mobile-settings-v1"];
    BOOL japanese = [s[@"language"] isKindOfClass:NSNumber.class] && [s[@"language"] intValue] == 1;
    atomic_store_explicit(&languageCache, japanese, memory_order_relaxed);
    pthread_mutex_unlock(&lock);
}
static NSString *T(NSString *english, NSString *japanese) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [NSNotificationCenter.defaultCenter addObserverForName:NSUserDefaultsDidChangeNotification
            object:NSUserDefaults.standardUserDefaults queue:nil usingBlock:^(NSNotification *note) {
                (void)note;
                RefreshLanguage();
            }];
        RefreshLanguage();
    });
    return atomic_load_explicit(&languageCache, memory_order_relaxed) ? japanese : english;
}

/* ---- paths and state --------------------------------------------------- */
static NSString *Base(void) {
    static NSString *base;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURL *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
        base = [support URLByAppendingPathComponent:@"SkyrimIOS" isDirectory:YES].path;
    });
    return base;
}
static NSString *ModsDir(void) { return [Base() stringByAppendingPathComponent:@"Mods"]; }
static BOOL ValidId(NSString *pid) {
    return [pid isKindOfClass:NSString.class] && pid.length == 36 && [[NSUUID alloc] initWithUUIDString:pid] != nil;
}
static NSString *PackageDir(NSString *pid) { return ValidId(pid) ? [ModsDir() stringByAppendingPathComponent:pid] : nil; }
static NSString *StatePath(void) { return [Base() stringByAppendingPathComponent:@"mods-state.json"]; }
static NSString *LaunchPath(void) { return [Base() stringByAppendingPathComponent:@"launch.json"]; }

static NSMutableDictionary *state;          /* main thread only */
static NSString *activeProfile;             /* this launch */
static NSString *launchNotice;
static BOOL launchWithoutMods, healthyMarked;
static NSArray<NSDictionary *> *launchPlugins;
static NSMutableDictionary *launchSetup;
static BOOL launchHasPackages, launchPluginListFailed;

static id MutableJSON(NSData *data) {
    if (!data) return nil;
    return [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:NULL];
}
static BOOL WriteJSON(id object, NSString *path) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:NULL];
    return data && [data writeToFile:path options:NSDataWritingAtomic error:NULL];
}
static BOOL SaveState(void) { return state && WriteJSON(state, StatePath()); }
static NSMutableArray<NSMutableDictionary *> *Profiles(void) { return state[@"profiles"]; }
static NSMutableDictionary *ProfileById(NSString *pid) {
    for (NSMutableDictionary *p in Profiles()) if ([p[@"id"] isEqual:pid]) return p;
    return nil;
}
static NSMutableDictionary *SelectedProfile(void) { return ProfileById(state[@"next"]) ?: Profiles().firstObject; }
static NSMutableDictionary *RunningProfile(void) { return ProfileById(activeProfile) ?: SelectedProfile(); }
static NSString *ProfileDir(NSDictionary *p) {
    NSString *pid = p[@"id"];
    if ([pid isEqual:@"default"]) return [Base() stringByAppendingPathComponent:@"Profile"];
    return ValidId(pid) ? [[Base() stringByAppendingPathComponent:@"Profiles"] stringByAppendingPathComponent:pid] : nil;
}

/* Published packages are immutable, so their package.json is read once and
 * kept; publishing or deleting a package clears the cache. Any thread. */
static NSMutableDictionary *packageCache;
static NSArray *installedCache;
static uint64_t packageGeneration;
static NSObject *PackageLock(void) {
    static NSObject *lock;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ lock = [NSObject new]; packageCache = [NSMutableDictionary dictionary]; });
    return lock;
}
static void InvalidatePackages(void) {
    @synchronized (PackageLock()) {
        ++packageGeneration;
        [packageCache removeAllObjects];
        installedCache = nil;
    }
}
static NSMutableDictionary *LoadPackage(NSString *pid) {
    if (!ValidId(pid)) return nil;
    for (;;) {
        uint64_t generation;
        @synchronized (PackageLock()) {
            id cached = packageCache[pid];
            if (cached) return cached == NSNull.null ? nil : cached;
            generation = packageGeneration;
        }
        NSMutableDictionary *p = MutableJSON([NSData dataWithContentsOfFile:[PackageDir(pid) stringByAppendingPathComponent:@"package.json"]]);
        if (![p isKindOfClass:NSMutableDictionary.class]) p = nil;
        @synchronized (PackageLock()) {
            if (generation != packageGeneration) continue;
            packageCache[pid] = p ?: (id)NSNull.null;
            return p;
        }
    }
}
static NSArray<NSMutableDictionary *> *InstalledPackages(void) {
    for (;;) {
        uint64_t generation;
        @synchronized (PackageLock()) {
            if (installedCache) return installedCache;
            generation = packageGeneration;
        }
        NSMutableArray *out = [NSMutableArray array];
        for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:ModsDir() error:NULL]) {
            if ([name hasPrefix:@"."]) continue;
            NSMutableDictionary *p = LoadPackage(name);
            if (p) [out addObject:p];
        }
        NSArray *snapshot = [out copy];
        @synchronized (PackageLock()) {
            if (generation != packageGeneration) continue;
            if (!installedCache) installedCache = snapshot;
            return installedCache;
        }
    }
}

static void EnsureState(void) {
    if (state) return;
    NSFileManager *fm = NSFileManager.defaultManager;
    state = MutableJSON([NSData dataWithContentsOfFile:StatePath()]);
    if (![state isKindOfClass:NSMutableDictionary.class] || ![state[@"profiles"] isKindOfClass:NSMutableArray.class]) {
        /* First run: the existing profile and saves become "Default". Plugin
         * choices from the plugin-only version are carried over. */
        NSMutableArray *plugins = [NSMutableArray array];
        id old = [NSUserDefaults.standardUserDefaults arrayForKey:@"mods-v1"];
        if ([old isKindOfClass:NSArray.class]) for (id row in old)
            if ([row isKindOfClass:NSDictionary.class] && [row[@"name"] isKindOfClass:NSString.class])
                [plugins addObject:[@{@"name":row[@"name"], @"on":@([row[@"on"] boolValue])} mutableCopy]];
        state = [@{@"version":@1, @"next":@"default", @"profiles":[@[[@{@"id":@"default", @"name":@"Default",
            @"dir":@"Profile", @"packages":[NSMutableArray array], @"plugins":plugins} mutableCopy]] mutableCopy]} mutableCopy];
    }
    NSMutableArray *validProfiles = [NSMutableArray array];
    for (id p in Profiles())
        if ([p isKindOfClass:NSMutableDictionary.class] && ProfileDir(p) &&
            [p[@"plugins"] isKindOfClass:NSMutableArray.class] && [p[@"packages"] isKindOfClass:NSMutableArray.class])
            [validProfiles addObject:p];
    state[@"profiles"] = validProfiles;
    if (!ProfileById(@"default")) [validProfiles insertObject:[@{@"id":@"default", @"name":@"Default", @"dir":@"Profile",
        @"packages":[NSMutableArray array], @"plugins":[NSMutableArray array]} mutableCopy] atIndex:0];
    /* Drop selections whose package no longer exists. */
    for (NSMutableDictionary *p in Profiles()) {
        NSMutableArray *keep = [NSMutableArray array];
        for (id row in p[@"packages"])
            if ([row isKindOfClass:NSMutableDictionary.class] && ValidId(row[@"id"]) &&
                [fm fileExistsAtPath:[PackageDir(row[@"id"]) stringByAppendingPathComponent:@"package.json"]]) [keep addObject:row];
        p[@"packages"] = keep;
    }
}

/* A disabled package's previous choice must not activate a same-named file
 * from another source. A second enabled package may still provide the name. */
static NSArray<NSDictionary *> *EffectivePlugins(NSDictionary *profile) {
    NSArray *choices = profile[@"plugins"];
    if (!choices.count) return @[];
    NSMutableSet *disabled = [NSMutableSet set], *enabled = [NSMutableSet set];
    for (NSDictionary *row in profile[@"packages"]) {
        NSMutableSet *set = [row[@"on"] boolValue] ? enabled : disabled;
        for (id name in LoadPackage(row[@"id"])[@"plugins"])
            if ([name isKindOfClass:NSString.class]) [set addObject:[name lowercaseString]];
    }
    [disabled minusSet:enabled];
    NSMutableArray *effective = [NSMutableArray array];
    for (id row in choices) {
        if (![row isKindOfClass:NSDictionary.class] || ![row[@"name"] isKindOfClass:NSString.class]) continue;
        BOOL on = [row[@"on"] boolValue] && ![disabled containsObject:[row[@"name"] lowercaseString]];
        [effective addObject:@{@"name":row[@"name"], @"on":@(on)}];
    }
    return effective;
}

static BOOL AnyOn(NSArray *rows) {
    for (NSDictionary *row in rows) if ([row[@"on"] boolValue]) return YES;
    return NO;
}

#ifdef SKYRIM_IOS_CLASSIC_SKSE
static BOOL SkyUITrialSelected(NSDictionary *profile) {
    return [profile[@"skyuiTrial"] isEqual:@YES];
}

BOOL skyrim_mods_prepare_skyui_trial(BOOL skyui_listed) {
    BOOL requested = SkyUITrialSelected(RunningProfile());
    BOOL enabled = requested && !launchWithoutMods && !launchPluginListFailed && skyui_listed;
    if (pc_guest_select_classic_skse(enabled) != 0) return NO;
    if (enabled) launchSetup[@"skyuiTrial"] = @YES;
    if (requested && !enabled && !launchNotice) {
        launchNotice = launchPluginListFailed
            ? T(@"SkyUI is off for this launch because the active plugin list could not be verified.",
                @"起動時のプラグイン一覧を確認できなかったため、今回のSkyUI試験はオフです。")
            : T(@"SkyUI trial is off for this session. Enable SkyUI.esp in this profile and restart.",
                @"今回はSkyUI試験が無効です。このプロファイルでSkyUI.espを有効にして、起動し直してください。");
    }
    fprintf(stderr, "IOS_CLASSIC_SKYUI requested=%d enabled=%d\n", (int)requested, (int)enabled);
    return YES;
}
#endif

/* ---- startup ----------------------------------------------------------- */
static void RestoreLastGoodSetup(NSMutableDictionary *profile) {
    NSDictionary *lastGood = profile[@"lastGood"];
    profile[@"packages"] = [lastGood[@"packages"] mutableCopy];
    profile[@"plugins"] = [lastGood[@"plugins"] mutableCopy];
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    profile[@"skyuiTrial"] = SkyUITrialSelected(lastGood) ? @YES : nil;
#endif
}

static NSInteger AskBeforeLaunch(NSString *title, NSString *message, NSArray<NSString *> *choices) {
    UIWindow *window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class]) { window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene]; break; }
    window.rootViewController = [UIViewController new];
    window.windowLevel = UIWindowLevelAlert;
    [window makeKeyAndVisible];
    __block NSInteger answer = -1;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    for (NSUInteger i = 0; i < choices.count; ++i)
        [alert addAction:[UIAlertAction actionWithTitle:choices[i] style:i ? UIAlertActionStyleDefault : UIAlertActionStyleCancel
                                                handler:^(UIAlertAction *a) { (void)a; answer = (NSInteger)i; }]];
    [window.rootViewController presentViewController:alert animated:NO completion:nil];
    while (answer < 0)
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    window.hidden = YES;
    return answer;
}

NSString *skyrim_mods_select_profile(NSString *defaultProfile) {
    EnsureState();
    NSMutableDictionary *profile = SelectedProfile();
    NSDictionary *last = MutableJSON([NSData dataWithContentsOfFile:LaunchPath()]);
    launchPlugins = EffectivePlugins(profile);
    launchWithoutMods = [state[@"safeNext"] boolValue];
    if (launchWithoutMods) { [state removeObjectForKey:@"safeNext"]; SaveState(); launchNotice = T(@"Started without MODs, as requested.", @"指定どおり、MODなしで起動しました。"); }
    else if ([last isKindOfClass:NSDictionary.class] && [last[@"hadMods"] boolValue] &&
             [last[@"profile"] isEqual:profile[@"id"]] && (AnyOn(profile[@"packages"]) || AnyOn(launchPlugins)) &&
             ![last[@"healthy"] boolValue] && ![last[@"backgrounded"] boolValue]) {
        BOOL canRevert = [profile[@"lastGood"] isKindOfClass:NSDictionary.class];
        NSMutableArray *choices = [@[T(@"Start without MODs (this time)", @"MODなしで起動（今回だけ）")] mutableCopy];
        if (canRevert) [choices addObject:T(@"Restore the last setup that started", @"前回正常に起動した構成に戻す")];
        [choices addObject:T(@"Start as is", @"このまま起動")];
        NSInteger answer = AskBeforeLaunch(T(@"The last launch ended unexpectedly", @"前回の起動が途中で終了しました"),
            T(@"A MOD may be the cause. Do not save while testing without MODs if your save uses them.",
              @"MODが原因の可能性があります。MODを使ったセーブを、MODなしの状態で上書き保存しないでください。"), choices);
        if (answer == 0) {
            launchWithoutMods = YES;
            launchNotice = T(@"Started without MODs. Avoid saving over a modded save.", @"MODなしで起動しました。MODを使ったセーブへの上書き保存は避けてください。");
        } else if (canRevert && answer == 1) {
            RestoreLastGoodSetup(profile);
            launchPlugins = EffectivePlugins(profile);
            SaveState();
            launchNotice = T(@"Restored the last MOD setup that started.", @"前回正常に起動したMOD構成に戻しました。");
        }
    }
    activeProfile = profile[@"id"];
    NSString *dir = [activeProfile isEqual:@"default"] ? defaultProfile : ProfileDir(profile);
    if (![activeProfile isEqual:@"default"] &&
        ![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL]) return nil;
    return dir;
}

static NSString *HexDigest(NSData *data) {
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, d);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) [s appendFormat:@"%02x", d[i]];
    return s;
}
static NSArray<NSString *> *EnabledPackageIds(NSDictionary *profile) {
    NSMutableArray *ids = [NSMutableArray array];
    for (NSDictionary *row in profile[@"packages"])
        if ([row[@"on"] boolValue] && LoadPackage(row[@"id"])) [ids addObject:row[@"id"]];
    return ids;
}

NSString *skyrim_mods_prepare_game(NSString *game) {
    EnsureState();
    NSMutableDictionary *profile = RunningProfile();
    NSArray *ids = launchWithoutMods ? @[] : EnabledPackageIds(profile);
    launchHasPackages = ids.count > 0;
    launchPluginListFailed = NO;
    if (launchWithoutMods) launchPlugins = @[];
    if (!ids.count && !AnyOn(launchPlugins)) {
        /* Consume a previous MOD run once. A fresh/vanilla launch creates
         * no marker and leaves the guest input and chosen profile as-is. */
        if ([NSFileManager.defaultManager fileExistsAtPath:LaunchPath()])
            [NSFileManager.defaultManager removeItemAtPath:LaunchPath() error:NULL];
        launchSetup = nil;
        return game;
    }
    /* Changes made in Settings during this session apply to the next
     * launch. Retain an independent copy of what actually started. */
    launchSetup = [@{@"packages":[[NSArray alloc] initWithArray:profile[@"packages"] copyItems:YES],
                     @"plugins":[[NSArray alloc] initWithArray:profile[@"plugins"] copyItems:YES]} mutableCopy];
    if (!WriteJSON(@{@"profile":profile[@"id"], @"hadMods":@YES, @"healthy":@NO, @"backgrounded":@NO,
                     @"started":@(NSDate.date.timeIntervalSince1970)}, LaunchPath())) {
        launchWithoutMods = YES; launchHasPackages = NO; launchPlugins = @[]; launchSetup = nil;
        launchNotice = T(@"Could not record the MOD setup for recovery. Started without MODs.",
                         @"復旧用のMOD構成を記録できなかったため、MODなしで起動しました。");
        return game;
    }
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        (void)n;
        NSMutableDictionary *l = MutableJSON([NSData dataWithContentsOfFile:LaunchPath()]);
        if ([l isKindOfClass:NSMutableDictionary.class]) { l[@"backgrounded"] = @YES; WriteJSON(l, LaunchPath()); }
    }];
    if (!ids.count) return game;
    uint64_t signature = 0;
    int signatureError = skyrim_tree_signature(game.fileSystemRepresentation, &signature);
    NSString *keySource = [NSString stringWithFormat:@"v1|%llx|%@", (unsigned long long)signature, [ids componentsJoinedByString:@","]];
    NSString *key = HexDigest([keySource dataUsingEncoding:NSUTF8StringEncoding]);
    NSString *views = [Base() stringByAppendingPathComponent:@"Views"];
    [NSFileManager.defaultManager createDirectoryAtPath:views withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *view = [views stringByAppendingPathComponent:profile[@"id"]];
    NSString *conflicts = [view stringByAppendingString:@".conflicts.tsv"];
    if (!signatureError && skyrim_deploy_matches(view.fileSystemRepresentation, key.UTF8String)) return view;
    NSMutableArray *roots = [NSMutableArray array];
    for (NSString *pid in ids) [roots addObject:[PackageDir(pid) stringByAppendingPathComponent:@"Data"]];
    const char **c = calloc(roots.count, sizeof *c);
    if (c) for (NSUInteger i = 0; i < roots.count; ++i) c[i] = [roots[i] fileSystemRepresentation];
    char err[512] = "";
    NSTimeInterval started = NSDate.date.timeIntervalSince1970;
    int r = signatureError ? signatureError : !c ? ENOMEM :
        skyrim_deploy_build(game.fileSystemRepresentation, c, roots.count, view.fileSystemRepresentation,
                            conflicts.fileSystemRepresentation, key.UTF8String, err, sizeof err);
    free(c);
    fprintf(stderr, "IOS_MODS deploy packages=%lu result=%d seconds=%.3f\n", (unsigned long)ids.count, r,
            NSDate.date.timeIntervalSince1970 - started);
    if (r) {
        launchWithoutMods = YES; launchHasPackages = NO; launchPlugins = @[]; launchSetup = nil;
        [NSFileManager.defaultManager removeItemAtPath:LaunchPath() error:NULL];
        launchNotice = T(@"Could not prepare the MODs, so the game started without them.", @"MODを準備できなかったため、MODなしで起動しました。");
        return game;
    }
    WriteJSON(ids, [view stringByAppendingString:@".packages.json"]);
    return view;
}

NSString *skyrim_mods_launch_notice(void) { return launchNotice; }
BOOL skyrim_mods_manages_plugins(void) { return !launchWithoutMods && AnyOn(launchPlugins); }
NSArray<NSDictionary *> *skyrim_mods_launch_plugin_choices(void) { return launchWithoutMods ? @[] : launchPlugins; }
void skyrim_mods_plugin_list_failed(void) { launchPluginListFailed = YES; }
void skyrim_mods_no_plugin_loaded(void) {
    if (launchHasPackages || !launchSetup) return;
    launchSetup = nil;
    [NSFileManager.defaultManager removeItemAtPath:LaunchPath() error:NULL];
}

NSArray<NSDictionary *> *skyrim_mods_plugin_choices(void) {
    EnsureState();
    return [RunningProfile()[@"plugins"] copy] ?: @[];
}
void skyrim_mods_set_plugin_choices(NSArray *rows) {
    EnsureState();
    NSMutableArray *out = [NSMutableArray array];
    for (NSDictionary *row in rows) [out addObject:[@{@"name":row[@"name"], @"on":@([row[@"on"] boolValue])} mutableCopy]];
    RunningProfile()[@"plugins"] = out;
    SaveState();
}

void skyrim_mods_frame(uint64_t frames) {
    if (!launchSetup || healthyMarked || frames < 1800) return;
    healthyMarked = YES;
    NSMutableDictionary *l = MutableJSON([NSData dataWithContentsOfFile:LaunchPath()]);
    if ([l isKindOfClass:NSMutableDictionary.class]) { l[@"healthy"] = @YES; WriteJSON(l, LaunchPath()); }
    if (launchWithoutMods || launchPluginListFailed || !state) return;
    NSMutableDictionary *p = RunningProfile();
    p[@"lastGood"] = launchSetup;
    SaveState();
}

/* ---- import ------------------------------------------------------------ */
static dispatch_queue_t ImportQueue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("SkyrimIOS.mod-import", DISPATCH_QUEUE_SERIAL); });
    return q;
}
static void Notice(NSString *text) { dispatch_async(dispatch_get_main_queue(), ^{ skyrim_mobile_show_notice(text); }); }
static NSString *FileDigest(NSString *path) {
    FILE *f = fopen(path.fileSystemRepresentation, "rb");
    if (!f) return nil;
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
    unsigned char buf[1 << 16]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    fclose(f);
    unsigned char d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &ctx);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) [s appendFormat:@"%02x", d[i]];
    return s;
}
static uint64_t FreeBytes(void) {
    NSDictionary *v = [[NSURL fileURLWithPath:Base()] resourceValuesForKeys:@[NSURLVolumeAvailableCapacityForImportantUsageKey] error:NULL];
    return [v[NSURLVolumeAvailableCapacityForImportantUsageKey] unsignedLongLongValue];
}
static NSArray<NSString *> *PluginsIn(NSString *data) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:data error:NULL]) {
        NSString *ext = name.pathExtension.lowercaseString;
        if ([ext isEqualToString:@"esp"] || [ext isEqualToString:@"esm"]) [out addObject:name];
    }
    return [out sortedArrayUsingSelector:@selector(caseInsensitiveCompare:)];
}

/* Inspect / convert a staged package in place before it is published.
 * Returns NO with *message to refuse the package. */
static unsigned TextureLimit(void) {
    id v = [NSUserDefaults.standardUserDefaults objectForKey:@"mods-texture-max"];
    return [v isKindOfClass:NSNumber.class] ? [v unsignedIntValue] : 2048;
}
static BOOL ProcessStaged(NSString *data, NSMutableDictionary *package, NSString **message) {
    (void)message;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableDictionary *opt = [NSMutableDictionary dictionary];
    BOOL optimize = ![NSUserDefaults.standardUserDefaults boolForKey:@"mods-optimize-off"];
    /* Special Edition archives become loose files the classic engine reads. */
    unsigned extracted = 0, archives = 0;
    for (NSString *name in [fm contentsOfDirectoryAtPath:data error:NULL]) {
        if (!optimize || ![name.pathExtension.lowercaseString isEqualToString:@"bsa"]) continue;
        NSString *path = [data stringByAppendingPathComponent:name];
        NSData *head = [[NSFileHandle fileHandleForReadingAtPath:path] readDataOfLength:8];
        if (head.length < 8 || skyrim_bsa_version(head.bytes, head.length) != 105) continue;
        unsigned files = 0;
        if (!skyrim_bsa_extract_se(path.fileSystemRepresentation, data.fileSystemRepresentation, &files)) {
            [fm removeItemAtPath:path error:NULL]; ++archives; extracted += files;
        }
    }
    opt[@"seArchives"] = @(archives); opt[@"seArchiveFiles"] = @(extracted);
    if (optimize) {
        NSMutableArray<NSString *> *textures = [NSMutableArray array];
        NSDirectoryEnumerator *en = [fm enumeratorAtPath:data];
        for (NSString *rel in en)
            if ([rel.pathExtension.lowercaseString isEqualToString:@"dds"]) [textures addObject:[data stringByAppendingPathComponent:rel]];
        unsigned *counts = calloc(SKYRIM_TEX_ERROR + 1, sizeof *counts);   /* shared with the block */
        unsigned limit = TextureLimit();
        NSObject *lock = [NSObject new];
        NSUInteger total = counts ? textures.count : 0;
        __block NSUInteger done = 0;
        /* At most two textures at once: imports can run while the game is
         * playing, and a decoded 4K texture alone needs 64 MB. */
        dispatch_semaphore_t slots = dispatch_semaphore_create(2);
        dispatch_group_t group = dispatch_group_create();
        dispatch_queue_t workers = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
        for (NSUInteger i = 0; i < total; ++i) {
            dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
            dispatch_group_async(group, workers, ^{
            int action = skyrim_texture_optimize(textures[i].fileSystemRepresentation, limit);
            @synchronized (lock) {
                ++counts[action];
                if (++done % 500 == 0)
                    Notice([NSString stringWithFormat:T(@"Optimizing textures… %lu / %lu", @"テクスチャを最適化中… %lu / %lu"), (unsigned long)done, (unsigned long)total]);
            }
            dispatch_semaphore_signal(slots);
            });
        }
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
        if (!counts) counts = calloc(SKYRIM_TEX_ERROR + 1, sizeof *counts);
        if (!counts) return YES;
        opt[@"trimmed"] = @(counts[SKYRIM_TEX_TRIMMED]); opt[@"reheadered"] = @(counts[SKYRIM_TEX_REHEADERED]);
        opt[@"transcoded"] = @(counts[SKYRIM_TEX_TRANSCODED]); opt[@"unsupported"] = @(counts[SKYRIM_TEX_UNSUPPORTED]);
        opt[@"failed"] = @(counts[SKYRIM_TEX_ERROR]); opt[@"limit"] = @(limit);
        free(counts);
    }
    package[@"optimized"] = opt;
    SkyrimModReport r;
    skyrim_mod_analyze(data.fileSystemRepresentation, &r);
    package[@"report"] = @{@"plugins":@(r.plugins), @"sePlugins":@(r.se_plugins), @"esl":@(r.esl), @"bsas":@(r.bsas),
        @"seBsas":@(r.se_bsas), @"foreignBsas":@(r.foreign_bsas), @"textures":@(r.textures), @"dx10":@(r.dx10_textures),
        @"bc7":@(r.bc7_textures), @"tex4k":@(r.textures_4k), @"noMips":@(r.textures_no_mips), @"maxTexture":@(r.max_texture),
        @"textureMB":@(r.texture_bytes >> 20), @"meshes":@(r.meshes), @"seMeshes":@(r.se_meshes), @"foreignMeshes":@(r.foreign_meshes),
        @"scripts":@(r.scripts), @"skseScripts":@(r.skse_scripts), @"mcm":@(r.mcm_scripts), @"skseDlls":@(r.skse_dlls),
        @"skseMissing":@(r.skse_missing)};
    return YES;
}

/* Publish a staged package (on the import queue) and select it on the main
 * thread. Packages are immutable once published. */
static void PublishStaged(NSString *staging, NSMutableDictionary *package) {
    NSString *data = [staging stringByAppendingPathComponent:@"Data"];
    NSString *message = nil;
    if (!ProcessStaged(data, package, &message)) {
        skyrim_remove_tree(staging.fileSystemRepresentation);
        Notice(message ?: T(@"This MOD cannot be used.", @"このMODは使えません。"));
        return;
    }
    package[@"plugins"] = PluginsIn(data);
    if (!package[@"files"]) {
        unsigned long long files = 0, bytes = 0;
        NSDirectoryEnumerator *en = [NSFileManager.defaultManager enumeratorAtPath:data];
        for (NSString *rel in en) { (void)rel; if ([en.fileAttributes[NSFileType] isEqual:NSFileTypeRegular]) { ++files; bytes += en.fileAttributes.fileSize; } }
        package[@"files"] = @(files); package[@"bytes"] = @(bytes);
    }
    NSString *pid = package[@"id"];
    if (!WriteJSON(package, [staging stringByAppendingPathComponent:@"package.json"]) ||
        rename(staging.fileSystemRepresentation, PackageDir(pid).fileSystemRepresentation)) {
        skyrim_remove_tree(staging.fileSystemRepresentation);
        Notice(T(@"Could not save the MOD.", @"MODを保存できませんでした。"));
        return;
    }
    InvalidatePackages();
    dispatch_async(dispatch_get_main_queue(), ^{
        EnsureState();
        NSMutableDictionary *p = SelectedProfile();
        [p[@"packages"] addObject:[@{@"id":pid, @"on":@YES} mutableCopy]];
        NSMutableArray *plugins = p[@"plugins"];
        for (NSString *name in package[@"plugins"]) {
            BOOL found = NO;
            for (NSMutableDictionary *row in plugins) if ([row[@"name"] caseInsensitiveCompare:name] == NSOrderedSame) { row[@"on"] = @YES; found = YES; }
            if (!found) [plugins addObject:[@{@"name":name, @"on":@YES} mutableCopy]];
        }
        SaveState();
        [NSNotificationCenter.defaultCenter postNotificationName:@"SKModsChanged" object:nil];
        skyrim_mobile_show_notice([NSString stringWithFormat:T(@"Added %@. Save your game, then fully close and reopen the app.",
            @"%@ を追加しました。ゲーム内で保存し、アプリを完全終了して開き直すと反映します。"), package[@"name"]]);
    });
}
static NSString *NewStaging(void) {
    NSString *s = [ModsDir() stringByAppendingPathComponent:[@".staging-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    if (![NSFileManager.defaultManager createDirectoryAtPath:[s stringByAppendingPathComponent:@"Data"] withIntermediateDirectories:YES attributes:nil error:NULL]) return nil;
    [[NSURL fileURLWithPath:ModsDir() isDirectory:YES] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:NULL];
    return s;
}
static NSString *ZipErrorText(int status) {
    switch (status) {
    case SKYRIM_ZIP_NOT_ZIP: return T(@"This is not a zip file.", @"zipファイルではありません。");
    case SKYRIM_ZIP_UNSAFE_PATH: case SKYRIM_ZIP_LINK: return T(@"The archive contains unsafe file paths.", @"安全でないファイルパスが含まれています。");
    case SKYRIM_ZIP_ENCRYPTED: return T(@"Password-protected archives are not supported.", @"パスワード付きの圧縮ファイルには対応していません。");
    case SKYRIM_ZIP_UNSUPPORTED: return T(@"This zip uses an unsupported compression method.", @"非対応の圧縮方式のzipです。");
    case SKYRIM_ZIP_DUPLICATE: return T(@"The archive has files that differ only in capitals.", @"大文字・小文字だけ違う同名ファイルが含まれています。");
    case SKYRIM_ZIP_TOO_LARGE: return T(@"The archive is too large.", @"圧縮ファイルが大きすぎます。");
    default: return [NSString stringWithFormat:T(@"Could not read the archive (%s).", @"圧縮ファイルを読めませんでした（%s）。"), skyrim_zip_error_name(status)];
    }
}

/* ---- installers (FOMOD) ---------------------------------------------- */
static NSString *FindModuleConfig(NSString *root) {
    NSDirectoryEnumerator *en = [NSFileManager.defaultManager enumeratorAtPath:root];
    for (NSString *rel in en) {
        if (en.level > 4) { [en skipDescendants]; continue; }
        if ([rel.lastPathComponent caseInsensitiveCompare:@"ModuleConfig.xml"] == NSOrderedSame &&
            [rel.stringByDeletingLastPathComponent.lastPathComponent caseInsensitiveCompare:@"fomod"] == NSOrderedSame)
            return [root stringByAppendingPathComponent:rel];
    }
    return nil;
}
/* raw holds the whole extracted archive (inside staging). Shows the installer
 * on the main thread; publishes on the import queue when it finishes. */
static void RunInstallerFromRaw(NSString *staging, NSString *raw, NSMutableDictionary *package) {
    NSString *config = FindModuleConfig(raw);
    if (!config) {
        skyrim_remove_tree(staging.fileSystemRepresentation);
        Notice(T(@"The installer file was not found.", @"インストーラーの設定ファイルが見つかりませんでした。"));
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        EnsureState();
        NSMutableSet *active = [NSMutableSet setWithObjects:@"skyrim.esm", @"update.esm", nil];
        for (NSDictionary *row in RunningProfile()[@"plugins"]) if ([row[@"on"] boolValue]) [active addObject:[row[@"name"] lowercaseString]];
        skyrim_fomod_run(package[@"name"] ?: package[@"archive"] ?: @"MOD", raw, config, [staging stringByAppendingPathComponent:@"Data"], active, ^(BOOL installed, NSString *message) {
            dispatch_async(ImportQueue(), ^{
                skyrim_remove_tree(raw.fileSystemRepresentation);
                if (!installed) { skyrim_remove_tree(staging.fileSystemRepresentation); if (message) Notice(message); return; }
                package[@"installer"] = @YES;
                PublishStaged(staging, package);
            });
        });
    });
}
static BOOL RunInstaller(SkyrimZip *zip, NSString *archive, NSMutableDictionary *package) {
    (void)archive;
    NSString *staging = NewStaging();
    if (!staging) return NO;
    NSString *raw = [staging stringByAppendingPathComponent:@"raw"];
    [NSFileManager.defaultManager createDirectoryAtPath:raw withIntermediateDirectories:YES attributes:nil error:NULL];
    int status = SKYRIM_ZIP_OK;
    for (size_t i = 0; i < zip->count && !status; ++i)
        if (!zip->entries[i].is_dir) status = skyrim_zip_extract(zip, i, raw.fileSystemRepresentation, zip->entries[i].path);
    if (status) { skyrim_remove_tree(staging.fileSystemRepresentation); Notice(ZipErrorText(status)); return YES; }
    RunInstallerFromRaw(staging, raw, package);
    return YES;
}

/* 7z / rar through libarchive, when the build provides it. */
static BOOL ImportOther(NSString *path, NSString *name, NSString *digest) {
    NSString *ext = name.pathExtension.lowercaseString;
    if (![ext isEqualToString:@"7z"] && ![ext isEqualToString:@"rar"]) return NO;
#ifdef SKYRIM_HAVE_LIBARCHIVE
    SkyrimArchiveList list;
    int status = skyrim_archive_list(path.fileSystemRepresentation, &list);
    if (status) { Notice(ZipErrorText(status)); return YES; }
    if (list.total + (256ull << 20) > FreeBytes()) {
        skyrim_archive_list_free(&list);
        Notice(T(@"Not enough free space on this iPhone for this MOD.", @"このMODを入れるための空き容量が足りません。"));
        return YES;
    }
    char prefix[1024]; int installer = 0;
    int found = skyrim_mod_data_root((const char *const *)list.paths, list.count, prefix, sizeof prefix, &installer);
    skyrim_archive_list_free(&list);
    NSMutableDictionary *package = [@{@"id":NSUUID.UUID.UUIDString, @"name":name.stringByDeletingPathExtension,
        @"archive":name, @"sha256":digest ?: @"", @"imported":@(NSDate.date.timeIntervalSince1970)} mutableCopy];
    NSString *staging = NewStaging();
    if (!staging) { Notice(T(@"Could not save the MOD.", @"MODを保存できませんでした。")); return YES; }
    size_t files = 0; uint64_t bytes = 0;
    if (installer) {
        NSString *raw = [staging stringByAppendingPathComponent:@"raw"];
        [NSFileManager.defaultManager createDirectoryAtPath:raw withIntermediateDirectories:YES attributes:nil error:NULL];
        status = skyrim_archive_extract(path.fileSystemRepresentation, "", 0, raw.fileSystemRepresentation, &files, &bytes);
        if (status) { skyrim_remove_tree(staging.fileSystemRepresentation); Notice(ZipErrorText(status)); return YES; }
        RunInstallerFromRaw(staging, raw, package);
        return YES;
    }
    if (!found) {
        skyrim_remove_tree(staging.fileSystemRepresentation);
        Notice(T(@"No Skyrim game files were found in this archive.", @"この圧縮ファイルにSkyrimのゲーム用ファイルが見つかりませんでした。"));
        return YES;
    }
    status = skyrim_archive_extract(path.fileSystemRepresentation, prefix, 1,
                                    [staging stringByAppendingPathComponent:@"Data"].fileSystemRepresentation, &files, &bytes);
    if (status) { skyrim_remove_tree(staging.fileSystemRepresentation); Notice(ZipErrorText(status)); return YES; }
    package[@"files"] = @(files); package[@"bytes"] = @(bytes); package[@"dataPrefix"] = @(prefix);
    PublishStaged(staging, package);
    return YES;
#else
    (void)path; (void)digest;
    Notice(T(@"This build cannot open 7z or RAR archives. Choose a ZIP file or folder.",
             @"このビルドは7z・RAR形式に対応していません。ZIPファイルまたはフォルダを選んでください。"));
    return YES;
#endif
}

static void ImportZip(NSString *path, NSString *name, NSString *digest) {
    SkyrimZip zip;
    int status = skyrim_zip_open(path.fileSystemRepresentation, &zip);
    if (status) { Notice(ZipErrorText(status)); return; }
    NSMutableDictionary *package = [@{@"id":NSUUID.UUID.UUIDString, @"name":name.stringByDeletingPathExtension,
        @"archive":name, @"sha256":digest ?: @"", @"imported":@(NSDate.date.timeIntervalSince1970)} mutableCopy];
    if ((uint64_t)zip.total + (256ull << 20) > FreeBytes()) {
        skyrim_zip_close(&zip);
        Notice(T(@"Not enough free space on this iPhone for this MOD.", @"このMODを入れるための空き容量が足りません。"));
        return;
    }
    const char **paths = calloc(zip.count ? zip.count : 1, sizeof *paths);
    size_t n = 0;
    for (size_t i = 0; i < zip.count; ++i) if (!zip.entries[i].is_dir) paths[n++] = zip.entries[i].path;
    char prefix[1024]; int installer = 0;
    int found = skyrim_mod_data_root(paths, n, prefix, sizeof prefix, &installer);
    free(paths);
    if (installer && RunInstaller(&zip, path, package)) { skyrim_zip_close(&zip); return; }
    if (!found) {
        skyrim_zip_close(&zip);
        Notice(installer ? T(@"This MOD needs its installer, which is not available in this version.", @"このMODはインストーラーが必要ですが、この版では使えません。")
                         : T(@"No Skyrim game files were found in this archive.", @"この圧縮ファイルにSkyrimのゲーム用ファイルが見つかりませんでした。"));
        return;
    }
    NSString *staging = NewStaging();
    if (!staging) { skyrim_zip_close(&zip); Notice(T(@"Could not save the MOD.", @"MODを保存できませんでした。")); return; }
    NSString *data = [staging stringByAppendingPathComponent:@"Data"];
    size_t plen = strlen(prefix), files = 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i < zip.count && !status; ++i) {
        SkyrimZipEntry *e = &zip.entries[i];
        if (e->is_dir || strncmp(e->path, prefix, plen)) continue;
        const char *rel = e->path + plen;
        if (!strncasecmp(rel, "fomod/", 6)) continue;
        status = skyrim_zip_extract(&zip, i, data.fileSystemRepresentation, rel);
        ++files; bytes += e->size;
    }
    skyrim_zip_close(&zip);
    if (status) { skyrim_remove_tree(staging.fileSystemRepresentation); Notice(ZipErrorText(status)); return; }
    package[@"files"] = @(files); package[@"bytes"] = @(bytes);
    package[@"dataPrefix"] = [NSString stringWithUTF8String:prefix];
    PublishStaged(staging, package);
}

/* Import a folder chosen in the Files app (copied; the source is unchanged). */
static void ImportFolder(NSString *folder) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableArray<NSString *> *rels = [NSMutableArray array];
    NSDirectoryEnumerator *en = [fm enumeratorAtPath:folder];
    for (NSString *rel in en)
        if ([en.fileAttributes[NSFileType] isEqual:NSFileTypeRegular]) [rels addObject:rel];
    const char **paths = calloc(rels.count ? rels.count : 1, sizeof *paths);
    for (NSUInteger i = 0; i < rels.count; ++i) paths[i] = rels[i].UTF8String;
    char prefix[1024]; int installer = 0;
    int found = skyrim_mod_data_root(paths, rels.count, prefix, sizeof prefix, &installer);
    free(paths);
    if (!found) { Notice(T(@"No Skyrim game files were found in this folder.", @"このフォルダにSkyrimのゲーム用ファイルが見つかりませんでした。")); return; }
    NSString *staging = NewStaging();
    NSString *data = [staging stringByAppendingPathComponent:@"Data"];
    NSString *p = [NSString stringWithUTF8String:prefix];
    uint64_t bytes = 0; NSUInteger files = 0;
    for (NSString *rel in rels) {
        if (![rel hasPrefix:p]) continue;
        NSString *inner = [rel substringFromIndex:p.length];
        char safe[1024];
        if (skyrim_zip_normalize(inner.UTF8String, strlen(inner.UTF8String), 1, safe, sizeof safe) != 1) continue;
        if (!strncasecmp(safe, "fomod/", 6)) continue;
        if (skyrim_make_parents(data.fileSystemRepresentation, safe) ||
            skyrim_clone_file([folder stringByAppendingPathComponent:rel].fileSystemRepresentation,
                              [data stringByAppendingPathComponent:@(safe)].fileSystemRepresentation)) {
            skyrim_remove_tree(staging.fileSystemRepresentation);
            Notice(T(@"Could not copy the folder.", @"フォルダをコピーできませんでした。"));
            return;
        }
        bytes += [[fm attributesOfItemAtPath:[folder stringByAppendingPathComponent:rel] error:NULL] fileSize];
        ++files;
    }
    NSMutableDictionary *package = [@{@"id":NSUUID.UUID.UUIDString, @"name":folder.lastPathComponent, @"archive":folder.lastPathComponent,
        @"sha256":@"", @"files":@(files), @"bytes":@(bytes), @"dataPrefix":p, @"imported":@(NSDate.date.timeIntervalSince1970)} mutableCopy];
    PublishStaged(staging, package);
}

static void ImportModList(NSString *path);

void skyrim_mods_import_url(NSURL *url, BOOL scoped) {
    BOOL accessing = scoped && [url startAccessingSecurityScopedResource];
    /* Copy into the app first, so the import continues after the share sheet
     * or picker releases the original. */
    NSString *inbox = [Base() stringByAppendingPathComponent:@"Inbox"];
    [NSFileManager.defaultManager createDirectoryAtPath:inbox withIntermediateDirectories:YES attributes:nil error:NULL];
    NSNumber *isDir = nil;
    [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:NULL];
    NSString *name = url.lastPathComponent ?: @"mod.zip";
    if (isDir.boolValue) {
        NSString *source = url.path;
        Notice(T(@"Adding MOD folder…", @"MODフォルダを追加しています…"));
        dispatch_async(ImportQueue(), ^{
            ImportFolder(source);
            if (accessing) [url stopAccessingSecurityScopedResource];
        });
        return;
    }
    NSString *copy = [inbox stringByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingPathExtension:name.pathExtension]];
    NSError *error = nil;
    BOOL copied = [NSFileManager.defaultManager copyItemAtPath:url.path toPath:copy error:&error];
    if (accessing) [url stopAccessingSecurityScopedResource];
    if (!copied) { Notice(T(@"Could not open the shared file.", @"共有されたファイルを開けませんでした。")); return; }
    Notice([NSString stringWithFormat:T(@"Adding %@…", @"%@ を追加しています…"), name]);
    dispatch_async(ImportQueue(), ^{
        NSString *ext = name.pathExtension.lowercaseString;
        if ([ext isEqualToString:@"json"]) dispatch_sync(dispatch_get_main_queue(), ^{ ImportModList(copy); });
        else {
            NSString *digest = FileDigest(copy);
            NSString *dup = nil;
            for (NSDictionary *p in InstalledPackages()) if (digest.length && [p[@"sha256"] isEqual:digest]) dup = p[@"name"];
            if (dup) Notice([NSString stringWithFormat:T(@"%@ is already added.", @"%@ は追加済みです。"), dup]);
            else if ([ext isEqualToString:@"zip"]) ImportZip(copy, name, digest);
            else {
                if (!ImportOther(copy, name, digest)) Notice(T(@"This file type is not supported.", @"このファイル形式には対応していません。"));
            }
        }
        [NSFileManager.defaultManager removeItemAtPath:copy error:NULL];
    });
}

/* Files shared with "Open in SkyrimIOS". SDL owns the application delegate;
 * its open-URL method is replaced once at load time so early cold-launch
 * URLs are not lost as SDL drop events. */
static BOOL SKOpenURL(id self, SEL cmd, UIApplication *app, NSURL *url, NSDictionary *options) {
    (void)self; (void)cmd; (void)app; (void)options;
    skyrim_mods_import_url(url, YES);
    return YES;
}
@interface SKModURLHook : NSObject @end
@implementation SKModURLHook
+ (void)load {
    Class c = NSClassFromString(@"SDLUIKitDelegate");
    if (c) class_replaceMethod(c, @selector(application:openURL:options:), (IMP)SKOpenURL, "B@:@@@");
}
@end

/* ---- MOD list export / import ----------------------------------------- */
static NSURL *ExportModList(NSDictionary *profile) {
    NSMutableArray *packages = [NSMutableArray array];
    for (NSDictionary *row in profile[@"packages"]) {
        NSDictionary *p = LoadPackage(row[@"id"]);
        if (p) [packages addObject:@{@"name":p[@"name"] ?: @"", @"archive":p[@"archive"] ?: @"", @"sha256":p[@"sha256"] ?: @"",
            @"bytes":p[@"bytes"] ?: @0, @"source":p[@"source"] ?: @"", @"on":row[@"on"] ?: @NO}];
    }
    NSDictionary *doc = @{@"format":@"skyrimios-modlist", @"version":@1, @"name":profile[@"name"] ?: @"",
                          @"packages":packages, @"plugins":profile[@"plugins"] ?: @[]};
    NSString *safe = [[profile[@"name"] componentsSeparatedByCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"/\\:*?\"<>|"]] componentsJoinedByString:@"_"];
    NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.skyrimios-modlist.json", safe]];
    return WriteJSON(doc, path) ? [NSURL fileURLWithPath:path] : nil;
}
static void ImportModList(NSString *path) {
    NSDictionary *doc = MutableJSON([NSData dataWithContentsOfFile:path]);
    if (![doc isKindOfClass:NSDictionary.class] || ![doc[@"format"] isEqual:@"skyrimios-modlist"]) {
        skyrim_mobile_show_notice(T(@"This is not a SkyrimIOS MOD list.", @"SkyrimIOSのMOD構成ファイルではありません。"));
        return;
    }
    EnsureState();
    NSArray *installed = InstalledPackages();
    NSMutableArray *rows = [NSMutableArray array], *missing = [NSMutableArray array];
    for (NSDictionary *want in doc[@"packages"]) {
        if (![want isKindOfClass:NSDictionary.class]) continue;
        NSDictionary *match = nil;
        for (NSDictionary *p in installed) if ([want[@"sha256"] length] && [p[@"sha256"] isEqual:want[@"sha256"]]) match = p;
        if (!match) for (NSDictionary *p in installed) if ([p[@"archive"] isEqual:want[@"archive"]]) match = p;
        if (match) [rows addObject:[@{@"id":match[@"id"], @"on":@([want[@"on"] boolValue])} mutableCopy]];
        else [missing addObject:[want[@"name"] description] ?: @"?"];
    }
    NSMutableArray *plugins = [NSMutableArray array];
    for (NSDictionary *row in doc[@"plugins"])
        if ([row isKindOfClass:NSDictionary.class] && [row[@"name"] isKindOfClass:NSString.class])
            [plugins addObject:[@{@"name":row[@"name"], @"on":@([row[@"on"] boolValue])} mutableCopy]];
    NSString *pid = NSUUID.UUID.UUIDString;
    NSString *name = [NSString stringWithFormat:@"%@ (%@)", [doc[@"name"] description] ?: @"MOD", T(@"imported", @"読み込み")];
    [Profiles() addObject:[@{@"id":pid, @"name":name, @"dir":[@"Profiles" stringByAppendingPathComponent:pid],
                             @"packages":rows, @"plugins":plugins} mutableCopy]];
    SaveState();
    [NSNotificationCenter.defaultCenter postNotificationName:@"SKModsChanged" object:nil];
    skyrim_mobile_show_notice(missing.count ?
        [NSString stringWithFormat:T(@"Created profile \"%@\". Missing MODs: %@", @"プロファイル「%@」を作成しました。足りないMOD：%@"), name, [missing componentsJoinedByString:@", "]] :
        [NSString stringWithFormat:T(@"Created profile \"%@\" with all MODs.", @"プロファイル「%@」を作成しました（MODはすべてそろっています）。"), name]);
}

/* ---- conflicts --------------------------------------------------------- */
static NSArray<NSDictionary *> *ConflictSummary(void) {
    NSString *view = [[Base() stringByAppendingPathComponent:@"Views"] stringByAppendingPathComponent:activeProfile ?: @""];
    NSArray *ids = MutableJSON([NSData dataWithContentsOfFile:[view stringByAppendingString:@".packages.json"]]);
    NSString *text = [NSString stringWithContentsOfFile:[view stringByAppendingString:@".conflicts.tsv"] encoding:NSUTF8StringEncoding error:NULL];
    if (![ids isKindOfClass:NSArray.class] || !text) return @[];
    NSMutableDictionary<NSString *, NSMutableArray *> *groups = [NSMutableDictionary dictionary];
    /* Names are looked up once per package, not once per conflicting file. */
    NSMutableArray<NSString *> *names = [NSMutableArray arrayWithCapacity:ids.count];
    for (id pid in ids) [names addObject:([pid isKindOfClass:NSString.class] ? LoadPackage(pid)[@"name"] : nil) ?: @"?"];
    NSString *original = T(@"Original game", @"元のゲーム");
    NSString *(^owner)(int) = ^NSString *(int i) { return i < 0 || i >= (int)names.count ? original : names[i]; };
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSArray *f = [line componentsSeparatedByString:@"\t"];
        if (f.count != 3) continue;
        NSString *key = [NSString stringWithFormat:@"%@\t%@", owner([f[2] intValue]), owner([f[1] intValue])];
        if (!groups[key]) groups[key] = [NSMutableArray array];
        [groups[key] addObject:f[0]];
    }
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *key in [groups.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        NSArray *names = [key componentsSeparatedByString:@"\t"];
        [out addObject:@{@"winner":names[0], @"loser":names[1], @"files":groups[key]}];
    }
    return out;
}

/* ---- save consistency ---------------------------------------------------- */
/* Saves of the running profile, newest first, with plugins they list that
 * this launch does not load. Reads only the save header. */
static NSArray<NSDictionary *> *SaveChecksIn(NSString *dir, NSSet<NSString *> *loaded, NSUInteger limit);
static NSString *SavesDir(void) { return [ProfileDir(RunningProfile()) stringByAppendingPathComponent:@"Documents/My Games/Skyrim/Saves"]; }
static NSSet<NSString *> *LoadedPluginSet(void) {
    NSMutableSet *loaded = [NSMutableSet set];
    for (NSString *n in skyrim_mobile_loaded_plugins()) [loaded addObject:n.lowercaseString];
    return loaded;
}
static NSArray<NSDictionary *> *SaveChecks(NSUInteger limit) { return SaveChecksIn(SavesDir(), LoadedPluginSet(), limit); }
/* Any thread: only reads files. */
static NSArray<NSDictionary *> *SaveChecksIn(NSString *dir, NSSet<NSString *> *loaded, NSUInteger limit) {
    NSMutableArray *saves = [NSMutableArray array];
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:NULL]) {
        if (![name.pathExtension.lowercaseString isEqualToString:@"ess"]) continue;
        NSDictionary *a = [NSFileManager.defaultManager attributesOfItemAtPath:[dir stringByAppendingPathComponent:name] error:NULL];
        if (a[NSFileModificationDate]) [saves addObject:@{@"name":name, @"date":a[NSFileModificationDate]}];
    }
    [saves sortUsingComparator:^NSComparisonResult(NSDictionary *x, NSDictionary *y) { return [y[@"date"] compare:x[@"date"]]; }];
    NSMutableArray *out = [NSMutableArray array];
    for (NSDictionary *save in saves) {
        if (out.count >= limit) break;
        char *names = NULL; unsigned count = 0;
        NSString *path = [dir stringByAppendingPathComponent:save[@"name"]];
        if (skyrim_save_plugins(path.fileSystemRepresentation, &names, &count)) {
            [out addObject:@{@"name":save[@"name"], @"date":save[@"date"], @"unreadable":@YES}];
            continue;
        }
        NSMutableArray *missing = [NSMutableArray array];
        for (unsigned i = 0, o = 0; i < count; ++i) {
            NSString *n = [[NSString alloc] initWithBytes:names + o length:strlen(names + o) encoding:NSWindowsCP1252StringEncoding] ?: @"?";
            if (![loaded containsObject:n.lowercaseString]) [missing addObject:n];
            o += (unsigned)strlen(names + o) + 1;
        }
        free(names);
        [out addObject:@{@"name":save[@"name"], @"date":save[@"date"], @"missing":missing}];
    }
    return out;
}
NSString *skyrim_mods_save_notice(void) {
    if (!launchSetup) return nil;
    NSDictionary *latest = SaveChecks(1).firstObject;
    NSArray *missing = latest[@"missing"];
    if (!missing.count) return nil;
    return [NSString stringWithFormat:T(@"Your latest save uses MODs that are not loaded now: %@. Load it only if you meant to remove them.",
        @"最新のセーブは、今は読み込まれていないMODを使っています：%@。外すつもりの場合だけ、そのセーブを読み込んでください。"),
        [missing componentsJoinedByString:@", "]];
}

@interface SKSaves : UITableViewController
@property(nonatomic, strong) NSArray<NSDictionary *> *saves;
@end
@implementation SKSaves
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = T(@"Save check", @"セーブの確認"); self.saves = nil;
    NSString *dir = SavesDir(); NSSet *loaded = LoadedPluginSet();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray *saves = SaveChecksIn(dir, loaded, 50);
        dispatch_async(dispatch_get_main_queue(), ^{ self.saves = saves; [self.tableView reloadData]; });
    });
}
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { (void)t; (void)s; return self.saves.count ? (NSInteger)self.saves.count : 1; }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s {
    (void)t; (void)s;
    return T(@"Compared with the plugins loaded in this launch. Loading a save without a MOD it used removes that MOD's items and changes from the save once you save again.",
             @"今回の起動で読み込んだプラグインと比べています。使っていたMODがない状態で読み込んで保存すると、そのMODの物や変化がセーブから消えます。");
}
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    (void)t;
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    c.selectionStyle = UITableViewCellSelectionStyleNone; c.detailTextLabel.numberOfLines = 0;
    if (!self.saves.count) { c.textLabel.text = self.saves ? T(@"No saves in this profile", @"このプロファイルにセーブはありません") : T(@"Loading…", @"読み込み中…"); return c; }
    NSDictionary *save = self.saves[p.row];
    c.textLabel.text = [save[@"name"] stringByDeletingPathExtension];
    NSArray *missing = save[@"missing"];
    c.detailTextLabel.text = [save[@"unreadable"] boolValue] ? T(@"Could not read this save", @"このセーブは読めませんでした") :
        missing.count ? [NSString stringWithFormat:T(@"⚠️ Missing: %@", @"⚠️ 足りないMOD：%@"), [missing componentsJoinedByString:@", "]] : T(@"✅ All MODs present", @"✅ MODはすべてそろっています");
    return c;
}
@end

/* ---- UI ---------------------------------------------------------------- */
/* Subtitle cell from the table's reuse pool, reset to a neutral state. */
static UITableViewCell *ReusableCell(UITableView *t) {
    UITableViewCell *c = [t dequeueReusableCellWithIdentifier:@"SKModCell"];
    if (!c) c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"SKModCell"];
    c.textLabel.text = nil; c.detailTextLabel.text = nil; c.imageView.image = nil;
    c.textLabel.numberOfLines = 1; c.detailTextLabel.numberOfLines = 1; c.textLabel.enabled = YES;
    c.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    c.accessoryView = nil; c.accessoryType = UITableViewCellAccessoryNone;
    c.selectionStyle = UITableViewCellSelectionStyleDefault;
    return c;
}
@interface SKListController : UITableViewController
@property(nonatomic, copy) NSString *footer;
@property(nonatomic, strong) NSArray<NSString *> *lines;
@end
@implementation SKListController
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { (void)t; (void)s; return (NSInteger)self.lines.count; }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s { (void)t; (void)s; return self.footer; }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    (void)t;
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    c.textLabel.text = self.lines[p.row]; c.textLabel.numberOfLines = 0;
    c.textLabel.font = [UIFont systemFontOfSize:14]; c.selectionStyle = UITableViewCellSelectionStyleNone;
    return c;
}
@end

@interface SKConflicts : UITableViewController
@property(nonatomic, strong) NSArray<NSDictionary *> *groups;
@end
@implementation SKConflicts
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = T(@"Conflicts", @"競合"); self.groups = nil;
    /* Large MOD sets can list tens of thousands of files: read off the main thread. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray *groups = ConflictSummary();
        dispatch_async(dispatch_get_main_queue(), ^{ self.groups = groups; [self.tableView reloadData]; });
    });
}
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { (void)t; (void)s; return self.groups.count ? (NSInteger)self.groups.count : 1; }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s {
    (void)t; (void)s;
    return T(@"Files from MODs lower in the MOD list replace the same files from MODs above them. Original .esm and .bsa files are never replaced. This shows the current launch.",
             @"MOD一覧で下にあるMODのファイルが、上のMODの同じファイルを上書きします。元の.esmと.bsaは上書きされません。今回の起動時の状態です。");
}
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    (void)t;
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    if (!self.groups.count) {
        c.textLabel.text = self.groups ? T(@"No conflicts", @"競合はありません") : T(@"Loading…", @"読み込み中…");
        c.selectionStyle = UITableViewCellSelectionStyleNone; return c;
    }
    NSDictionary *g = self.groups[p.row];
    c.textLabel.text = [NSString stringWithFormat:T(@"%@ replaces %@", @"%@ が %@ を上書き"), g[@"winner"], g[@"loser"]];
    c.textLabel.numberOfLines = 2;
    c.detailTextLabel.text = [NSString stringWithFormat:T(@"%lu files", @"%lu個のファイル"), (unsigned long)[g[@"files"] count]];
    c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return c;
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)p {
    [t deselectRowAtIndexPath:p animated:YES];
    if (!self.groups.count) return;
    SKListController *list = [[SKListController alloc] initWithStyle:UITableViewStyleInsetGrouped];
    NSArray *files = self.groups[p.row][@"files"];
    list.lines = files.count > 500 ? [files subarrayWithRange:NSMakeRange(0, 500)] : files;
    list.title = T(@"Files", @"ファイル");
    [self.navigationController pushViewController:list animated:YES];
}
@end

@interface SKProfiles : UITableViewController @end
@implementation SKProfiles
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = T(@"Profiles", @"プロファイル");
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(add)];
}
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { (void)t; (void)s; return (NSInteger)Profiles().count; }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s {
    (void)t; (void)s;
    return T(@"The checked profile is used from the next launch. Each profile has its own MODs, plugin order, settings files and saves. Swipe to rename or delete.",
             @"チェックしたプロファイルが次回起動から使われます。プロファイルごとにMOD・プラグインの並び・設定ファイル・セーブが別々です。左にスワイプで名前変更・削除。");
}
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    (void)t;
    NSDictionary *profile = Profiles()[p.row];
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    c.textLabel.text = [profile[@"id"] isEqual:@"default"] ? T(@"Default", @"標準") : profile[@"name"];
    NSUInteger on = 0; for (NSDictionary *r in profile[@"packages"]) on += [r[@"on"] boolValue];
    NSString *running = [profile[@"id"] isEqual:activeProfile] ? T(@" · running now", @" · 現在使用中") : @"";
    c.detailTextLabel.text = [NSString stringWithFormat:T(@"%lu MODs on%@", @"MOD %lu個オン%@"), (unsigned long)on, running];
    c.accessoryType = [profile[@"id"] isEqual:SelectedProfile()[@"id"]] ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    return c;
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)p {
    [t deselectRowAtIndexPath:p animated:YES];
    state[@"next"] = Profiles()[p.row][@"id"]; SaveState(); [t reloadData];
    [NSNotificationCenter.defaultCenter postNotificationName:@"SKModsChanged" object:nil];
    skyrim_mobile_show_notice(T(@"Profile saved. Save your game, then fully close and reopen the app.", @"プロファイルを保存しました。ゲーム内で保存し、アプリを完全終了して開き直すと切り替わります。"));
}
- (void)askName:(NSString *)title initial:(NSString *)initial done:(void (^)(NSString *))done {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:nil preferredStyle:UIAlertControllerStyleAlert];
    [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.text = initial; }];
    [a addAction:[UIAlertAction actionWithTitle:T(@"Cancel", @"キャンセル") style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
        (void)x; NSString *name = [a.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (name.length) done(name);
    }]];
    [self presentViewController:a animated:YES completion:nil];
}
- (void)add {
    [self askName:T(@"New profile", @"新しいプロファイル") initial:@"MOD" done:^(NSString *name) {
        NSDictionary *from = SelectedProfile();
        NSString *pid = NSUUID.UUID.UUIDString, *dir = [@"Profiles" stringByAppendingPathComponent:pid];
        /* Start from the selected profile's MODs and INI files, not its saves. */
        NSString *src = [ProfileDir(from) stringByAppendingPathComponent:@"Documents/My Games/Skyrim"];
        NSString *dst = [[Base() stringByAppendingPathComponent:dir] stringByAppendingPathComponent:@"Documents/My Games/Skyrim"];
        [NSFileManager.defaultManager createDirectoryAtPath:dst withIntermediateDirectories:YES attributes:nil error:NULL];
        for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:src error:NULL])
            if ([f.pathExtension.lowercaseString isEqualToString:@"ini"])
                [NSFileManager.defaultManager copyItemAtPath:[src stringByAppendingPathComponent:f] toPath:[dst stringByAppendingPathComponent:f] error:NULL];
        NSMutableArray *packages = [NSMutableArray array], *plugins = [NSMutableArray array];
        for (NSDictionary *r in from[@"packages"]) [packages addObject:[r mutableCopy]];
        for (NSDictionary *r in from[@"plugins"]) [plugins addObject:[r mutableCopy]];
        [Profiles() addObject:[@{@"id":pid, @"name":name, @"dir":dir, @"packages":packages, @"plugins":plugins} mutableCopy]];
        SaveState(); [self.tableView reloadData];
        skyrim_mobile_show_notice(T(@"Profile created with its own save folder. Start a new game there or copy a save with the Files app.",
            @"専用のセーブフォルダ付きでプロファイルを作りました。新しくゲームを始めるか、セーブを個別にコピーしてください。"));
    }];
}
- (UISwipeActionsConfiguration *)tableView:(UITableView *)t trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)p {
    NSMutableDictionary *profile = Profiles()[p.row];
    NSMutableArray *actions = [NSMutableArray array];
    [actions addObject:[UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal title:T(@"Rename", @"名前変更")
        handler:^(UIContextualAction *a, UIView *v, void (^done)(BOOL)) {
        (void)a; (void)v;
        [self askName:T(@"Rename", @"名前変更") initial:profile[@"name"] done:^(NSString *name) { profile[@"name"] = name; SaveState(); [t reloadData]; }];
        done(YES);
    }]];
    if (![profile[@"id"] isEqual:@"default"] && ![profile[@"id"] isEqual:activeProfile]) {
        [actions addObject:[UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive title:T(@"Delete", @"削除")
            handler:^(UIContextualAction *a, UIView *v, void (^done)(BOOL)) {
            (void)a; (void)v;
            UIAlertController *c = [UIAlertController alertControllerWithTitle:T(@"Delete this profile and its saves?", @"このプロファイルとセーブを削除しますか？")
                message:T(@"Saves in this profile are deleted permanently. MOD files stay installed.", @"このプロファイルのセーブは完全に削除され、元に戻せません。MOD自体は残ります。") preferredStyle:UIAlertControllerStyleAlert];
            [c addAction:[UIAlertAction actionWithTitle:T(@"Cancel", @"キャンセル") style:UIAlertActionStyleCancel handler:nil]];
            [c addAction:[UIAlertAction actionWithTitle:T(@"Delete", @"削除") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
                (void)x;
                skyrim_remove_tree(ProfileDir(profile).fileSystemRepresentation);
                skyrim_remove_tree([[Base() stringByAppendingPathComponent:@"Views"] stringByAppendingPathComponent:profile[@"id"]].fileSystemRepresentation);
                if ([state[@"next"] isEqual:profile[@"id"]]) state[@"next"] = @"default";
                [Profiles() removeObject:profile]; SaveState(); [t reloadData];
            }]];
            [self presentViewController:c animated:YES completion:nil];
            done(YES);
        }]];
    }
    return [UISwipeActionsConfiguration configurationWithActions:actions];
}
@end

/* Report lines shown for a package, most serious first (the first line is
 * also shown in the MOD list). Warnings only; nothing is blocked. */
static NSArray<NSString *> *ReportLines(NSDictionary *package) {
    NSDictionary *r = package[@"report"], *o = package[@"optimized"];
    if (![r isKindOfClass:NSDictionary.class]) return @[];
    NSMutableArray *bad = [NSMutableArray array], *warn = [NSMutableArray array], *info = [NSMutableArray array];
    unsigned (^n)(NSString *) = ^unsigned(NSString *k) { return [r[k] unsignedIntValue]; };
    if (n(@"sePlugins")) [bad addObject:[NSString stringWithFormat:T(@"⛔ %u Special Edition plugin(s): likely to crash", @"⛔ SE用のプラグイン %u個：落ちる可能性が高い"), n(@"sePlugins")]];
    if (n(@"esl")) [bad addObject:T(@"⛔ ESL files are not supported by the 2011 engine", @"⛔ ESLは2011年版のエンジンでは使えません")];
    if (n(@"skseDlls")) [bad addObject:[NSString stringWithFormat:T(@"⛔ %u SKSE plugin DLL(s): cannot run on iPhone", @"⛔ SKSEプラグイン（DLL）%u個：iPhoneでは動きません"), n(@"skseDlls")]];
    if (n(@"seMeshes")) [warn addObject:[NSString stringWithFormat:T(@"⚠️ %u Special Edition mesh(es): may look broken or crash", @"⚠️ SE用のメッシュ %u個：表示崩れや落ちる可能性"), n(@"seMeshes")]];
    if (n(@"skseScripts")) {
        NSString *missing = [r[@"skseMissing"] length] ? [NSString stringWithFormat:T(@" (not available: %@)", @"（未対応：%@）"), r[@"skseMissing"]] : @"";
        [warn addObject:[NSString stringWithFormat:T(@"⚠️ %u script(s) need SKSE%@", @"⚠️ SKSEが必要なスクリプト %u個%@"), n(@"skseScripts"), missing]];
    }
    if (n(@"mcm")) [warn addObject:T(@"⚠️ Uses the SkyUI MCM menu (needs SkyUI and SKSE)", @"⚠️ SkyUIのMCMメニューを使用（SkyUIとSKSEが必要）")];
    if (n(@"foreignBsas") || n(@"foreignMeshes")) [warn addObject:T(@"⚠️ Files from another game version", @"⚠️ 別のゲーム用のファイルが含まれています")];
    unsigned mb = n(@"textureMB");
    if (mb >= 1024) [warn addObject:[NSString stringWithFormat:T(@"⚠️ Heavy: about %u MB of textures", @"⚠️ 重い：テクスチャ約%u MB"), mb]];
    else if (mb >= 256) [info addObject:[NSString stringWithFormat:T(@"Medium: about %u MB of textures", @"やや重い：テクスチャ約%u MB"), mb]];
    else if (n(@"textures")) [info addObject:[NSString stringWithFormat:T(@"Light: about %u MB of textures", @"軽い：テクスチャ約%u MB"), mb]];
    if ([o isKindOfClass:NSDictionary.class]) {
        unsigned changed = [o[@"trimmed"] unsignedIntValue] + [o[@"transcoded"] unsignedIntValue] + [o[@"reheadered"] unsignedIntValue];
        if (changed || [o[@"seArchives"] unsignedIntValue])
            [info addObject:[NSString stringWithFormat:T(@"Auto-optimized: %u texture(s) (limit %@), %u SE archive(s) unpacked", @"自動最適化：テクスチャ %u個（上限 %@）、SE用BSA %u個を展開"),
                changed, [o[@"limit"] unsignedIntValue] ? [o[@"limit"] stringValue] : T(@"none", @"なし"), [o[@"seArchives"] unsignedIntValue]]];
        if ([o[@"unsupported"] unsignedIntValue] || [o[@"failed"] unsignedIntValue])
            [warn addObject:[NSString stringWithFormat:T(@"⚠️ %u texture(s) could not be converted", @"⚠️ 変換できなかったテクスチャ %u個"),
                [o[@"unsupported"] unsignedIntValue] + [o[@"failed"] unsignedIntValue]]];
    }
    if (!bad.count && !warn.count) [info insertObject:T(@"✅ No problems found", @"✅ 問題は見つかりませんでした") atIndex:0];
    [bad addObjectsFromArray:warn]; [bad addObjectsFromArray:info];
    return bad;
}

@interface SKModHub : UITableViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) NSMutableArray<NSMutableDictionary *> *rows;   /* selected profile's packages */
@property(nonatomic, strong) NSDictionary<NSString *, NSDictionary *> *packages;
@property(nonatomic, strong) NSDictionary<NSString *, NSString *> *summaries;   /* computed once per reload */
@property(nonatomic) BOOL pickingFolder, pickingList;
@end
enum { HubProfile, HubMods, HubAdd, HubTools, HubOptimize, HubSafety, HubShare, HubCount };
#ifdef SKYRIM_IOS_CLASSIC_SKSE
enum { HubSkyUITrialRow = 3 };
#endif
@implementation SKModHub
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = @"MOD";
    self.tableView.estimatedRowHeight = 60; self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:T(@"Reorder", @"並べ替え") style:UIBarButtonItemStylePlain target:self action:@selector(toggleReorder)];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(reload) name:@"SKModsChanged" object:nil];
    [self reload];
}
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)reload {
    EnsureState();
    self.rows = SelectedProfile()[@"packages"];
    NSMutableDictionary *byId = [NSMutableDictionary dictionary];
    NSMutableDictionary *summaries = [NSMutableDictionary dictionary];
    for (NSDictionary *p in InstalledPackages()) {
        byId[p[@"id"]] = p;
        NSString *size = [NSByteCountFormatter stringFromByteCount:[p[@"bytes"] longLongValue] countStyle:NSByteCountFormatterCountStyleFile];
        NSMutableArray *bits = [NSMutableArray arrayWithObject:[NSString stringWithFormat:T(@"%@ · %@ files", @"%@ · %@ファイル"), size, p[@"files"] ?: @0]];
        NSArray *report = ReportLines(p);
        if (report.count) [bits addObject:report.firstObject];
        summaries[p[@"id"]] = [bits componentsJoinedByString:@"\n"];
    }
    self.packages = byId; self.summaries = summaries;
    /* Installed packages not yet in this profile are listed, off. */
    for (NSString *pid in byId) {
        BOOL present = NO;
        for (NSDictionary *r in self.rows) if ([r[@"id"] isEqual:pid]) present = YES;
        if (!present) [self.rows addObject:[@{@"id":pid, @"on":@NO} mutableCopy]];
    }
    [self.tableView reloadData];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    RefreshLanguage();
    [self updateReorderTitle];
    /* The game keeps drawing on the main thread behind this screen; pause it
     * while any MOD screen is open so scrolling stays smooth. Released when the
     * settings root appears again (see mobile_ui.m). */
    skyrim_mobile_menu_pause(SKYRIM_MENU_PAUSE_MODS, 1);
    [self reload];
}
- (void)toggleReorder {
    [self setEditing:!self.editing animated:YES];
    [self updateReorderTitle];
}
- (void)updateReorderTitle {
    self.navigationItem.rightBarButtonItem.title = self.editing ? T(@"Done", @"完了") : T(@"Reorder", @"並べ替え");
}
- (void)changed:(NSString *)message { SaveState(); skyrim_mobile_show_notice(message ?: T(@"Saved. Save your game, then fully close and reopen the app.", @"保存しました。ゲーム内で保存し、アプリを完全終了して開き直すと反映します。")); }
- (NSInteger)numberOfSectionsInTableView:(UITableView *)t { (void)t; return HubCount; }
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s {
    (void)t;
    switch (s) {
    case HubMods: return self.rows.count ? (NSInteger)self.rows.count : 1;
    case HubAdd: return 3;
    case HubTools:
#ifdef SKYRIM_IOS_CLASSIC_SKSE
        return HubSkyUITrialRow + 1;
#else
        return 3;
#endif
    case HubOptimize: return 2;
    case HubShare: return 2;
    default: return 1;
    }
}
- (NSString *)tableView:(UITableView *)t titleForHeaderInSection:(NSInteger)s {
    (void)t;
    return @[T(@"Profile", @"プロファイル"), T(@"MODs — lower ones win", @"MOD — 下にあるほど優先"), T(@"Add", @"追加"),
             T(@"Details", @"詳細"), T(@"Automatic optimization", @"自動最適化"), T(@"Safety", @"安全"), T(@"Share", @"共有")][s];
}
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s {
    (void)t;
    if (s == HubProfile && ![SelectedProfile()[@"id"] isEqual:activeProfile])
        return T(@"This profile is used from the next launch. The plugin order is shown for the running profile.", @"このプロファイルは次回起動から使われます。プラグインの並びは現在使用中のプロファイルのものです。");
    if (s == HubMods) return T(@"Changes apply after a full restart. MOD files are kept inside the app; the original game files are not changed. Only MODs for Skyrim 2011 / Legendary Edition work.",
        @"変更はアプリの完全終了・再起動後に反映します。MODはアプリ内に保存され、元のゲームファイルは変更しません。Skyrim 2011年版／Legendary Edition用のMODだけが動きます。");
    if (s == HubAdd) return T(@"You can also share a zip from Safari or the Files app to SkyrimIOS.", @"Safariや「ファイル」アプリからzipをSkyrimIOSへ共有しても追加できます。");
    if (s == HubOptimize) return T(@"Applied to each MOD when it is added: oversized textures are reduced, Special Edition textures and archives are converted for the 2011 engine, and missing mipmaps are rebuilt. Your original download is not changed. Applies to MODs added from now on.",
        @"MODを追加するときに自動で行います：大きすぎるテクスチャの縮小、SE用テクスチャとBSAの2011年版向け変換、縮小版（ミップマップ）の作成。元のダウンロードファイルは変更しません。これから追加するMODに適用されます。");
    if (s == HubSafety) return T(@"If a launch with MODs ends unexpectedly, the app asks what to do at the next launch.", @"MODありの起動が途中で終了した場合、次回起動時に対応を選べます。");
    if (s == HubShare) return T(@"The MOD list file contains names and checksums only, not MOD files or saves.", @"構成ファイルにはMODの名前と識別情報だけが入り、MOD本体やセーブは含まれません。");
    return nil;
}
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    UITableViewCell *c = ReusableCell(t);
    c.textLabel.numberOfLines = 2; c.detailTextLabel.numberOfLines = 3;
    switch (p.section) {
    case HubProfile: {
        NSDictionary *pr = SelectedProfile();
        c.textLabel.text = [pr[@"id"] isEqual:@"default"] ? T(@"Default", @"標準") : pr[@"name"];
        c.imageView.image = [UIImage systemImageNamed:@"person.crop.square"];
        c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        break;
    }
    case HubMods: {
        if (!self.rows.count) { c.textLabel.text = T(@"No MODs yet", @"MODはまだありません"); c.selectionStyle = UITableViewCellSelectionStyleNone; break; }
        NSMutableDictionary *row = self.rows[p.row];
        NSDictionary *pkg = self.packages[row[@"id"]];
        c.textLabel.text = pkg[@"name"] ?: @"?";
        c.detailTextLabel.text = self.summaries[row[@"id"]];
        UISwitch *toggle = [UISwitch new]; toggle.on = [row[@"on"] boolValue]; toggle.tag = p.row;
        [toggle addTarget:self action:@selector(toggled:) forControlEvents:UIControlEventValueChanged];
        c.accessoryView = toggle;
        break;
    }
    case HubAdd: {
        NSArray *titles = @[T(@"Add a zip file", @"zipファイルを選んで追加"), T(@"Add a folder", @"フォルダを選んで追加"), T(@"Find on Nexus Mods", @"Nexus ModsでMODを探す")];
        NSArray *icons = @[@"doc.zipper", @"folder", @"safari"];
        c.textLabel.text = titles[p.row]; c.imageView.image = [UIImage systemImageNamed:icons[p.row]];
        break;
    }
    case HubTools:
#ifdef SKYRIM_IOS_CLASSIC_SKSE
        if (p.row == HubSkyUITrialRow) {
            c.textLabel.text = T(@"SkyUI 5.1 trial", @"SkyUI 5.1 の試験");
            c.detailTextLabel.text = T(@"Enable SkyUI.esp in this profile, then restart.", @"このプロファイルでSkyUI.espを有効にし、アプリを起動し直してください。");
            UISwitch *toggle = [UISwitch new]; toggle.on = SkyUITrialSelected(SelectedProfile());
            toggle.accessibilityLabel = c.textLabel.text;
            toggle.accessibilityIdentifier = @"mod-skyui-trial";
            [toggle addTarget:self action:@selector(skyuiTrialToggled:) forControlEvents:UIControlEventValueChanged];
            c.accessoryView = toggle; c.selectionStyle = UITableViewCellSelectionStyleNone;
            break;
        }
#endif
        c.textLabel.text = @[T(@"Plugin order", @"プラグインの並び"), T(@"Conflicts", @"競合"), T(@"Save check", @"セーブの確認")][p.row];
        c.imageView.image = [UIImage systemImageNamed:@[@"list.number", @"arrow.triangle.merge", @"checkmark.shield"][p.row]];
        c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        break;
    case HubOptimize:
        if (p.row == 0) {
            c.textLabel.text = T(@"Optimize new MODs", @"追加するMODを最適化");
            UISwitch *toggle = [UISwitch new]; toggle.on = ![NSUserDefaults.standardUserDefaults boolForKey:@"mods-optimize-off"];
            [toggle addTarget:self action:@selector(optimizeToggled:) forControlEvents:UIControlEventValueChanged];
            c.accessoryView = toggle; c.selectionStyle = UITableViewCellSelectionStyleNone;
        } else {
            unsigned limit = TextureLimit();
            c.textLabel.text = T(@"Texture size limit", @"テクスチャの上限");
            c.detailTextLabel.text = limit ? [NSString stringWithFormat:@"%u px", limit] : T(@"No limit", @"上限なし");
            c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        }
        break;
    case HubSafety: {
        c.textLabel.text = T(@"Start without MODs next time", @"次回はMODなしで起動");
        UISwitch *toggle = [UISwitch new]; toggle.on = [state[@"safeNext"] boolValue];
        [toggle addTarget:self action:@selector(safeToggled:) forControlEvents:UIControlEventValueChanged];
        c.accessoryView = toggle; c.selectionStyle = UITableViewCellSelectionStyleNone;
        break;
    }
    case HubShare:
        c.textLabel.text = p.row ? T(@"Load a MOD list", @"構成を読み込む") : T(@"Share this MOD list", @"この構成を書き出して共有");
        c.imageView.image = [UIImage systemImageNamed:p.row ? @"square.and.arrow.down" : @"square.and.arrow.up"];
        break;
    }
    return c;
}
- (void)toggled:(UISwitch *)toggle {
    if ((NSUInteger)toggle.tag >= self.rows.count) return;
    NSMutableDictionary *row = self.rows[toggle.tag];
    row[@"on"] = @(toggle.on);
    /* Turning a package on also turns its plugins on in this profile. */
    if (toggle.on) {
        NSMutableArray *plugins = SelectedProfile()[@"plugins"];
        for (NSString *name in self.packages[row[@"id"]][@"plugins"]) {
            BOOL found = NO;
            for (NSMutableDictionary *r in plugins) if ([r[@"name"] caseInsensitiveCompare:name] == NSOrderedSame) { r[@"on"] = @YES; found = YES; }
            if (!found) [plugins addObject:[@{@"name":name, @"on":@YES} mutableCopy]];
        }
    }
    [self changed:nil];
}
- (void)optimizeToggled:(UISwitch *)toggle { [NSUserDefaults.standardUserDefaults setBool:!toggle.on forKey:@"mods-optimize-off"]; }
- (void)chooseLimit:(NSIndexPath *)p {
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:T(@"Texture size limit", @"テクスチャの上限")
        message:T(@"Lower is lighter and more stable; 2048 is a good balance on iPhone.", @"小さいほど軽く安定します。iPhoneでは2048がおすすめです。") preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSNumber *v in @[@1024, @2048, @4096, @0])
        [sheet addAction:[UIAlertAction actionWithTitle:v.unsignedIntValue ? [NSString stringWithFormat:@"%@ px", v] : T(@"No limit", @"上限なし")
            style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) {
            (void)a; [NSUserDefaults.standardUserDefaults setObject:v forKey:@"mods-texture-max"]; [self.tableView reloadData];
        }]];
    [sheet addAction:[UIAlertAction actionWithTitle:T(@"Cancel", @"キャンセル") style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView = [self.tableView cellForRowAtIndexPath:p];
    [self presentViewController:sheet animated:YES completion:nil];
}
- (void)safeToggled:(UISwitch *)toggle { state[@"safeNext"] = @(toggle.on); [self changed:nil]; }
#ifdef SKYRIM_IOS_CLASSIC_SKSE
- (void)skyuiTrialToggled:(UISwitch *)toggle {
    NSMutableDictionary *profile = SelectedProfile();
    BOOL previous = SkyUITrialSelected(profile);
    profile[@"skyuiTrial"] = toggle.on ? @YES : nil;
    if (!SaveState()) {
        profile[@"skyuiTrial"] = previous ? @YES : nil;
        toggle.on = previous;
        skyrim_mobile_show_notice(T(@"Could not save the SkyUI trial setting.", @"SkyUI試験の設定を保存できませんでした。"));
        return;
    }
    skyrim_mobile_show_notice(T(@"SkyUI trial setting saved for the next launch.", @"SkyUI試験の設定を保存しました。次回起動から反映します。"));
}
#endif
- (BOOL)tableView:(UITableView *)t canEditRowAtIndexPath:(NSIndexPath *)p { (void)t; return p.section == HubMods && self.rows.count; }
- (BOOL)tableView:(UITableView *)t canMoveRowAtIndexPath:(NSIndexPath *)p { (void)t; return p.section == HubMods && self.rows.count > 1; }
- (NSIndexPath *)tableView:(UITableView *)t targetIndexPathForMoveFromRowAtIndexPath:(NSIndexPath *)from toProposedIndexPath:(NSIndexPath *)to {
    (void)t;
    if (to.section != HubMods) return [NSIndexPath indexPathForRow:to.section < HubMods ? 0 : (NSInteger)self.rows.count - 1 inSection:HubMods];
    (void)from; return to;
}
- (UITableViewCellEditingStyle)tableView:(UITableView *)t editingStyleForRowAtIndexPath:(NSIndexPath *)p {
    (void)t; (void)p; return self.editing ? UITableViewCellEditingStyleNone : UITableViewCellEditingStyleDelete;
}
- (BOOL)tableView:(UITableView *)t shouldIndentWhileEditingRowAtIndexPath:(NSIndexPath *)p { (void)t; (void)p; return NO; }
- (void)tableView:(UITableView *)t moveRowAtIndexPath:(NSIndexPath *)from toIndexPath:(NSIndexPath *)to {
    NSMutableDictionary *row = self.rows[from.row];
    [self.rows removeObjectAtIndex:from.row]; [self.rows insertObject:row atIndex:to.row];
    [self changed:nil];
    dispatch_async(dispatch_get_main_queue(), ^{ [t reloadData]; });
}
- (void)tableView:(UITableView *)t commitEditingStyle:(UITableViewCellEditingStyle)style forRowAtIndexPath:(NSIndexPath *)p {
    if (style != UITableViewCellEditingStyleDelete) return;
    NSDictionary *row = self.rows[p.row];
    NSString *name = self.packages[row[@"id"]][@"name"] ?: @"?";
    UIAlertController *c = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:T(@"Delete %@?", @"%@ を削除しますか？"), name]
        message:T(@"The MOD is removed from every profile. Saves that use it may need it again.", @"すべてのプロファイルから外れます。このMODを使ったセーブは、また必要になる場合があります。")
        preferredStyle:UIAlertControllerStyleAlert];
    [c addAction:[UIAlertAction actionWithTitle:T(@"Cancel", @"キャンセル") style:UIAlertActionStyleCancel handler:nil]];
    [c addAction:[UIAlertAction actionWithTitle:T(@"Delete", @"削除") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a) {
        (void)a;
        NSString *pid = row[@"id"];
        /* The running view is a clone, so deleting the package now is safe. */
        int removedTree = skyrim_remove_tree(PackageDir(pid).fileSystemRepresentation);
        InvalidatePackages();
        if (removedTree) {
            skyrim_mobile_show_notice(T(@"Could not delete the MOD.", @"MODを削除できませんでした。"));
            return;
        }
        NSMutableSet *removed = [NSMutableSet set];
        for (NSString *plugin in self.packages[pid][@"plugins"]) [removed addObject:plugin.lowercaseString];
        for (NSMutableDictionary *profile in Profiles()) {
            NSMutableArray *keep = [NSMutableArray array];
            NSMutableSet *noProvider = [removed mutableCopy];
            BOOL hadPackage = NO;
            for (NSDictionary *r in profile[@"packages"]) {
                if ([r[@"id"] isEqual:pid]) { hadPackage = YES; continue; }
                [keep addObject:r];
                if ([r[@"on"] boolValue])
                    for (NSString *plugin in LoadPackage(r[@"id"])[@"plugins"]) [noProvider removeObject:plugin.lowercaseString];
            }
            profile[@"packages"] = keep;
            if (hadPackage) for (NSMutableDictionary *choice in profile[@"plugins"])
                if ([noProvider containsObject:[choice[@"name"] lowercaseString]]) choice[@"on"] = @NO;
        }
        [self changed:[NSString stringWithFormat:T(@"Deleted %@.", @"%@ を削除しました。"), name]];
        [self reload];
    }]];
    [self presentViewController:c animated:YES completion:nil];
}
- (void)pick:(NSArray<UTType *> *)types folder:(BOOL)folder list:(BOOL)list {
    self.pickingFolder = folder; self.pickingList = list;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO];
    picker.delegate = self; picker.allowsMultipleSelection = !folder && !list;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)picker didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)picker;
    for (NSURL *url in urls) skyrim_mods_import_url(url, YES);
}
- (void)share:(NSURL *)file from:(NSIndexPath *)p {
    if (!file) return;
    UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:@[file] applicationActivities:nil];
    share.popoverPresentationController.sourceView = [self.tableView cellForRowAtIndexPath:p];
    [self presentViewController:share animated:YES completion:nil];
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)p {
    [t deselectRowAtIndexPath:p animated:YES];
    switch (p.section) {
    case HubProfile: [self.navigationController pushViewController:[[SKProfiles alloc] initWithStyle:UITableViewStyleInsetGrouped] animated:YES]; break;
    case HubMods: {
        if (!self.rows.count) break;
        NSDictionary *pkg = self.packages[self.rows[p.row][@"id"]];
        SKListController *detail = [[SKListController alloc] initWithStyle:UITableViewStyleInsetGrouped];
        NSMutableArray *lines = [NSMutableArray arrayWithObject:[NSString stringWithFormat:T(@"File: %@", @"元のファイル：%@"), pkg[@"archive"] ?: @"?"]];
        if ([pkg[@"plugins"] count]) [lines addObject:[NSString stringWithFormat:T(@"Plugins: %@", @"プラグイン：%@"), [pkg[@"plugins"] componentsJoinedByString:@", "]]];
        [lines addObjectsFromArray:ReportLines(pkg)];
        detail.lines = lines; detail.title = pkg[@"name"];
        [self.navigationController pushViewController:detail animated:YES];
        break;
    }
    case HubAdd:
        if (p.row == 0) {
            NSMutableArray *types = [NSMutableArray arrayWithObject:UTTypeZIP];
            for (NSString *ext in @[@"7z", @"rar"]) { UTType *u = [UTType typeWithFilenameExtension:ext]; if (u) [types addObject:u]; }
            [self pick:types folder:NO list:NO];
        } else if (p.row == 1) [self pick:@[UTTypeFolder] folder:YES list:NO];
        else [self.navigationController pushViewController:skyrim_mods_browser_controller() animated:YES];
        break;
    case HubTools:
#ifdef SKYRIM_IOS_CLASSIC_SKSE
        if (p.row == HubSkyUITrialRow) break;
#endif
        if (p.row == 0) [self.navigationController pushViewController:skyrim_mobile_plugin_controller() animated:YES];
        else if (p.row == 1) [self.navigationController pushViewController:[[SKConflicts alloc] initWithStyle:UITableViewStyleInsetGrouped] animated:YES];
        else [self.navigationController pushViewController:[[SKSaves alloc] initWithStyle:UITableViewStyleInsetGrouped] animated:YES];
        break;
    case HubOptimize:
        if (p.row == 1) [self chooseLimit:p];
        break;
    case HubShare:
        if (p.row) [self pick:@[UTTypeJSON] folder:NO list:YES];
        else [self share:ExportModList(SelectedProfile()) from:p];
        break;
    }
}
@end

UIViewController *skyrim_mods_hub_controller(void) {
    EnsureState();
    return [[SKModHub alloc] initWithStyle:UITableViewStyleInsetGrouped];
}
