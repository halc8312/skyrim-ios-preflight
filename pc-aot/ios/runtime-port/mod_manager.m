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
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mobile_ui.h"
#include "mod_manager.h"
#include "game_language.h"
#include "copy_pair.h"
#ifdef SKYRIM_IOS_CLASSIC_SKSE
#include "PcGuestSkseTrial.h"
#include "inspect.h"
#include "mod_requirements.h"
#include "mod_features.h"
#ifdef SKYRIM_IOS_MOD_HOOKS
#include "PcGuestModHooksABI.h"
#include "classic_vm_epoch.h"
#include "mod_camera_host.h"
#include "mod_ece_tri_host.h"
#include "mod_fuz.h"
#include "mod_fuz_host.h"
#include "mod_hook_selection.h"
#include "mod_loading_menu_host.h"
#include "mod_racemenu_preset.h"
#include "mod_save_host.h"
#include "highheel_identity.h"
#include "mod_highheel_class_host.h"
#include "mod_highheel_effect_host.h"
#include "mod_highheel_pex_host.h"
#include "mod_highheel_scene_host.h"
#include "probe_asset_file.h"
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
#include "mod_highheel_scene_requests.h"
#endif
/* classic_skse_control.h, in the staged skse_system.c */
void skyrim_classic_control_form_deleted(struct CPU *cpu, uint32_t vm, uint64_t handle);
#endif
#endif
#ifdef SKYRIM_IOS_SKSE_DLL_PROBE
#include "mod_dll_probe.h"
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
static NSString *CanonicalSupportPath(NSString *path);

static NSMutableDictionary *state;          /* main thread only */
static NSString *activeProfile;             /* this launch */
static NSString *originalGameDirectory;     /* set at launch, before the installer is available */
static NSString *launchNotice;
static BOOL launchWithoutMods, healthyMarked;
static NSArray<NSDictionary *> *launchPlugins;
static NSArray<NSString *> *launchPackageIds; /* conflict owner order for the running view */
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
static NSString *ProfileName(NSDictionary *profile) {
    if ([profile[@"id"] isEqual:@"default"]) return T(@"Default", @"標準");
    id name = profile[@"name"];
    return [name isKindOfClass:NSString.class] && [name length] ? name : T(@"Profile", @"プロファイル");
}
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
        if (![p isKindOfClass:NSMutableDictionary.class] || ![p[@"id"] isEqual:pid]) p = nil;
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
/* Read-only: the original Data folder, then the profile's enabled packages in order. */
static NSArray<NSString *> *DataRootsFor(NSArray<NSString *> *packageIds, NSMutableArray<NSString *> *labels,
                                         BOOL *firstIsOriginal) {
    NSMutableArray<NSString *> *roots = [NSMutableArray array];
    if (firstIsOriginal) *firstIsOriginal = NO;
    if (originalGameDirectory.length) {
        char data[4096];
        if (skyrim_plugin_child_path(originalGameDirectory.fileSystemRepresentation, "Data", data, sizeof data) >= 0) {
            [roots addObject:@(data)];
            if (firstIsOriginal) *firstIsOriginal = YES;
            [labels addObject:T(@"Original game Data", @"元のゲームのData")];
        }
    }
    for (NSString *pid in packageIds) {
        NSString *dir = PackageDir(pid);
        if (!dir) continue;
        [roots addObject:[dir stringByAppendingPathComponent:@"Data"]];
        if (labels) {
            id name = LoadPackage(pid)[@"name"];
            [labels addObject:[name isKindOfClass:NSString.class] && [name length] ? name : pid];
        }
    }
    return roots;
}

static SkyrimSkyUIRequirements SkyUIRequirementsFor(NSArray<NSString *> *packageIds) {
    NSArray<NSString *> *roots = DataRootsFor(packageIds, nil, NULL);
    const char **paths = calloc(roots.count + 1, sizeof *paths);
    SkyrimSkyUIRequirements result = {.status = SKYRIM_SKYUI_REQ_UNREADABLE};
    if (paths) {
        for (NSUInteger i = 0; i < roots.count; ++i) paths[i] = roots[i].fileSystemRepresentation;
        if (skyrim_skyui_requirements(paths, roots.count, &result)) result.status = SKYRIM_SKYUI_REQ_UNREADABLE;
        free(paths);
    }
    return result;
}

static NSArray<NSString *> *XpmseRequirementLines(NSArray<NSString *> *packageIds, NSArray<NSDictionary *> *choices) {
    NSArray<NSString *> *roots = DataRootsFor(packageIds, nil, NULL);
    const char **paths = calloc(roots.count + 1, sizeof *paths);
    if (!paths) return @[];
    for (NSUInteger i = 0; i < roots.count; ++i) paths[i] = roots[i].fileSystemRepresentation;
    SkyrimXpmseRequirements result;
    int status = skyrim_xpmse_requirements(paths, roots.count, &result);
    free(paths);
    if (status) return @[];
    NSMutableArray *lines = [NSMutableArray arrayWithArray:@[
        [NSString stringWithFormat:T(@"Common FNIS-related HKX files with a 32-bit Skyrim header: %u of %u.",
          @"Skyrimの32ビット形式を確認した主な関連HKX：%u／%u個。"),
          result.fnis_hkx32, (unsigned)SKYRIM_XPMSE_FNIS_HKX],
        [NSString stringWithFormat:T(@"Animation data lists found: %u of %u.",
          @"アニメーションデータの一覧ファイル：%u／%u個。"),
          result.fnis_lists, (unsigned)SKYRIM_XPMSE_FNIS_LISTS],
        result.skse_script ? T(@"SKSE Classic script found.", @"SKSE Classicのスクリプトを確認しました。")
                           : T(@"Add the SKSE Classic scripts to this profile.", @"このプロファイルにSKSE Classicのスクリプトを追加してください。"),
    ]];
    if (result.fnis_hkx64)
        [lines addObject:[NSString stringWithFormat:T(@"%u of the checked HKX files are 64-bit. Use Skyrim LE output.",
          @"確認したHKXのうち%u個は64ビット形式です。Skyrim LE用の出力を使ってください。"), result.fnis_hkx64]];
    if (result.fnis_hkx_unknown)
        [lines addObject:[NSString stringWithFormat:T(@"Format could not be confirmed for %u of the checked HKX files.",
          @"確認したHKXのうち%u個は形式を判定できませんでした。"), result.fnis_hkx_unknown]];
    BOOL fnisSelected = NO;
    for (NSDictionary *row in choices) {
        if ([row[@"name"] caseInsensitiveCompare:@"FNIS.esp"] != NSOrderedSame) continue;
        fnisSelected = [row[@"on"] boolValue];
        break;
    }
    if (result.fnis_plugin && fnisSelected)
        [lines addObject:T(@"FNIS.esp is selected for the next launch.", @"FNIS.espは次回起動時に有効にする設定です。")];
    else
        [lines addObject:T(@"Install and enable FNIS.esp to use FNIS Alternate Animations with XPMSE.",
                           @"XPMSEでFNISの代替アニメーションを使うには、FNIS.espを入れて有効にしてください。")];
    [lines addObject:T(@"For animation changes, import the full FNIS output generated on a PC for this Skyrim LE profile.",
                       @"アニメーションを変えるには、このSkyrim LEプロファイル用にPCで生成したFNISの出力をまとめて取り込んでください。")];
    [lines addObject:T(@"NiOverride is optional for the skeleton files. Some XPMSE options on PC use RaceMenu's bundled copy or the standalone package.",
                       @"骨格ファイルにNiOverrideは必須ではありません。PC版XPMSEの一部のオプションには、RaceMenu同梱版か単体版を使います。")];
    if (result.nioverride)
        [lines addObject:T(@"NiOverride DLL detected. Running NiOverride in the game on iPhone is still unsupported.",
                           @"NiOverrideのDLLを検出しました。iPhoneのゲーム内でのNiOverride実行は未対応です。")];
    if (result.unreadable) [lines addObject:T(@"Some dependency paths could not be checked.",
                                               @"前提ファイルの一部を確認できませんでした。")];
    return lines;
}

static SkyrimFeatureScan FeatureScanRoots(NSArray<NSString *> *roots, BOOL firstIsOriginal) {
    const char **paths = calloc(roots.count + 1, sizeof *paths);
    SkyrimFeatureScan scan;
    if (!paths) { memset(&scan, 0, sizeof scan); return scan; }
    for (NSUInteger i = 0; i < roots.count; ++i) paths[i] = roots[i].fileSystemRepresentation;
    if (skyrim_features_scan(paths, roots.count, firstIsOriginal, &scan)) memset(&scan, 0, sizeof scan);
    free(paths);
    return scan;
}

/* Text for a missing or unusable requirement, nil when SkyUI is absent or complete. */
static NSString *SkyUIRequirementText(SkyrimSkyUIRequirements r) {
    switch (r.status) {
    case SKYRIM_SKYUI_REQ_SKSE_SCRIPT_MISSING:
        return T(@"SkyUI needs the SKSE 1.7.3 scripts. Add your own SKSE Classic 1.7.3 archive to this profile.",
                 @"SkyUIにはSKSE 1.7.3のスクリプトが必要です。所有しているSKSE Classic 1.7.3のアーカイブをこのプロファイルに追加してください。");
    case SKYRIM_SKYUI_REQ_SKSE_SCRIPT_INVALID:
        return T(@"Scripts/SKSE.pex is not the SKSE 1.7.3 script. Add your own SKSE Classic 1.7.3 archive.",
                 @"Scripts/SKSE.pex がSKSE 1.7.3のスクリプトではありません。所有しているSKSE Classic 1.7.3のアーカイブを追加してください。");
    case SKYRIM_SKYUI_REQ_SKYUI_ARCHIVE_UNSUPPORTED:
        return T(@"SkyUI.esp is present, but SkyUI.bsa is missing or is not the Skyrim 2011 (LE) version.",
                 @"SkyUI.espはありますが、SkyUI.bsaがない、またはSkyrim 2011年版（LE）用ではありません。");
    case SKYRIM_SKYUI_REQ_UNREADABLE:
        return T(@"The SkyUI files in this profile cannot be checked (links, duplicate names or read errors).",
                 @"このプロファイルのSkyUIのファイルを確認できません（リンク、同名のファイル、読み取りエラー）。");
    case SKYRIM_SKYUI_REQ_OK:
        if (r.support_present < r.support_expected)
            return [NSString stringWithFormat:T(@"Some SKSE support scripts are missing (%u of %u present).",
                @"SKSEの補助スクリプトが一部ありません（%u／%u個）。"), r.support_present, r.support_expected];
        return nil;
    default: return nil;
    }
}

/* Chosen in the profile (profile[@"features"][key]); frozen at launch below. */
static BOOL FeatureSelected(NSDictionary *profile, unsigned feature) {
    NSDictionary *chosen = profile[@"features"];
    return [chosen isKindOfClass:NSDictionary.class] && [chosen[@(skyrim_feature_key(feature) ?: "")] isEqual:@YES];
}

static BOOL FeatureImplementedInApp(unsigned feature) {
#ifndef SKYRIM_IOS_MOD_HOOKS
    if (feature == SKYRIM_FEATURE_SAFETY_LOAD) return NO;
#endif
    return skyrim_feature_implemented(feature) != 0;
}
static uint32_t launchFeatureMask; /* set once on the startup thread before the guest runs */
int skyrim_mods_feature_enabled(unsigned feature) {
    return feature < SKYRIM_FEATURE_COUNT && (__atomic_load_n(&launchFeatureMask, __ATOMIC_RELAXED) >> feature & 1u);
}

#ifdef SKYRIM_IOS_MOD_HOOKS
static NSString *HexDigest(NSData *data);
static NSArray<NSString *> *EnabledPackageIds(NSDictionary *profile);

static NSData *ReadBoundedRegularFile(NSString *path, size_t limit) {
    if (!path) return nil;
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW_ANY
    flags |= O_NOFOLLOW_ANY;
#else
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path.fileSystemRepresentation, flags);
    if (fd < 0) return nil;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > limit) {
        close(fd);
        return nil;
    }
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)st.st_size];
    size_t offset = 0;
    while (offset < data.length) {
        ssize_t n = read(fd, (char *)data.mutableBytes + offset, data.length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += (size_t)n;
    }
    char excess;
    ssize_t extra;
    do { extra = read(fd, &excess, 1); } while (extra < 0 && errno == EINTR);
    close(fd);
    return offset == data.length && extra == 0 && data.length ? data : nil;
}

static BOOL VerifiedViewDLL(NSDictionary *profile, int (*find)(const char *, char *, size_t),
                            size_t size, NSString *sha256) {
    NSString *profileID = profile[@"id"];
    if (!launchPackageIds.count || !ValidId(profileID)) return NO;
    NSString *view = CanonicalSupportPath([[Base() stringByAppendingPathComponent:@"Views"]
                                          stringByAppendingPathComponent:profileID]);
    char data[4096], path[4096];
    if (!view || skyrim_plugin_child_path(view.fileSystemRepresentation, "Data", data, sizeof data) != 1 ||
        find(data, path, sizeof path) != 1) return NO;
    NSString *dllPath = [NSFileManager.defaultManager stringWithFileSystemRepresentation:path length:strlen(path)];
    NSData *dll = ReadBoundedRegularFile(dllPath, size);
    return dll.length == size && [HexDigest(dll) isEqualToString:sha256];
}

static bool SkipChargenPrecache(void) { return true; }  /* Only selected for the verified LE DLL. */

static NSString *RaceMenuPresetLeaf(NSDictionary *package) {
    id name = package[@"archive"];
    if (![name isKindOfClass:NSString.class] || ![[name pathExtension].lowercaseString isEqualToString:@"jslot"])
        return nil;
    const char *bytes = [name UTF8String];
    char normalized[NAME_MAX + 2];
    const size_t length = bytes ? strnlen(bytes, NAME_MAX + 1) : 0;
    if (!length || length > NAME_MAX || length != [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] ||
        strchr(bytes, '/') || strchr(bytes, '\\') ||
        skyrim_zip_normalize(bytes, length, 1, normalized, sizeof normalized) != 1) return nil;
    return [NSString stringWithUTF8String:normalized];
}

/* An app-owned package is immutable, but read and hash the same bounded
 * descriptor so a stale or manually replaced import cannot be selected. */
static BOOL ReadRaceMenuPreset(NSDictionary *package, SkyrimRaceMenuPreset *preset) {
    NSString *leaf = RaceMenuPresetLeaf(package);
    NSString *directory = PackageDir(package[@"id"]);
    id digest = package[@"sha256"];
    if (!leaf || !directory || ![digest isKindOfClass:NSString.class] || [digest length] != 64) return NO;
    NSString *path = CanonicalSupportPath([[directory stringByAppendingPathComponent:@"Data/SKSE/Plugins/CharGen/Presets"]
                                           stringByAppendingPathComponent:leaf]);
    NSData *data = ReadBoundedRegularFile(path, SKYRIM_RACEMENU_PRESET_LIMIT);
    return data && [HexDigest(data) isEqual:digest] &&
        skyrim_racemenu_preset_parse(data.bytes, data.length, preset) == SKYRIM_RACEMENU_PRESET_OK;
}

/* Read once on the startup thread. Guest menu callbacks only touch the
 * numeric state. The main-thread acknowledgement clears the same request,
 * even if the user navigated to another profile while the game was running. */
enum { RaceMenuEmpty, RaceMenuPending, RaceMenuTaken, RaceMenuApplied };
static atomic_uint raceMenuState;
static float raceMenuNose;
static NSDictionary *launchRaceMenuSelection;
static NSString *launchRaceMenuProfile;

static BOOL PrepareRaceMenuPreset(NSDictionary *profile) {
    if (launchWithoutMods || launchPluginListFailed ||
        !(pc_guest_mod_hooks_compiled() & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER)) return NO;
    id selection = profile[@"pendingPreset"];
    if (![selection isKindOfClass:NSDictionary.class] || !ValidId(selection[@"request"]) ||
        !ValidId(selection[@"package"]) || ![launchPackageIds containsObject:selection[@"package"]]) return NO;
    NSDictionary *package = LoadPackage(selection[@"package"]);
    SkyrimRaceMenuPreset preset;
    if (![selection[@"file"] isEqual:RaceMenuPresetLeaf(package)] ||
        ![selection[@"sha256"] isEqual:package[@"sha256"]] || !ReadRaceMenuPreset(package, &preset)) return NO;
    launchRaceMenuProfile = [profile[@"id"] copy];
    launchRaceMenuSelection = [selection copy];
    raceMenuNose = preset.morphs[SKYRIM_RACEMENU_NOSE_HEIGHT];
    atomic_store_explicit(&raceMenuState, RaceMenuPending, memory_order_release);
    return YES;
}

static bool RaceMenuPendingValue(void) {
    return atomic_load_explicit(&raceMenuState, memory_order_acquire) == RaceMenuPending;
}

static bool TakeRaceMenuNoseHeight(uint32_t menu, uint32_t actor, uint32_t base, float min, float max, float *out) {
    if (!menu || !actor || !base || !out || !RaceMenuPendingValue() ||
        !isfinite(min) || !isfinite(max) || min >= max || raceMenuNose < min || raceMenuNose > max) return false;
    unsigned expected = RaceMenuPending;
    if (!atomic_compare_exchange_strong_explicit(&raceMenuState, &expected, RaceMenuTaken,
                                                  memory_order_acq_rel, memory_order_relaxed)) return false;
    *out = raceMenuNose;
    return true;
}

static void RaceMenuDidApply(void) {
    unsigned expected = RaceMenuTaken;
    if (!atomic_compare_exchange_strong_explicit(&raceMenuState, &expected, RaceMenuApplied,
                                                  memory_order_acq_rel, memory_order_relaxed)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSMutableDictionary *profile = ProfileById(launchRaceMenuProfile);
        if (![profile[@"pendingPreset"] isEqual:launchRaceMenuSelection]) return;
        [profile removeObjectForKey:@"pendingPreset"];
        if (!SaveState()) profile[@"pendingPreset"] = launchRaceMenuSelection;
        skyrim_mobile_show_notice(T(@"Nose Height was applied. Save the game to keep it.",
                                    @"鼻の高さを設定しました。変更を残すにはゲーム内で保存してください。"));
    });
}
#endif

#ifdef SKYRIM_IOS_MOD_HOOKS
static bool DigestEceInput(int fd, size_t length, uint8_t result[32]) {
    CC_SHA256_CTX hash;
    if (CC_SHA256_Init(&hash) != 1) return false;
    unsigned char buffer[16384];
    while (length) {
        size_t count = length < sizeof buffer ? length : sizeof buffer;
        ssize_t n = read(fd, buffer, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || CC_SHA256_Update(&hash, buffer, (CC_LONG)n) != 1) return false;
        length -= (size_t)n;
    }
    ssize_t tail;
    do { tail = read(fd, buffer, 1); } while (tail < 0 && errno == EINTR);
    return tail == 0 && CC_SHA256_Final(result, &hash) == 1;
}

static bool PrepareEceTriObserver(void) {
    if (!launchPackageIds.count || !ValidId(activeProfile)) return false;
    BOOL plugin = NO;
    for (NSString *name in skyrim_mobile_loaded_plugins()) {
        if ([name caseInsensitiveCompare:@"CharacterMakingExtender.esp"] == NSOrderedSame ||
            [name caseInsensitiveCompare:@"EnhancedCharacterEdit.esp"] == NSOrderedSame) {
            plugin = YES;
            break;
        }
    }
    if (!plugin) return false;
    NSString *view = CanonicalSupportPath([[Base() stringByAppendingPathComponent:@"Views"]
                                          stringByAppendingPathComponent:activeProfile]);
    char data[4096];
    if (!view || skyrim_plugin_child_path(view.fileSystemRepresentation, "Data", data, sizeof data) != 1) return false;
    int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef O_NOFOLLOW_ANY
    flags |= O_NOFOLLOW_ANY;
#else
    flags |= O_NOFOLLOW;
#endif
    int fd = open(data, flags);
    if (fd < 0) return false;
    const bool ready = skyrim_ece_tri_configure(fd, DigestEceInput);
    close(fd);
    return ready;
}

static bool DigestHighHeelPex(const void *bytes, size_t length, uint8_t output[32]) {
    return CC_SHA256(bytes, (CC_LONG)length, output) != NULL;
}

static bool HighHeelTrialUsesStockSettings(int data) {
    static const size_t expected_size = 194;
    uint8_t *bytes = NULL;
    size_t size = 0;
    if (skse_probe_asset_read_unique(&data, "SKSE/Plugins/hdtHighHeelNative.ini", expected_size, &bytes, &size))
        return false;
    const bool stock = size == expected_size &&
        [HexDigest([NSData dataWithBytes:bytes length:size])
            isEqualToString:@"db4e58a6c5b385a4e6cec2777688b818c4b0a00634f933465b09c1b4a5a81f33"];
    free(bytes);
    return stock;
}

static HighHeelIdentityResult PrepareHighHeelObservers(NSString *game, bool wantEffect, bool *effectReady,
                                                       bool *trialSettingsReady) {
    if (launchPackageIds.count) game = CanonicalSupportPath(game);
    char data[PATH_MAX];
    if (!game || skyrim_plugin_child_path(game.fileSystemRepresentation, "Data", data, sizeof data) != 1)
        return HIGHHEEL_IDENTITY_UNREADABLE;
    int noFollow = O_NOFOLLOW;
#ifdef O_NOFOLLOW_ANY
    if (launchPackageIds.count) noFollow = O_NOFOLLOW_ANY;
#endif
    const int fd = open(data, O_RDONLY | O_DIRECTORY | O_CLOEXEC | noFollow);
    if (fd < 0) return HIGHHEEL_IDENTITY_UNREADABLE;
    const HighHeelIdentityResult result = highheel_identity_check_assets(fd, DigestHighHeelPex);
    if (result == HIGHHEEL_IDENTITY_MATCH) {
        skyrim_highheel_pex_configure(DigestHighHeelPex);
        if (wantEffect && skyrim_highheel_effect_prepare(fd, DigestHighHeelPex)) {
            skyrim_highheel_pex_observe_class(skyrim_highheel_class_observe);
            *effectReady = true;
            if (trialSettingsReady) *trialSettingsReady = HighHeelTrialUsesStockSettings(fd);
        }
    }
    close(fd);
    return result;
}

#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
static void RetireHighHeelPlayerTrial(void) {
    if (skyrim_highheel_native_retire()) skyrim_highheel_scene_cancel_player_trial();
    else fputs("PRIVATE_GUEST HighHeels player trial native retirement failed\n", stderr);
}
#endif

/* Select the guest hook groups for the features allowed so far, before the
 * guest starts. Returns the features whose groups were selected. */
static uint32_t SelectModHooks(uint32_t requested, BOOL inventoryFit, NSString *saveProfile,
                               NSString *game, SkyrimHookPlan *plan) {
    const uint32_t fuzGroups = PC_GUEST_MOD_HOOK_FUZ_MENU | PC_GUEST_MOD_HOOK_FUZ_WORLD;
    const BOOL menuObserver = skyrim_loading_menu_app_ready();
    const uint32_t host = fuzGroups | PC_GUEST_MOD_HOOK_VM_LIFECYCLE | PC_GUEST_MOD_HOOK_CHARGEN_SLIDER |
                          PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE |
                          (menuObserver ? PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE | PC_GUEST_MOD_HOOK_SAFETY_LOAD : 0);
    const uint32_t compiled = pc_guest_mod_hooks_compiled();
    skyrim_mod_hook_plan(requested, compiled, host, plan);
    if (plan->groups & fuzGroups) {
        NSArray<NSString *> *roots = DataRootsFor(launchPackageIds, nil, NULL);
        const char **paths = calloc(roots.count + 1, sizeof *paths);
        SkyrimFuzSettings settings;
        for (NSUInteger i = 0; paths && i < roots.count; ++i) paths[i] = roots[i].fileSystemRepresentation;
        const BOOL ready = paths && !skyrim_fuz_read_settings(paths, roots.count, &settings)
            && !skyrim_fuz_app_configure(&settings) && skyrim_vm_epoch_subscribe(skyrim_fuz_app_retire, NULL);
        free(paths);
        if (!ready) skyrim_mod_hook_plan(requested, compiled, host & ~fuzGroups, plan);
    }
    /* Camera diagnostics report observations while the original renderer runs. */
    if (!launchWithoutMods && !launchPluginListFailed && skyrim_camera_app_ready())
        plan->groups |= compiled & (PC_GUEST_MOD_HOOK_CAMERA_OBSERVE | PC_GUEST_MOD_HOOK_CAMERA_RENDER_OBSERVE |
                                   PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE);
    if (!launchWithoutMods && !launchPluginListFailed && menuObserver && skyrim_mobile_verbose() &&
        (compiled & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE)) plan->groups |= PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE;
    if (!launchWithoutMods && !launchPluginListFailed && skyrim_mobile_verbose() &&
        (compiled & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE) && PrepareEceTriObserver()) {
        plan->groups |= PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE;
        // Observe the stock path only when no preset substitution was selected.
        if (!(plan->groups & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER))
            plan->groups |= compiled & PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE;
    }
    if (inventoryFit && (compiled & PC_GUEST_MOD_HOOK_INVENTORY_FIT))
        plan->groups |= PC_GUEST_MOD_HOOK_INVENTORY_FIT;
    if (saveProfile && (compiled & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE) &&
        skyrim_save_app_configure(saveProfile.fileSystemRepresentation)) {
        plan->groups |= PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE;
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
        unsigned text, voices;
        NSUUID *profileId = [[NSUUID alloc] initWithUUIDString:activeProfile];
        const BOOL supportsLoad = (compiled & PC_GUEST_MOD_HOOK_FRESH_LOAD) &&
            (plan->groups & PC_GUEST_MOD_HOOK_FRESH_LOAD_REQUIRED) == PC_GUEST_MOD_HOOK_FRESH_LOAD_REQUIRED;
        if (supportsLoad && profileId && skyrim_mobile_classic_game_language(&text, &voices)) {
            uuid_t idBytes;
            [profileId getUUIDBytes:idBytes];
            if (skyrim_save_app_configure_storage(saveProfile.fileSystemRepresentation, idBytes, text, voices))
                plan->groups |= PC_GUEST_MOD_HOOK_FRESH_LOAD;
        }
#endif
    }
    const uint32_t heelGroups = PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE;
    const BOOL heelPex = (compiled & heelGroups) == heelGroups;
    const BOOL heelScene = (compiled & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE) && skyrim_camera_app_ready();
    const bool heelEffect = (compiled & PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE) != 0;
    const bool heelDiagnostics = skyrim_mobile_verbose();
    bool heelPlayerTrial = false;
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
    const char *heelOption = getenv("SKYRIM_CLASSIC_HIGHHEEL_PLAYER_TRIAL");
    const bool heelTrialRequested = heelOption && !strcmp(heelOption, "1");
    const uint32_t heelTrialGroups = heelGroups | PC_GUEST_MOD_HOOK_HIGHHEEL_TRANSFORM_REQUIRED |
                                    PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM;
    heelPlayerTrial = heelTrialRequested && pc_guest_classic_skse_active &&
                      (compiled & heelTrialGroups) == heelTrialGroups;
#endif
    if (!launchWithoutMods && !launchPluginListFailed &&
        (heelPlayerTrial || (heelDiagnostics && (heelPex || heelScene)))) {
        for (NSString *name in skyrim_mobile_loaded_plugins()) {
            if ([name caseInsensitiveCompare:@"hdtHighHeel.esm"] == NSOrderedSame) {
                uint32_t observers = heelDiagnostics && heelScene ? PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE : 0;
                bool trialReady = false;
                bool effectReady = false;
                if (heelPex) {
                    bool stockSettings = false;
                    const HighHeelIdentityResult result = PrepareHighHeelObservers(
                        game, heelEffect, &effectReady, heelPlayerTrial ? &stockSettings : NULL);
                    fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_ASSETS schema=1 result=%u\n", (unsigned)result);
                    if (result == HIGHHEEL_IDENTITY_MATCH) observers |= heelGroups;
                    if (effectReady) observers |= PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE;
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
                    if (heelPlayerTrial && effectReady && stockSettings && skyrim_highheel_scene_prepare_player_trial()) {
                        trialReady = skyrim_highheel_native_prepare(skyrim_highheel_scene_queue_player_update, NULL);
                        if (trialReady) observers |= heelTrialGroups;
                        else skyrim_highheel_scene_cancel_player_trial();
                    }
#endif
                }
                if (heelDiagnostics || trialReady) plan->groups |= observers;
                else if (effectReady) skyrim_highheel_effect_cancel();
                break;
            }
        }
    }
    PcGuestModHooks table = {.version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof table, .groups = plan->groups};
    if (plan->groups & PC_GUEST_MOD_HOOK_VM_LIFECYCLE)
        table.vm_lifecycle = (PcGuestModVmHooks){skyrim_vm_epoch_before_revert, skyrim_vm_epoch_after_load,
                                                 skyrim_classic_control_form_deleted};
    if (plan->groups & fuzGroups) skyrim_fuz_app_fill(&table);
    if (plan->groups & PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE)
        table.chargen_precache = (PcGuestModChargenHooks){SkipChargenPrecache};
    if (plan->groups & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER)
        table.chargen_slider = (PcGuestModChargenSliderHooks){RaceMenuPendingValue, TakeRaceMenuNoseHeight, RaceMenuDidApply};
    if (plan->groups & PC_GUEST_MOD_HOOK_CAMERA_OBSERVE) table.camera.observe_return = skyrim_camera_app_observe;
    if (plan->groups & PC_GUEST_MOD_HOOK_CAMERA_RENDER_OBSERVE) table.camera_render.observe = skyrim_camera_app_observe_render;
    if (plan->groups & PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE) table.camera_cull.notify = skyrim_camera_app_observe_cull;
    if (plan->groups & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE) table.loading_menu.observe = skyrim_loading_menu_app_observe;
    if (plan->groups & PC_GUEST_MOD_HOOK_SAFETY_LOAD) table.safety_load.bypass_reclamation = skyrim_safety_load_app_bypass;
    if (plan->groups & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE)
        table.ece_tri = (PcGuestModEceTriHooks){skyrim_ece_tri_app_begin, skyrim_ece_tri_app_finish};
    if (plan->groups & PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE) table.ece_nose.notify = skyrim_ece_nose_app_observe;
    if (plan->groups & PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE) skyrim_highheel_pex_fill(&table);
    if (plan->groups & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE) skyrim_highheel_scene_fill(&table);
    if (plan->groups & PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE) skyrim_highheel_effect_fill(&table);
    if (plan->groups & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE) skyrim_save_app_fill(&table);
    if (pc_guest_select_mod_hooks(plan->groups, plan->groups ? &table : NULL) == 0) {
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
        if (heelTrialRequested) {
            const bool selected = (plan->groups & PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM) != 0;
            fprintf(stderr, "PRIVATE_GUEST HighHeels player trial selected=%u\n", (unsigned)selected);
        }
#endif
        return plan->enabled;
    }
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
    if (plan->groups & PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM) RetireHighHeelPlayerTrial();
#endif
    skyrim_highheel_effect_cancel();
    /* Keep the original paths: select nothing. */
    if (pc_guest_select_mod_hooks(0, NULL) != 0) fputs("IOS_MOD_HOOKS selection failed\n", stderr);
    for (unsigned f = 0; f < SKYRIM_FEATURE_COUNT; ++f)
        if (plan->enabled >> f & 1u) plan->reason[f] = SKYRIM_HOOK_PLAN_HOST_UNAVAILABLE;
    plan->enabled = plan->groups = 0;
    return 0;
}
#endif

/* Reasons: 0 enabled, 1 host implementation unavailable, 2 MOD not in the active packages, 3 safe mode or plugin list
 * failure, 4 the guest lacks the needed hook groups (rebuild it), 5 the MOD version could not be verified. */
static void FreezeFeatures(NSDictionary *profile, NSString *game) {
    NSMutableDictionary *enabledKeys = [NSMutableDictionary dictionary];
    BOOL firstIsOriginal;
    NSArray<NSString *> *roots = DataRootsFor(launchWithoutMods ? @[] : launchPackageIds, nil, &firstIsOriginal);
    SkyrimFeatureScan scan = FeatureScanRoots(roots, firstIsOriginal);
    uint32_t mask = 0;
    unsigned reasons[SKYRIM_FEATURE_COUNT];
    uint32_t hooked = 0;
    for (unsigned f = 0; f < SKYRIM_FEATURE_COUNT; ++f) {
        reasons[f] = launchWithoutMods || launchPluginListFailed ? 3 : !FeatureImplementedInApp(f) ? 1
                   : !scan.features[f].providers ? 2 : 0;
#ifdef SKYRIM_IOS_MOD_HOOKS
        if (f == SKYRIM_FEATURE_PRECACHE_KILLER && FeatureSelected(profile, f) && !reasons[f] &&
            !VerifiedViewDLL(profile, skyrim_precache_killer_path, 108032,
                @"cec5d2b00f46867405e138702ab7fadcac949d5dded054a12f6d8ec2e70ba2e4")) reasons[f] = 5;
        if (f == SKYRIM_FEATURE_SAFETY_LOAD && FeatureSelected(profile, f) && !reasons[f] &&
            !VerifiedViewDLL(profile, skyrim_safety_load_path, 108032,
                @"5a6eb517ed331892f43f9f4516e89781a39150fe5f9564f1f9143324884892a9")) reasons[f] = 5;
        if (FeatureSelected(profile, f) && !reasons[f] && skyrim_mod_hook_requirements(f)) hooked |= 1u << f;
#endif
    }
#ifdef SKYRIM_IOS_MOD_HOOKS
    SkyrimHookPlan plan;
    if (PrepareRaceMenuPreset(profile)) hooked |= 1u << SKYRIM_FEATURE_RACEMENU;
    const BOOL inventoryFit = ![profile[@"inventoryFit"] isEqual:@NO];
    NSString *saveProfile = nil;
    if (!launchWithoutMods && !launchPluginListFailed && skyrim_mobile_verbose() &&
        [activeProfile isEqual:profile[@"id"]] && ![activeProfile isEqual:@"default"] &&
        FeatureSelected(profile, SKYRIM_FEATURE_PAPYRUSUTIL) && !reasons[SKYRIM_FEATURE_PAPYRUSUTIL])
        saveProfile = ProfileDir(profile);
    const uint32_t selected = SelectModHooks(hooked,
        inventoryFit && !launchWithoutMods && !launchPluginListFailed, saveProfile, game, &plan);
    launchSetup[@"inventoryFit"] = @(inventoryFit);
    fprintf(stderr, "IOS_INVENTORY_FIT armed=%d\n", (plan.groups & PC_GUEST_MOD_HOOK_INVENTORY_FIT) != 0);
    for (unsigned f = 0; f < SKYRIM_FEATURE_COUNT; ++f) {
        if ((hooked >> f & 1u) && !(selected >> f & 1u))
            reasons[f] = plan.reason[f] == SKYRIM_HOOK_PLAN_GUEST_HOOKS_MISSING ? 4 : 1;
    }
#else
    (void)game;
#endif
    for (unsigned f = 0; f < SKYRIM_FEATURE_COUNT; ++f) {
        if (!FeatureSelected(profile, f)) continue;
        const unsigned reason = reasons[f];
        if (!reason) { mask |= 1u << f; enabledKeys[@(skyrim_feature_key(f))] = @YES; }
        fprintf(stderr, "IOS_MOD_FEATURE id=%u requested=1 available=%u enabled=%u reason=%u\n", f,
                scan.features[f].providers ? 1u : 0u, reason ? 0u : 1u, reason);
    }
    if (enabledKeys.count) launchSetup[@"features"] = enabledKeys;
    __atomic_store_n(&launchFeatureMask, mask, __ATOMIC_RELAXED);
}

void skyrim_mods_report_features_at_exit(void) {
#ifdef SKYRIM_IOS_MOD_HOOKS
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
    if (pc_guest_mod_hooks_selected() & PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM) RetireHighHeelPlayerTrial();
#endif
    skyrim_highheel_effect_cancel();
    if (skyrim_mods_feature_enabled(SKYRIM_FEATURE_FUZ)) skyrim_fuz_app_report(1);
#endif
}

static BOOL SkyUITrialSelected(NSDictionary *profile) {
    return [profile[@"skyuiTrial"] isEqual:@YES];
}

BOOL skyrim_mods_prepare_skyui_trial(BOOL skyui_listed, NSString *game_view) {
    NSDictionary *profile = RunningProfile();
    BOOL requested = SkyUITrialSelected(profile);
    BOOL enabled = requested && !launchWithoutMods && !launchPluginListFailed && skyui_listed;
    if (pc_guest_select_classic_skse(enabled) != 0) return NO;
    if (enabled) launchSetup[@"skyuiTrial"] = @YES;
    const SkyrimSkyUIRequirements needs = requested ? SkyUIRequirementsFor(launchWithoutMods ? @[] : launchPackageIds)
        : (SkyrimSkyUIRequirements){.status = SKYRIM_SKYUI_REQ_SKYUI_ABSENT};
    FreezeFeatures(profile, game_view);
    if (requested && !enabled && !launchNotice) {
        launchNotice = launchPluginListFailed
            ? T(@"SkyUI is off for this launch because the active plugin list could not be verified.",
                @"起動時のプラグイン一覧を確認できなかったため、今回のSkyUI試験はオフです。")
            : T(@"SkyUI trial is off for this session. Enable SkyUI.esp in this profile and restart.",
                @"今回はSkyUI試験が無効です。このプロファイルでSkyUI.espを有効にして、起動し直してください。");
    }
    fprintf(stderr, "IOS_CLASSIC_SKYUI requested=%d enabled=%d\n", (int)requested, (int)enabled);
    if (requested) {
        fprintf(stderr, "IOS_CLASSIC_SKYUI_INPUTS skyui=%d skse_pex=%d support=%u\n",
                needs.status != SKYRIM_SKYUI_REQ_SKYUI_ABSENT && needs.status != SKYRIM_SKYUI_REQ_UNREADABLE,
                needs.status == SKYRIM_SKYUI_REQ_OK, needs.support_present);
        NSString *warning = SkyUIRequirementText(needs);
        if (warning) launchNotice = launchNotice ? [launchNotice stringByAppendingFormat:@"\n%@", warning] : warning;
    }
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
#ifdef SKYRIM_IOS_MOD_HOOKS
    profile[@"inventoryFit"] = [lastGood[@"inventoryFit"] isEqual:@NO] ? @NO : nil;
#endif
    profile[@"features"] = [lastGood[@"features"] isKindOfClass:NSDictionary.class] ? [lastGood[@"features"] mutableCopy] : nil;
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
/* This translation uses the English filenames. Keep it out of English and
 * Japanese game views even when the user keeps it enabled in the profile. */
static BOOL IsReviewedArabicTextPack(NSDictionary *package) {
    return [package[@"arabicText"] isEqual:@YES] ||
        [package[@"sha256"] isEqual:@"ab5059db24c89ef34a378fbf8b537fd88cd57e3ad649cbd5d18bddbff542c36a"];
}

static NSArray<NSString *> *EnabledPackageIdsForText(NSDictionary *profile, BOOL includeArabic) {
    NSMutableArray *ids = [NSMutableArray array];
    for (NSDictionary *row in profile[@"packages"]) {
        if (![row[@"on"] boolValue]) continue;
        NSDictionary *package = LoadPackage(row[@"id"]);
        if (package && (includeArabic || !IsReviewedArabicTextPack(package))) [ids addObject:row[@"id"]];
    }
    return ids;
}

static NSArray<NSString *> *EnabledPackageIds(NSDictionary *profile) {
    return EnabledPackageIdsForText(profile, YES);
}

BOOL skyrim_mods_has_arabic_text(void) {
    for (NSDictionary *row in SelectedProfile()[@"packages"])
        if ([row[@"on"] boolValue] && IsReviewedArabicTextPack(LoadPackage(row[@"id"]))) return YES;
    return NO;
}

/* Generated outputs are excluded from the source packages and source plugin list. */
static NSDictionary *GeneratedInputFingerprint(NSDictionary *profile, NSArray<NSString *> *enabled) {
    NSMutableArray<NSString *> *inputs = [NSMutableArray array];
    NSMutableSet<NSString *> *sourcePlugins = [NSMutableSet set];
    for (NSString *packageID in enabled) {
        NSDictionary *package = LoadPackage(packageID);
        if (package[@"generatedFor"] || IsReviewedArabicTextPack(package)) continue;
        [inputs addObject:packageID];
        for (id name in package[@"plugins"])
            if ([name isKindOfClass:NSString.class]) [sourcePlugins addObject:[name lowercaseString]];
    }
    char data[PATH_MAX];
    BOOL hasData = originalGameDirectory &&
        skyrim_plugin_child_path(originalGameDirectory.fileSystemRepresentation, "Data", data, sizeof data) == 1;
    /* The launch list visits explicit choices first, then inherited DLC from Data. */
    unsigned inheritedDlc = 0;
    if (hasData) skyrim_plugin_read_profile_dlc(ProfileDir(profile).fileSystemRepresentation, &inheritedDlc);
    NSMutableArray<NSString *> *choices = [NSMutableArray array];
    for (id row in profile[@"plugins"]) {
        if (![row isKindOfClass:NSDictionary.class] || ![row[@"name"] isKindOfClass:NSString.class]) continue;
        NSString *name = row[@"name"];
        const char *plugin = name.UTF8String;
        if (!skyrim_plugin_name_valid(plugin) || skyrim_plugin_is_base(plugin)) continue;
        inheritedDlc &= ~skyrim_plugin_dlc_bit(plugin);
        if (![row[@"on"] boolValue]) continue;
        NSString *key = name.lowercaseString;
        if ([sourcePlugins containsObject:key] || (hasData && skyrim_plugin_file_present(data, plugin)))
            [choices addObject:key];
    }
    if (inheritedDlc) {
        for (NSString *name in @[@"dawnguard.esm", @"hearthfires.esm", @"dragonborn.esm"])
            if ((inheritedDlc & skyrim_plugin_dlc_bit(name.UTF8String)) &&
                skyrim_plugin_file_present(data, name.UTF8String)) [choices addObject:name];
    }
    return @{@"profile":profile[@"id"],
             @"packagesSHA256":HexDigest([[inputs componentsJoinedByString:@","] dataUsingEncoding:NSUTF8StringEncoding]),
             @"pluginsSHA256":HexDigest([[choices componentsJoinedByString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding])};
}

static NSString *GeneratedOutputWarning(NSDictionary *package, NSDictionary *fingerprint) {
    id stored = package[@"generatedFor"];
    if ([stored isEqual:fingerprint]) return nil;
    if ([stored isKindOfClass:NSDictionary.class] && !stored[@"pluginsSHA256"] &&
        [stored[@"profile"] isEqual:fingerprint[@"profile"]] &&
        [stored[@"packagesSHA256"] isEqual:fingerprint[@"packagesSHA256"]])
        return T(@"The original plugin setup was not recorded. Check it before adding these generated files again.",
                 @"取り込み時のプラグイン設定が記録されていません。構成を確認してから生成ファイルを追加し直してください。");
    return T(@"These generated files were added with a different MOD setup. Regenerate for this profile.",
             @"別のMOD構成で追加した生成ファイルです。このプロファイル用に再生成してください。");
}

NSString *skyrim_mods_prepare_game(NSString *game, BOOL arabicText) {
    EnsureState();
    originalGameDirectory = game;
    launchPackageIds = nil;
    NSMutableDictionary *profile = RunningProfile();
    NSArray *ids = launchWithoutMods ? @[] : EnabledPackageIdsForText(profile, arabicText);
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
    NSDictionary *fingerprint = nil;
    for (NSString *packageID in ids) {
        NSDictionary *package = LoadPackage(packageID);
        if (!package[@"generatedFor"]) continue;
        if (!fingerprint) fingerprint = GeneratedInputFingerprint(profile, ids);
        NSString *warning = GeneratedOutputWarning(package, fingerprint);
        if (warning) {
            NSString *message = [NSString stringWithFormat:@"%@ — %@", package[@"name"] ?: @"MOD", warning];
            launchNotice = launchNotice ? [launchNotice stringByAppendingFormat:@"\n%@", message] : message;
            break;
        }
    }
    uint64_t signature = 0;
    int signatureError = skyrim_tree_signature(game.fileSystemRepresentation, &signature);
    NSString *keySource = [NSString stringWithFormat:@"v1|%llx|%@", (unsigned long long)signature, [ids componentsJoinedByString:@","]];
    NSString *key = HexDigest([keySource dataUsingEncoding:NSUTF8StringEncoding]);
    NSString *views = [Base() stringByAppendingPathComponent:@"Views"];
    [NSFileManager.defaultManager createDirectoryAtPath:views withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *view = [views stringByAppendingPathComponent:profile[@"id"]];
    NSString *conflicts = [view stringByAppendingString:@".conflicts.tsv"];
    if (!signatureError && skyrim_deploy_matches(view.fileSystemRepresentation,
                                                 conflicts.fileSystemRepresentation, key.UTF8String)) {
        launchPackageIds = [ids copy];
        return view;
    }
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
    launchPackageIds = [ids copy];
    return view;
}

/* Skyrim LE Arabic falta uses English filenames. Pin the user-provided archive
 * and the base game, Update and Interface files, in this order. DLC string
 * tables are used by the game only when their own ESMs are present. */
static BOOL ArabicTextFilesMatch(const char *data) {
    static const struct { const char *file; off_t bytes; } files[] = {
        {"FontConfig.txt", 1066}, {"fonts_en.swf", 1652614},
        {"fonts_en2.swf", 74230}, {"Translate_english.txt", 54102},
        {"Skyrim_english.STRINGS", 1331960}, {"Skyrim_english.DLSTRINGS", 2894890},
        {"Skyrim_english.ILSTRINGS", 5361511}, {"Update_english.STRINGS", 46963},
        {"Update_english.DLSTRINGS", 136390}, {"Update_english.ILSTRINGS", 25947},
    };
    char interfaceDir[PATH_MAX], stringsDir[PATH_MAX], path[PATH_MAX];
    if (skyrim_plugin_child_path(data, "Interface", interfaceDir, sizeof interfaceDir) != 1 ||
        skyrim_plugin_child_path(data, "Strings", stringsDir, sizeof stringsDir) != 1) return NO;
    CC_SHA256_CTX hash;
    if (CC_SHA256_Init(&hash) != 1) return NO;
    unsigned char buffer[16384], digest[CC_SHA256_DIGEST_LENGTH];
    for (size_t i = 0; i < sizeof files / sizeof *files; ++i) {
        if (skyrim_plugin_child_path(i < 4 ? interfaceDir : stringsDir, files[i].file, path, sizeof path) != 1)
            return NO;
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) return NO;
        struct stat st;
        BOOL valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == files[i].bytes;
        off_t remaining = files[i].bytes;
        while (valid && remaining > 0) {
            size_t count = remaining < (off_t)sizeof buffer ? (size_t)remaining : sizeof buffer;
            ssize_t got = read(fd, buffer, count);
            if (got < 0 && errno == EINTR) continue;
            valid = got > 0 && CC_SHA256_Update(&hash, buffer, (CC_LONG)got) == 1;
            if (valid) remaining -= got;
        }
        if (close(fd) || !valid) return NO;
    }
    static const unsigned char expected[] = {
        0xef, 0x16, 0x4e, 0x51, 0x0c, 0x30, 0x91, 0x5e, 0xb6, 0x79, 0x79, 0x0b, 0xf2, 0x95, 0xce, 0x21,
        0x2a, 0xa8, 0x9a, 0x9e, 0x72, 0xea, 0x0f, 0x2a, 0x38, 0x67, 0xe6, 0xd4, 0x1c, 0x20, 0x6e, 0x9c,
    };
    return CC_SHA256_Final(digest, &hash) == 1 && !memcmp(digest, expected, sizeof digest);
}

unsigned skyrim_mods_arabic_text_status(NSString *gameView) {
    for (NSString *pid in launchPackageIds) {
        NSDictionary *package = LoadPackage(pid);
        if (!IsReviewedArabicTextPack(package)) continue;
        char data[PATH_MAX];
        return skyrim_plugin_child_path(gameView.fileSystemRepresentation, "Data", data, sizeof data) == 1 &&
            ArabicTextFilesMatch(data) ? SKYRIM_ARABIC_MOD_READY : SKYRIM_ARABIC_MOD_OVERRIDDEN;
    }
    return SKYRIM_ARABIC_MOD_OFF;
}

#if defined(SKYRIM_IOS_CLASSIC_SKSE) && defined(SKYRIM_IOS_SKSE_DLL_PROBE)
int skyrim_mods_acquire_papyrus_inputs(SkyrimPapyrusInputs *inputs) {
    if (!inputs) return EINVAL;
    *inputs = (SkyrimPapyrusInputs){.settings_fd = -1, .game_fd = -1};
    if (![NSThread isMainThread]) return EPERM;
    if (launchWithoutMods || launchPluginListFailed || !ValidId(activeProfile) ||
        ![launchSetup[@"skyuiTrial"] isEqual:@YES] || !launchPackageIds.count) return ENOENT;

    NSString *directory = CanonicalSupportPath(Base());
    if (!directory.length) return ENOENT;
    int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef O_NOFOLLOW_ANY
    flags |= O_NOFOLLOW_ANY;
#else
    flags |= O_NOFOLLOW;
#endif
    int support = open(directory.fileSystemRepresentation, flags);
    if (support < 0) return errno;
    NSArray<NSString *> *packages = launchPackageIds;
    const char **ids = calloc(packages.count, sizeof *ids);
    int result = ENOMEM;
    if (ids) {
        for (NSUInteger i = 0; i < packages.count; ++i) ids[i] = [packages[i] UTF8String];
        result = skyrim_papyrus_inputs_open(support, activeProfile.UTF8String, ids, packages.count, inputs);
        free(ids);
    }
    close(support);
    return result;
}
#endif

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
/* The game overlay sits behind Settings. Keep the latest import message in
 * the MOD menu, including after dismissal of a document picker or installer. */
static NSString *const ImportNoticeChanged = @"SKModImportNoticeChanged";
static NSString *lastImportNotice; /* main thread */
static void Notice(NSString *text) {
    void (^publish)(void) = ^{
        lastImportNotice = [text copy];
        [NSNotificationCenter.defaultCenter postNotificationName:ImportNoticeChanged object:nil];
        skyrim_mobile_show_notice(text);
    };
    if (NSThread.isMainThread) publish();
    else dispatch_async(dispatch_get_main_queue(), publish);
}
static NSString *FileDigest(NSString *path) {
    FILE *f = fopen(path.fileSystemRepresentation, "rb");
    if (!f) return nil;
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
    unsigned char buf[1 << 16]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    BOOL complete = !ferror(f);
    if (fclose(f) || !complete) return nil;
    unsigned char d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &ctx);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) [s appendFormat:@"%02x", d[i]];
    return s;
}
static BOOL ImportSpaceAvailable(uint64_t bytes) {
    NSString *path = Base();
    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    id capacity = nil;
    if (![url getResourceValue:&capacity forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:NULL] ||
        ![capacity isKindOfClass:NSNumber.class] || [capacity longLongValue] < 0)
        capacity = [NSFileManager.defaultManager attributesOfFileSystemForPath:path error:NULL][NSFileSystemFreeSize];
    if (![capacity isKindOfClass:NSNumber.class] || [capacity longLongValue] < 0) {
        Notice(T(@"Could not check the free space for this MOD.", @"このMODの追加に必要な空き容量を確認できませんでした。"));
        return NO;
    }
    if (bytes > SKYRIM_ZIP_MAX_TOTAL || bytes + (256ull << 20) > [capacity unsignedLongLongValue]) {
        Notice(T(@"Not enough free space on this iPhone for this MOD.", @"このMODを入れるための空き容量が足りません。"));
        return NO;
    }
    return YES;
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
        @"hkx64":@(r.hkx_64bit), @"scripts":@(r.scripts), @"skseScripts":@(r.skse_scripts), @"mcm":@(r.mcm_scripts), @"skseDlls":@(r.skse_dlls),
        @"skseMissing":@(r.skse_missing)};
    return YES;
}

static BOOL ProfileHasPackage(NSDictionary *profile, NSString *packageID) {
    for (NSDictionary *row in profile[@"packages"])
        if ([row[@"id"] isEqual:packageID]) return YES;
    return NO;
}

/* Main-thread only. Both new and known packages use this profile transaction. */
static void AttachPackage(NSMutableDictionary *profile, NSDictionary *package) {
    NSString *name = [package[@"name"] isKindOfClass:NSString.class] ? package[@"name"] : T(@"MOD", @"MOD");
    if (!profile) {
        Notice([NSString stringWithFormat:T(@"%@ is installed, but the selected profile was removed.",
            @"%@ は取り込み済みですが、選んだプロファイルが削除されています。"), name]);
        return;
    }
    NSString *profileName = ProfileName(profile);
    NSString *packageID = package[@"id"];
    if (ProfileHasPackage(profile, packageID)) {
        Notice([NSString stringWithFormat:T(@"%@ is already in %@.", @"%@ は「%@」に追加済みです。"),
            name, profileName]);
        return;
    }

    NSMutableArray *oldPackages = profile[@"packages"];
    NSMutableArray *oldPlugins = profile[@"plugins"];
    NSMutableArray *packages = [oldPackages mutableCopy];
    NSMutableArray *plugins = [oldPlugins mutableCopy];
    [packages addObject:[@{@"id":packageID, @"on":@YES} mutableCopy]];
    NSMutableSet *configured = [NSMutableSet set];
    for (NSDictionary *row in oldPlugins)
        if ([row isKindOfClass:NSDictionary.class] && [row[@"name"] isKindOfClass:NSString.class])
            [configured addObject:[row[@"name"] lowercaseString]];
    NSArray *names = [package[@"plugins"] isKindOfClass:NSArray.class] ? package[@"plugins"] : @[];
    for (NSString *plugin in names) {
        if (![plugin isKindOfClass:NSString.class] || !plugin.length) continue;
        NSString *key = plugin.lowercaseString;
        if ([configured containsObject:key]) continue;
        [plugins addObject:[@{@"name":plugin, @"on":@YES} mutableCopy]];
        [configured addObject:key];
    }

    profile[@"packages"] = packages;
    profile[@"plugins"] = plugins;
    if (!SaveState()) {
        profile[@"packages"] = oldPackages;
        profile[@"plugins"] = oldPlugins;
        Notice([NSString stringWithFormat:T(@"Could not add %@ to %@.",
            @"%@ を「%@」に追加できませんでした。"), name, profileName]);
        return;
    }
    [NSNotificationCenter.defaultCenter postNotificationName:@"SKModsChanged" object:nil];
    NSString *added = [NSString stringWithFormat:T(@"Added %@ to profile \"%@\". Save your game, then fully close and reopen the app.",
        @"%@ をプロファイル「%@」に追加しました。ゲーム内で保存し、アプリを完全終了して開き直すと反映します。"), name, profileName];
    if (IsReviewedArabicTextPack(package))
        added = [added stringByAppendingFormat:@"\n%@", T(
            @"Choose العربية under Settings → Game text, then restart the app.",
            @"設定の「ゲームのテキスト」で「العربية」を選び、アプリを再起動してください。")];
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    BOOL providesSkyUI = NO;
    for (id plugin in names) if ([plugin isKindOfClass:NSString.class] && [[plugin lowercaseString] isEqualToString:@"skyui.esp"]) providesSkyUI = YES;
    NSString *skyuiWarning = providesSkyUI ? SkyUIRequirementText(SkyUIRequirementsFor(EnabledPackageIds(profile))) : nil;
    if (skyuiWarning) added = [added stringByAppendingFormat:@"\n%@", skyuiWarning];
#endif
    Notice(added);
}

static BOOL ReuseInstalledArchive(NSString *digest, NSString *targetProfileID) {
    if (!digest.length) return NO;
    NSMutableArray<NSString *> *matchingIDs = [NSMutableArray array];
    for (NSDictionary *candidate in InstalledPackages())
        if ([candidate[@"sha256"] isEqual:digest]) [matchingIDs addObject:candidate[@"id"]];
    if (!matchingIDs.count) return NO;

    __block BOOL handled = NO;
    dispatch_sync(dispatch_get_main_queue(), ^{
        EnsureState();
        NSMutableDictionary *profile = ProfileById(targetProfileID);
        NSDictionary *match = nil;
        BOOL ambiguous = NO;
        for (NSString *packageID in matchingIDs) {
            NSDictionary *candidate = LoadPackage(packageID);
            if (![candidate[@"sha256"] isEqual:digest]) continue;
            if (ProfileHasPackage(profile, packageID)) {
                match = candidate;
                ambiguous = NO;
                break;
            }
            ambiguous |= match != nil;
            match = candidate;
        }
        if (!match) return; // The installed package changed. Use the staged archive.
        handled = YES;
        if (ambiguous) {
            Notice(T(@"This archive has more than one installed setup.",
                @"このアーカイブは複数の構成で取り込み済みです。"));
            return;
        }
        AttachPackage(profile, match);
    });
    return handled;
}

/* Publish the staged package on the import queue, then attach its immutable
 * metadata to the captured profile on the main thread. */
static void PublishStaged(NSString *staging, NSMutableDictionary *package, NSString *targetProfileID) {
    NSString *data = [staging stringByAppendingPathComponent:@"Data"];
    NSString *message = nil;
    if (!ProcessStaged(data, package, &message)) {
        skyrim_remove_tree(staging.fileSystemRepresentation);
        Notice(message ?: T(@"This MOD cannot be used.", @"このMODは使えません。"));
        return;
    }
    package[@"plugins"] = PluginsIn(data);
    if (ArabicTextFilesMatch(data.fileSystemRepresentation)) package[@"arabicText"] = @YES;
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
        AttachPackage(ProfileById(targetProfileID), package);
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

/* Add installed files in this directory and return the unresolved choices. */
static unsigned AddActiveOfficialDlc(NSMutableSet<NSString *> *active, NSString *data, unsigned selected) {
    if (!data || !selected) return selected;
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *name in [fm contentsOfDirectoryAtPath:data error:NULL]) {
        unsigned bit = skyrim_plugin_dlc_bit(name.UTF8String);
        if (!(selected & bit)) continue;
        NSDictionary *attrs = [fm attributesOfItemAtPath:[data stringByAppendingPathComponent:name] error:NULL];
        if (![attrs[NSFileType] isEqual:NSFileTypeRegular]) continue;
        [active addObject:name.lowercaseString];
        selected &= ~bit;
        if (!selected) break;
    }
    return selected;
}

static NSSet<NSString *> *InstallerActivePlugins(NSDictionary *profile) {
    NSMutableSet *active = [NSMutableSet setWithObjects:@"skyrim.esm", @"update.esm", nil];
    unsigned inherited = 0, explicitOn = 0;
    int error = skyrim_plugin_read_profile_dlc(ProfileDir(profile).fileSystemRepresentation, &inherited);
    if (error) fprintf(stderr, "IOS_MODS fomod_dlc_choices_read=%d\n", error);
    for (NSDictionary *row in EffectivePlugins(profile)) {
        NSString *name = row[@"name"];
        unsigned bit = skyrim_plugin_dlc_bit(name.UTF8String);
        inherited &= ~bit;  /* An explicit choice takes precedence even when off. */
        if (![row[@"on"] boolValue]) continue;
        if (bit) explicitOn |= bit;
        else [active addObject:name.lowercaseString];
    }
    char data[PATH_MAX];
    if ((inherited || explicitOn) &&
        skyrim_plugin_child_path(originalGameDirectory.fileSystemRepresentation, "Data", data, sizeof data) == 1)
        explicitOn &= AddActiveOfficialDlc(active, [NSString stringWithUTF8String:data], inherited | explicitOn);
    if (explicitOn) for (NSString *package in EnabledPackageIds(profile)) {
        explicitOn = AddActiveOfficialDlc(active, [PackageDir(package) stringByAppendingPathComponent:@"Data"], explicitOn);
        if (!explicitOn) break;
    }
    return active;
}

/* raw holds the whole extracted archive (inside staging). Shows the installer
 * on the main thread; publishes on the import queue when it finishes. */
static void RunInstallerFromRaw(NSString *staging, NSString *raw, NSMutableDictionary *package, NSString *targetProfileID) {
    NSString *config = FindModuleConfig(raw);
    if (!config) {
        skyrim_remove_tree(staging.fileSystemRepresentation);
        Notice(T(@"The installer file was not found.", @"インストーラーの設定ファイルが見つかりませんでした。"));
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        EnsureState();
        NSDictionary *profile = ProfileById(targetProfileID);
        if (!profile) {
            dispatch_async(ImportQueue(), ^{ skyrim_remove_tree(staging.fileSystemRepresentation); });
            Notice(T(@"The profile for this MOD was removed. Choose a profile and add it again.",
                @"このMODの追加先のプロファイルが削除されました。プロファイルを選び直してから追加してください。"));
            return;
        }
        NSSet *active = InstallerActivePlugins(profile);
        skyrim_fomod_run(package[@"name"] ?: package[@"archive"] ?: @"MOD", raw, config, [staging stringByAppendingPathComponent:@"Data"], active, ^(BOOL installed, NSString *message) {
            dispatch_async(ImportQueue(), ^{
                skyrim_remove_tree(raw.fileSystemRepresentation);
                if (!installed) {
                    skyrim_remove_tree(staging.fileSystemRepresentation);
                    Notice(message ?: T(@"MOD installation canceled.", @"MODの追加を取り消しました。"));
                    return;
                }
                package[@"installer"] = @YES;
                PublishStaged(staging, package, targetProfileID);
            });
        });
    });
}
static BOOL RunInstaller(SkyrimZip *zip, NSMutableDictionary *package, NSString *targetProfileID) {
    NSString *staging = NewStaging();
    if (!staging) return NO;
    NSString *raw = [staging stringByAppendingPathComponent:@"raw"];
    [NSFileManager.defaultManager createDirectoryAtPath:raw withIntermediateDirectories:YES attributes:nil error:NULL];
    int status = SKYRIM_ZIP_OK;
    for (size_t i = 0; i < zip->count && !status; ++i)
        if (!zip->entries[i].is_dir) status = skyrim_zip_extract(zip, i, raw.fileSystemRepresentation, zip->entries[i].path);
    if (status) { skyrim_remove_tree(staging.fileSystemRepresentation); Notice(ZipErrorText(status)); return YES; }
    RunInstallerFromRaw(staging, raw, package, targetProfileID);
    return YES;
}

/* 7z / rar through libarchive, when the build provides it. */
static BOOL ImportOther(NSString *path, NSString *name, NSString *digest, NSString *targetProfileID) {
    NSString *ext = name.pathExtension.lowercaseString;
    if (![ext isEqualToString:@"7z"] && ![ext isEqualToString:@"rar"]) return NO;
#ifdef SKYRIM_HAVE_LIBARCHIVE
    SkyrimArchiveList list;
    int status = skyrim_archive_list(path.fileSystemRepresentation, &list);
    if (status) { Notice(ZipErrorText(status)); return YES; }
    if (!ImportSpaceAvailable(list.total)) {
        skyrim_archive_list_free(&list);
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
        RunInstallerFromRaw(staging, raw, package, targetProfileID);
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
    PublishStaged(staging, package, targetProfileID);
    return YES;
#else
    (void)path; (void)digest; (void)targetProfileID;
    Notice(T(@"This build cannot open 7z or RAR archives. Choose a ZIP file or folder.",
             @"このビルドは7z・RAR形式に対応していません。ZIPファイルまたはフォルダを選んでください。"));
    return YES;
#endif
}

static void ImportZip(NSString *path, NSString *name, NSString *digest, NSString *targetProfileID) {
    SkyrimZip zip;
    int status = skyrim_zip_open(path.fileSystemRepresentation, &zip);
    if (status) { Notice(ZipErrorText(status)); return; }
    NSMutableDictionary *package = [@{@"id":NSUUID.UUID.UUIDString, @"name":name.stringByDeletingPathExtension,
        @"archive":name, @"sha256":digest ?: @"", @"imported":@(NSDate.date.timeIntervalSince1970)} mutableCopy];
    if (!ImportSpaceAvailable(zip.total)) {
        skyrim_zip_close(&zip);
        return;
    }
    const char **paths = calloc(zip.count ? zip.count : 1, sizeof *paths);
    size_t n = 0;
    for (size_t i = 0; i < zip.count; ++i) if (!zip.entries[i].is_dir) paths[n++] = zip.entries[i].path;
    char prefix[1024]; int installer = 0;
    int found = skyrim_mod_data_root(paths, n, prefix, sizeof prefix, &installer);
    free(paths);
    if (installer && RunInstaller(&zip, package, targetProfileID)) { skyrim_zip_close(&zip); return; }
    if (!found) {
        skyrim_zip_close(&zip);
        Notice(installer ? T(@"This MOD needs its installer, which is not available in this version.", @"このMODはインストーラーが必要ですが、この版では使えません。")
                         : T(@"No Skyrim game files were found in this archive.", @"この圧縮ファイルにSkyrimのゲーム用ファイルが見つかりませんでした。"));
        return;
    }
    NSString *staging = NewStaging();
    if (!staging) { skyrim_zip_close(&zip); Notice(T(@"Could not save the MOD.", @"MODを保存できませんでした。")); return; }
    NSString *data = [staging stringByAppendingPathComponent:@"Data"];
    size_t files = 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i < zip.count && !status; ++i) {
        SkyrimZipEntry *e = &zip.entries[i];
        if (e->is_dir) continue;
        const char *rel = skyrim_archive_relative_path(e->path, prefix);
        if (!rel || !strncasecmp(rel, "fomod/", 6)) continue;
        status = skyrim_zip_extract(&zip, i, data.fileSystemRepresentation, rel);
        ++files; bytes += e->size;
    }
    skyrim_zip_close(&zip);
    if (status) { skyrim_remove_tree(staging.fileSystemRepresentation); Notice(ZipErrorText(status)); return; }
    package[@"files"] = @(files); package[@"bytes"] = @(bytes);
    package[@"dataPrefix"] = [NSString stringWithUTF8String:prefix];
    PublishStaged(staging, package, targetProfileID);
}

static void ImportRaceMenuPreset(NSString *path, NSString *name, NSString *digest, NSString *targetProfileID) {
    uint64_t bytes = [[NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL] fileSize];
    if (!ImportSpaceAvailable(bytes)) return;
    NSString *staging = NewStaging();
    NSString *data = [staging stringByAppendingPathComponent:@"Data"];
    if (!staging || skyrim_mod_stage_racemenu_preset(path.fileSystemRepresentation, name.fileSystemRepresentation,
                                                  data.fileSystemRepresentation)) {
        if (staging) skyrim_remove_tree(staging.fileSystemRepresentation);
        Notice(T(@"Could not add the RaceMenu preset.", @"RaceMenuプリセットを追加できませんでした。"));
        return;
    }
    NSMutableDictionary *package = [@{@"id":NSUUID.UUID.UUIDString, @"name":name.stringByDeletingPathExtension,
        @"archive":name, @"sha256":digest ?: @"", @"imported":@(NSDate.date.timeIntervalSince1970),
        @"files":@1, @"bytes":@(bytes)} mutableCopy];
    PublishStaged(staging, package, targetProfileID);
}

static void ImportEceSlot(NSString *staged, NSString *name, NSString *targetProfileID) {
    // The serial import queue copied the file into Inbox first. Hold the main
    // queue only for profile lookup and the final metadata-only publication.
    dispatch_sync(dispatch_get_main_queue(), ^{
        EnsureState();
        NSDictionary *profile = ProfileById(targetProfileID);
        NSString *directory = CanonicalSupportPath(ProfileDir(profile));
        int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef O_NOFOLLOW_ANY
        flags |= O_NOFOLLOW_ANY;
#else
        flags |= O_NOFOLLOW;
#endif
        int fd = directory ? open(directory.fileSystemRepresentation, flags) : -1;
        if (fd < 0) {
            Notice(T(@"The selected profile is unavailable.", @"選んだプロファイルを開けませんでした。"));
            return;
        }
        unsigned slot = 0;
        int error = skyrim_mod_publish_ece_slot(fd, staged.fileSystemRepresentation, name.fileSystemRepresentation, &slot);
        close(fd);
        if (!slot) {
            Notice(T(@"Choose a numbered ECE slot such as _1_commonTriRaces.slot (1–40).",
                @"_1_commonTriRaces.slot のような、1〜40番のECEスロットを選んでください。"));
        } else if (error == EEXIST) {
            Notice([NSString stringWithFormat:T(@"Slot %u in %@ is already in use.",
                @"スロット%uは「%@」で使用中です。"), slot, ProfileName(profile)]);
        } else if (error) {
            Notice(T(@"Could not prepare the ECE slot.", @"ECEスロットを用意できませんでした。"));
        } else {
            Notice([NSString stringWithFormat:T(@"Stored ECE slot %u in %@. Applying it in character creation is under verification.",
                @"ECEスロット%uを「%@」に保存しました。キャラクター作成画面での反映は検証中です。"), slot, ProfileName(profile)]);
        }
    });
}

/* Import a folder chosen in the Files app (copied; the source is unchanged). */
static void ImportFolder(NSString *folder, NSString *targetProfileID, NSDictionary *generatedFor) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableArray<NSString *> *rels = [NSMutableArray array];
    NSDirectoryEnumerator *en = [fm enumeratorAtPath:folder];
    for (NSString *rel in en)
        if ([en.fileAttributes[NSFileType] isEqual:NSFileTypeRegular]) [rels addObject:rel];
    const char **paths = calloc(rels.count ? rels.count : 1, sizeof *paths);
    if (!paths) { Notice(T(@"Could not read the folder.", @"フォルダを読み込めませんでした。")); return; }
    for (NSUInteger i = 0; i < rels.count; ++i) paths[i] = rels[i].UTF8String;
    char prefix[1024]; int installer = 0;
    int found = skyrim_mod_data_root(paths, rels.count, prefix, sizeof prefix, &installer);
    free(paths);
    if (installer && generatedFor) {
        Notice(T(@"Select the generated output folder, without a MOD installer.",
                 @"MODインストーラーを含まない生成済み出力フォルダを選んでください。"));
        return;
    }
    if (!found && !installer) {
        Notice(T(@"No Skyrim game files were found in this folder.", @"このフォルダにSkyrimのゲーム用ファイルが見つかりませんでした。"));
        return;
    }
    NSString *staging = NewStaging();
    if (!staging) { Notice(T(@"Could not save the MOD.", @"MODを保存できませんでした。")); return; }
    NSString *destination = [staging stringByAppendingPathComponent:installer ? @"raw" : @"Data"];
    int status = SKYRIM_ZIP_OK;
    if (installer && ![fm createDirectoryAtPath:destination withIntermediateDirectories:YES attributes:nil error:NULL])
        status = SKYRIM_ZIP_WRITE;
    uint64_t bytes = 0; NSUInteger files = 0;
    for (NSString *rel in rels) {
        if (status) break;
        const char *inner = skyrim_archive_relative_path(rel.UTF8String, installer ? NULL : prefix);
        if (!inner) continue;
        char safe[1024];
        int normalized = skyrim_zip_normalize(inner, strlen(inner), 1, safe, sizeof safe);
        if (normalized != 1) {
            if (installer) status = SKYRIM_ZIP_UNSAFE_PATH;
            continue;
        }
        if (!installer && !strncasecmp(safe, "fomod/", 6)) continue;
        NSString *source = [folder stringByAppendingPathComponent:rel];
        if (skyrim_make_parents(destination.fileSystemRepresentation, safe) ||
            skyrim_clone_file(source.fileSystemRepresentation,
                              [destination stringByAppendingPathComponent:@(safe)].fileSystemRepresentation)) {
            status = SKYRIM_ZIP_WRITE;
            break;
        }
        bytes += [[fm attributesOfItemAtPath:source error:NULL] fileSize];
        ++files;
    }
    if (status || !files) {
        skyrim_remove_tree(staging.fileSystemRepresentation);
        Notice(status == SKYRIM_ZIP_UNSAFE_PATH ? ZipErrorText(status) :
               T(@"Could not copy the folder.", @"フォルダをコピーできませんでした。"));
        return;
    }
    NSMutableDictionary *package = [@{@"id":NSUUID.UUID.UUIDString, @"name":folder.lastPathComponent, @"archive":folder.lastPathComponent,
        @"sha256":@"", @"imported":@(NSDate.date.timeIntervalSince1970)} mutableCopy];
    if (installer) {
        // The installer operates on the staged copy after Files access ends.
        RunInstallerFromRaw(staging, destination, package, targetProfileID);
        return;
    }
    package[@"files"] = @(files);
    package[@"bytes"] = @(bytes);
    package[@"dataPrefix"] = [NSString stringWithUTF8String:prefix];
    if (generatedFor) package[@"generatedFor"] = generatedFor;
    PublishStaged(staging, package, targetProfileID);
}

static void ImportModList(NSString *path);

static void ImportURL(NSURL *url, BOOL scoped, BOOL generated) {
    if (!url.isFileURL) {
        Notice(T(@"Select a file saved in the Files app.", @"「ファイル」アプリに保存したファイルを選んでください。"));
        return;
    }
    /* A profile can change while the archive is being unpacked or optimized. */
    __block NSString *targetProfileID;
    __block NSDictionary *generatedFor;
    void (^captureProfile)(void) = ^{
        EnsureState();
        NSDictionary *profile = SelectedProfile();
        targetProfileID = [profile[@"id"] copy];
        if (generated) generatedFor = GeneratedInputFingerprint(profile, EnabledPackageIds(profile));
    };
    if (NSThread.isMainThread) captureProfile();
    else dispatch_sync(dispatch_get_main_queue(), captureProfile);

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
            ImportFolder(source, targetProfileID, generatedFor);
            if (accessing) [url stopAccessingSecurityScopedResource];
        });
        return;
    }
    if (generatedFor) {
        if (accessing) [url stopAccessingSecurityScopedResource];
        Notice(T(@"Select a generated output folder.", @"生成済み出力フォルダを選んでください。"));
        return;
    }
    NSString *copy = [inbox stringByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingPathExtension:name.pathExtension]];
    void (^stageArchive)(void) = ^{
        __block BOOL copied = NO;
        if (scoped) {
            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            [coordinator coordinateReadingItemAtURL:url options:0 error:NULL byAccessor:^(NSURL *readURL) {
                copied = [NSFileManager.defaultManager copyItemAtPath:readURL.path toPath:copy error:NULL];
            }];
        } else {
            copied = [NSFileManager.defaultManager copyItemAtPath:url.path toPath:copy error:NULL];
        }
        if (accessing) [url stopAccessingSecurityScopedResource];
        if (!copied) {
            [NSFileManager.defaultManager removeItemAtPath:copy error:NULL];
            Notice(T(@"Could not open the selected file. Try saving a local copy in Files.",
                     @"選んだファイルを開けませんでした。「ファイル」アプリでこのiPhone内にコピーしてみてください。"));
            return;
        }
        Notice([NSString stringWithFormat:T(@"Adding %@…", @"%@ を追加しています…"), name]);
        dispatch_async(ImportQueue(), ^{
            NSString *ext = name.pathExtension.lowercaseString;
            if ([ext isEqualToString:@"json"]) dispatch_sync(dispatch_get_main_queue(), ^{ ImportModList(copy); });
            else if ([ext isEqualToString:@"slot"]) ImportEceSlot(copy, name, targetProfileID);
            else {
                NSString *digest = FileDigest(copy);
                if (!ReuseInstalledArchive(digest, targetProfileID)) {
                    if ([ext isEqualToString:@"zip"]) ImportZip(copy, name, digest, targetProfileID);
                    else if ([ext isEqualToString:@"jslot"]) ImportRaceMenuPreset(copy, name, digest, targetProfileID);
                    else if (!ImportOther(copy, name, digest, targetProfileID))
                        Notice(T(@"This file type is not supported.", @"このファイル形式には対応していません。"));
                }
            }
            [NSFileManager.defaultManager removeItemAtPath:copy error:NULL];
        });
    };
    if (scoped) {
        Notice([NSString stringWithFormat:T(@"Reading %@…", @"%@ を読み込んでいます…"), name]);
        dispatch_async(ImportQueue(), stageArchive);
    } else {
        // The built-in browser releases its temporary download after this call.
        stageArchive();
    }
}

void skyrim_mods_import_url(NSURL *url, BOOL scoped) { ImportURL(url, scoped, NO); }

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
        Notice(T(@"This is not a SkyrimIOS MOD list.", @"SkyrimIOSのMOD構成ファイルではありません。"));
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
    Notice(missing.count ?
        [NSString stringWithFormat:T(@"Created profile \"%@\". Missing MODs: %@", @"プロファイル「%@」を作成しました。足りないMOD：%@"), name, [missing componentsJoinedByString:@", "]] :
        [NSString stringWithFormat:T(@"Created profile \"%@\" with all MODs.", @"プロファイル「%@」を作成しました（MODはすべてそろっています）。"), name]);
}

/* ---- conflicts --------------------------------------------------------- */
static NSArray<NSDictionary *> *ConflictSummary(void) {
    NSArray *ids = launchPackageIds;
    if (!ids.count) return @[];
    NSString *view = [[Base() stringByAppendingPathComponent:@"Views"] stringByAppendingPathComponent:activeProfile ?: @""];
    NSString *text = [NSString stringWithContentsOfFile:[view stringByAppendingString:@".conflicts.tsv"] encoding:NSUTF8StringEncoding error:NULL];
    if (!text) return @[];
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
static NSString *ProfileSavesDir(NSDictionary *profile) {
    return [ProfileDir(profile) stringByAppendingPathComponent:@"Documents/My Games/Skyrim/Saves"];
}
static NSString *SavesDir(void) { return ProfileSavesDir(RunningProfile()); }
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

/* Resolve Apple's container alias only. Open paths below it without following links. */
static NSString *CanonicalSupportPath(NSString *path) {
    NSString *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                            inDomains:NSUserDomainMask].firstObject.path;
    NSString *prefix = [support stringByAppendingString:@"/"];
    if (!support.length || ![path hasPrefix:prefix]) return nil;
    char resolved[PATH_MAX];
    if (!realpath(support.fileSystemRepresentation, resolved)) return nil;
    NSString *root = [NSFileManager.defaultManager stringWithFileSystemRepresentation:resolved length:strlen(resolved)];
    return [root stringByAppendingPathComponent:[path substringFromIndex:prefix.length]];
}

static NSString *RelativeSavesDir(NSDictionary *profile) {
    NSString *prefix = [Base() stringByAppendingString:@"/"];
    NSString *path = ProfileSavesDir(profile);
    return prefix.length && [path hasPrefix:prefix] ? [path substringFromIndex:prefix.length] : nil;
}

static dispatch_queue_t SaveCopyQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("SkyrimIOS.save-copy",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    });
    return queue;
}

static int CopySelectedSave(NSString *source, NSString *target, NSString *profileDirectory,
                            NSString *profileID, NSString *name, int *side) {
    NSString *path = CanonicalSupportPath(Base());
    if (!path.length) return ENOENT;
    int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef O_NOFOLLOW_ANY
    flags |= O_NOFOLLOW_ANY;
#else
    flags |= O_NOFOLLOW;
#endif
    int root = open(path.fileSystemRepresentation, flags);
    if (root < 0) return errno;
#if defined(SKYRIM_IOS_CLASSIC_PAPYRUSUTIL) && defined(SKYRIM_IOS_MOD_HOOKS)
    NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:profileID];
    uuid_t bytes;
    if (uuid) [uuid getUUIDBytes:bytes];
    NSString *profile = CanonicalSupportPath(profileDirectory);
    int error = skyrim_save_app_copy_selected(root, source.fileSystemRepresentation, target.fileSystemRepresentation,
        profile.fileSystemRepresentation, uuid ? bytes : NULL, name.fileSystemRepresentation, side);
#else
    (void)profileDirectory; (void)profileID;
    int error = skse_copy_named_save(root, source.fileSystemRepresentation, target.fileSystemRepresentation,
                                    name.fileSystemRepresentation, side);
#endif
    close(root);
    return error;
}

static NSString *saveCopyTargetID; /* main thread, one explicit copy at a time */

static NSString *SaveCopyMessage(int error, int side, NSString *profile) {
    switch (error) {
    case 0:
        if (side & 2) return [NSString stringWithFormat:T(@"Copied the save and its StorageUtil data to %@.",
                                                          @"%@ にセーブとStorageUtilの追加データをコピーしました。"), profile];
        return [NSString stringWithFormat:side ? T(@"Copied both .ess and .skse files to %@.", @"%@ に .ess と .skse の両方をコピーしました。") :
            T(@"Copied the .ess file to %@.", @"%@ に .ess をコピーしました。"), profile];
    case EEXIST:
        return T(@"A save or companion with this name already exists.", @"同じ名前のセーブまたは追加セーブがあります。");
    case ESTALE:
        return T(@"The save or its companion changed during copying.", @"コピー中にセーブまたは追加セーブが変わりました。");
    case ENOSPC:
        return T(@"There is not enough space for the save copy.", @"セーブのコピーに必要な空き容量がありません。");
    case EBUSY:
        return T(@"The companion save is being written. Try copying again shortly.",
                 @"追加セーブを保存中です。少し待ってからコピーし直してください。");
    case ENOTSUP:
        return T(@"Choose a named profile to carry over the StorageUtil data.",
                 @"StorageUtilの追加データを引き継ぐには、名前付きプロファイルを選んでください。");
    case ENOENT:
        return T(@"The save, its companion, or the profile folder is unavailable.", @"セーブ、追加セーブ、またはプロファイルのフォルダが見つかりません。");
    default:
        return T(@"The save file and its optional companion could not be copied.", @"セーブと、同名の追加セーブがある場合はそのファイルをコピーできませんでした。");
    }
}

@interface SKSaveCopyProfiles : UITableViewController
@property(nonatomic, copy) NSString *sourceID, *saveName;
@property(nonatomic, copy) NSArray<NSDictionary *> *targets;
@end

@implementation SKSaveCopyProfiles
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = T(@"Copy save to profile", @"セーブのコピー先");
    NSMutableArray<NSMutableDictionary *> *targets = [NSMutableArray array];
    NSCountedSet *names = [NSCountedSet set];
    NSUInteger number = 0;
    for (NSDictionary *profile in Profiles()) {
        ++number;
        NSString *pid = profile[@"id"];
        if ([pid isEqual:self.sourceID]) continue;
        NSString *name = ProfileName(profile);
        [names addObject:name];
        [targets addObject:[@{@"id":pid, @"name":name, @"number":@(number)} mutableCopy]];
    }
    for (NSMutableDictionary *target in targets)
        if ([names countForObject:target[@"name"]] > 1)
            target[@"detail"] = [NSString stringWithFormat:T(@"Profile %lu in the list", @"一覧で%lu番目のプロファイル"),
                [target[@"number"] unsignedLongValue]];
    self.targets = targets;
}
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section {
    (void)table; (void)section;
    return self.targets.count ? (NSInteger)self.targets.count : 1;
}
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)item {
    (void)table;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    if (!self.targets.count) {
        cell.textLabel.text = T(@"No other profiles available", @"コピー先にできるプロファイルがありません");
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        return cell;
    }
    NSDictionary *target = self.targets[item.row];
    cell.textLabel.text = target[@"name"];
    cell.detailTextLabel.text = target[@"detail"];
    return cell;
}
- (void)reportCopy:(int)error side:(int)side profile:(NSString *)profile {
    NSString *message = SaveCopyMessage(error, side, profile);
    if (!self.viewIfLoaded.window || self.navigationController.topViewController != self) {
        skyrim_mobile_show_notice(message);
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:error ? T(@"Could not copy save", @"セーブをコピーできませんでした") :
        T(@"Save copied", @"セーブをコピーしました") message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)item {
    [table deselectRowAtIndexPath:item animated:YES];
    if (saveCopyTargetID || self.presentedViewController || (NSUInteger)item.row >= self.targets.count) return;
    NSString *targetID = self.targets[item.row][@"id"];
    NSDictionary *source = ProfileById(self.sourceID), *target = ProfileById(targetID);
    NSString *sourcePath = RelativeSavesDir(source), *targetPath = RelativeSavesDir(target);
    if (![self.sourceID isEqual:activeProfile] || [targetID isEqual:activeProfile] || !sourcePath || !targetPath) {
        [self reportCopy:ENOENT side:0 profile:nil];
        return;
    }
    NSString *targetName = ProfileName(target), *saveName = self.saveName, *targetDirectory = ProfileDir(target);
    saveCopyTargetID = targetID;
    self.navigationItem.hidesBackButton = YES;
    UIGestureRecognizer *back = self.navigationController.interactivePopGestureRecognizer;
    BOOL backEnabled = back.enabled;
    back.enabled = NO;
    table.userInteractionEnabled = NO;
    UIActivityIndicatorView *progress = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    progress.accessibilityLabel = T(@"Copying save", @"セーブをコピー中");
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithCustomView:progress];
    [progress startAnimating];
    dispatch_async(SaveCopyQueue(), ^{
        int side = 0;
        int error = CopySelectedSave(sourcePath, targetPath, targetDirectory, targetID, saveName, &side);
        dispatch_async(dispatch_get_main_queue(), ^{
            saveCopyTargetID = nil;
            self.navigationItem.hidesBackButton = NO;
            self.navigationItem.rightBarButtonItem = nil;
            back.enabled = backEnabled;
            table.userInteractionEnabled = YES;
            [self reportCopy:error side:side profile:targetName];
        });
    });
}
@end

@interface SKSaves : UITableViewController
@property(nonatomic, strong) NSArray<NSDictionary *> *saves;
@property(nonatomic, copy) NSString *sourceProfileID;
#ifdef SKYRIM_IOS_CLASSIC_SKSE
@property(nonatomic, copy) NSString *companionDirectory;
@property(nonatomic) BOOL checkingCompanion;
#endif
@end
@implementation SKSaves
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = T(@"Save check", @"セーブの確認"); self.saves = nil;
    self.sourceProfileID = activeProfile;
    NSString *dir = SavesDir(); NSSet *loaded = LoadedPluginSet();
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    self.companionDirectory = CanonicalSupportPath(dir);
#endif
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray *saves = SaveChecksIn(dir, loaded, 50);
        dispatch_async(dispatch_get_main_queue(), ^{ self.saves = saves; [self.tableView reloadData]; });
    });
}
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { (void)t; (void)s; return self.saves.count ? (NSInteger)self.saves.count : 1; }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s {
    (void)t; (void)s;
    return T(@"Swipe a save left to copy it to another profile. The check uses this launch's plugin list and available files. Saving again after loading without a MOD removes its items and changes from the save.",
             @"セーブを左にスワイプすると別のプロファイルへコピーできます。確認は今回のプラグイン一覧と利用できるファイルに基づきます。使っていたMODを外して読み込み、保存すると、そのMODの物や変化がセーブから消えます。");
}
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    (void)t;
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    c.selectionStyle = UITableViewCellSelectionStyleNone; c.detailTextLabel.numberOfLines = 0;
    if (!self.saves.count) { c.textLabel.text = self.saves ? T(@"No saves in this profile", @"このプロファイルにセーブはありません") : T(@"Loading…", @"読み込み中…"); return c; }
    NSDictionary *save = self.saves[p.row];
    c.textLabel.text = [save[@"name"] stringByDeletingPathExtension];
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    if (self.companionDirectory) {
        c.selectionStyle = UITableViewCellSelectionStyleDefault;
        c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    }
#endif
    NSArray *missing = save[@"missing"];
    c.detailTextLabel.text = [save[@"unreadable"] boolValue] ? T(@"Could not read this save", @"このセーブは読めませんでした") :
        missing.count ? [NSString stringWithFormat:T(@"⚠️ Missing: %@", @"⚠️ 足りないプラグイン：%@"), [missing componentsJoinedByString:@", "]] : T(@"✅ No missing plugins detected", @"✅ 足りないプラグインは見つかりませんでした");
    return c;
}
- (UISwipeActionsConfiguration *)tableView:(UITableView *)table trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)item {
    (void)table;
    if (saveCopyTargetID || ![self.sourceProfileID isEqual:activeProfile] || (NSUInteger)item.row >= self.saves.count) return nil;
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    if (self.checkingCompanion) return nil;
#endif
    NSString *name = self.saves[item.row][@"name"];
    UIContextualAction *copy = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal
        title:T(@"Copy to profile", @"別のプロファイルへコピー") handler:^(UIContextualAction *action, UIView *view, void (^done)(BOOL)) {
        (void)action; (void)view;
        if (saveCopyTargetID || self.navigationController.topViewController != self) { done(NO); return; }
        SKSaveCopyProfiles *targets = [[SKSaveCopyProfiles alloc] initWithStyle:UITableViewStyleInsetGrouped];
        targets.sourceID = self.sourceProfileID;
        targets.saveName = name;
        done(YES);
        [self.navigationController pushViewController:targets animated:YES];
    }];
    UISwipeActionsConfiguration *actions = [UISwipeActionsConfiguration configurationWithActions:@[copy]];
    actions.performsFirstActionWithFullSwipe = NO;
    return actions;
}
#ifdef SKYRIM_IOS_CLASSIC_SKSE
static NSString *CompanionResult(SkseInspectStatus status, const SkseInspectInfo *info) {
    switch (status) {
    case SKSE_INSPECT_OK:
        return [NSString stringWithFormat:T(@"Found a same-name SKSE Classic file. Sections %u · Records %u",
                                             @"同名のSKSE Classic形式のファイルを確認しました。セクション %u 件、レコード %u 件。"),
                info->owners, info->records];
    case SKSE_INSPECT_MISSING:
        return T(@"There is no same-name .skse file in this profile.", @"このプロファイルに同名の .skse ファイルがありません。");
    case SKSE_INSPECT_BASE_MISSING:
        return T(@"This save disappeared before it could be checked.", @"確認する前に元のセーブが見つからなくなりました。");
    case SKSE_INSPECT_AMBIGUOUS:
        return T(@"More than one file matches this name.", @"同じ名前に該当するファイルが複数あります。");
    case SKSE_INSPECT_UNSUPPORTED:
        return T(@"The .skse file uses a different format or game version.", @"この .skse ファイルの形式かゲームの版が対応範囲外です。");
    case SKSE_INSPECT_INVALID:
        return T(@"The .skse file is incomplete or malformed.", @"この .skse ファイルは記録が欠けているか形式が壊れています。");
    case SKSE_INSPECT_LIMIT:
        return T(@"The .skse file exceeds the inspection limit.", @"この .skse ファイルは確認できる容量か件数の上限を超えています。");
    case SKSE_INSPECT_UNSTABLE:
        return T(@"The save changed during the check. Try again.", @"確認中に保存内容が変わりました。もう一度お試しください。");
    case SKSE_INSPECT_IO:
    case SKSE_INSPECT_ARGUMENT:
        return T(@"The additional save could not be read.", @"追加セーブを読み取れませんでした。");
    }
    return T(@"The additional save could not be read.", @"追加セーブを読み取れませんでした。");
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (saveCopyTargetID || !self.companionDirectory || self.checkingCompanion || (NSUInteger)indexPath.row >= self.saves.count) return;
    self.checkingCompanion = YES;
    NSString *directory = self.companionDirectory;
    NSString *name = self.saves[indexPath.row][@"name"];
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    UIActivityIndicatorView *progress = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    cell.accessoryView = progress;
    [progress startAnimating];
    __weak SKSaves *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        SkseInspectInfo info = {0};
        SkseInspectStatus status = skse_inspect_companion(directory.fileSystemRepresentation, name.fileSystemRepresentation, &info);
        dispatch_async(dispatch_get_main_queue(), ^{
            SKSaves *view = weakSelf;
            view.checkingCompanion = NO;
            cell.accessoryView = nil;
            if (!view.viewIfLoaded.window || view.navigationController.topViewController != view || view.presentedViewController) return;
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:T(@"SKSE additional save", @"SKSE追加セーブ")
                message:CompanionResult(status, &info) preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
            [view presentViewController:alert animated:YES completion:nil];
        });
    });
}
#endif
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
#ifdef SKYRIM_IOS_SKSE_DLL_PROBE
@property(nonatomic, strong) NSURL *dllProbeFolder;
#endif
#ifdef SKYRIM_IOS_MOD_HOOKS
@property(nonatomic, copy) NSString *raceMenuPackage;
@property(nonatomic, copy) NSString *raceMenuProfile;
#endif
@end
@implementation SKListController
#ifdef SKYRIM_IOS_SKSE_DLL_PROBE
- (void)viewDidLoad {
    [super viewDidLoad];
    if (self.dllProbeFolder)
        self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:T(@"Check DLLs", @"DLLを診断")
            style:UIBarButtonItemStylePlain target:self action:@selector(checkInstalledDlls)];
}
- (void)checkInstalledDlls {
    BOOL japanese = atomic_load_explicit(&languageCache, memory_order_relaxed);
    UIViewController *check = skyrim_mod_dll_probe_controller(japanese, self.dllProbeFolder);
    [self.navigationController pushViewController:check animated:YES];
}
#endif
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s {
    (void)t; (void)s;
#ifdef SKYRIM_IOS_MOD_HOOKS
    if (self.raceMenuPackage) return (NSInteger)self.lines.count + 1;
#endif
    return (NSInteger)self.lines.count;
}
#ifdef SKYRIM_IOS_MOD_HOOKS
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)p {
    [t deselectRowAtIndexPath:p animated:YES];
    if (!self.raceMenuPackage || (NSUInteger)p.row != self.lines.count) return;
    NSMutableDictionary *profile = ProfileById(self.raceMenuProfile);
    NSDictionary *package = LoadPackage(self.raceMenuPackage);
    NSString *leaf = RaceMenuPresetLeaf(package);
    if (!leaf || ![EnabledPackageIds(profile) containsObject:self.raceMenuPackage]) {
        skyrim_mobile_show_notice(T(@"Enable this preset package in the profile first.",
                                    @"先にこのプロファイルでプリセットのMODを有効にしてください。"));
        return;
    }
    SkyrimRaceMenuPreset preset;
    if (!ReadRaceMenuPreset(package, &preset)) {
        skyrim_mobile_show_notice(T(@"This file is not a readable RaceMenu LE preset.",
                                    @"読み取れるRaceMenu LEプリセットではありません。"));
        return;
    }
    id previous = profile[@"pendingPreset"];
    profile[@"pendingPreset"] = @{@"request":NSUUID.UUID.UUIDString, @"package":self.raceMenuPackage,
                                   @"file":leaf, @"sha256":package[@"sha256"]};
    if (!SaveState()) {
        profile[@"pendingPreset"] = previous;
        skyrim_mobile_show_notice(T(@"Could not save the pending preset.", @"プリセットの適用設定を保存できませんでした。"));
        return;
    }
    skyrim_mobile_show_notice(T(@"After restarting, open character creation to apply only Nose Height.",
                                @"アプリ再起動後にキャラクター作成を開くと、鼻の高さだけを反映します。"));
}
#endif
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s { (void)t; (void)s; return self.footer; }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    (void)t;
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
#ifdef SKYRIM_IOS_MOD_HOOKS
    if (self.raceMenuPackage && (NSUInteger)p.row == self.lines.count) {
        c.textLabel.text = T(@"Apply Nose Height on next launch", @"次回起動時に鼻の高さを反映");
        c.textLabel.numberOfLines = 0;
        c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        return c;
    }
#endif
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
    c.textLabel.text = ProfileName(profile);
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
        skyrim_mobile_show_notice(T(@"Profile created. Select it and restart the app to save to this profile.",
            @"新しいプロファイルを作りました。選んでからアプリを起動し直すと、そのプロファイルにセーブできます。"));
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
    if (![profile[@"id"] isEqual:@"default"] && ![profile[@"id"] isEqual:activeProfile] &&
        ![profile[@"id"] isEqual:saveCopyTargetID]) {
        [actions addObject:[UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive title:T(@"Delete", @"削除")
            handler:^(UIContextualAction *a, UIView *v, void (^done)(BOOL)) {
            (void)a; (void)v;
            UIAlertController *c = [UIAlertController alertControllerWithTitle:T(@"Delete this profile and its saves?", @"このプロファイルとセーブを削除しますか？")
                message:T(@"Saves in this profile are deleted permanently. MOD files stay installed.", @"このプロファイルのセーブは完全に削除され、元に戻せません。MOD自体は残ります。") preferredStyle:UIAlertControllerStyleAlert];
            [c addAction:[UIAlertAction actionWithTitle:T(@"Cancel", @"キャンセル") style:UIAlertActionStyleCancel handler:nil]];
            [c addAction:[UIAlertAction actionWithTitle:T(@"Delete", @"削除") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
                (void)x;
                if ([profile[@"id"] isEqual:saveCopyTargetID]) {
                    skyrim_mobile_show_notice(T(@"This profile is receiving a save copy.", @"このプロファイルへセーブをコピー中です。"));
                    return;
                }
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
#ifdef SKYRIM_IOS_SKSE_DLL_PROBE
    if (n(@"skseDlls")) [bad addObject:[NSString stringWithFormat:T(@"%u SKSE DLLs can be checked in this MOD's details",
        @"このMODのSKSE用DLL %u個は詳細画面から診断できます"), n(@"skseDlls")]];
#else
    if (n(@"skseDlls")) [bad addObject:[NSString stringWithFormat:T(@"⛔ %u SKSE plugin DLL(s): cannot run on iPhone", @"⛔ SKSEプラグイン（DLL）%u個：iPhoneでは動きません"), n(@"skseDlls")]];
#endif
    if (n(@"seMeshes")) [warn addObject:[NSString stringWithFormat:T(@"⚠️ %u Special Edition mesh(es): may look broken or crash", @"⚠️ SE用のメッシュ %u個：表示崩れや落ちる可能性"), n(@"seMeshes")]];
    if (n(@"hkx64")) [warn addObject:[NSString stringWithFormat:T(@"⚠️ Incompatible 64-bit HKX detected (%u). Use Skyrim LE files.",
        @"⚠️ LE非対応の64ビットHKXが%u件あります。LE用のファイルを使ってください。"), n(@"hkx64")]];
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

#ifdef SKYRIM_IOS_CLASSIC_SKSE
/* First-wave MOD features found in the selected profile. Off by default; each
 * switch applies at the next launch. Detection is by file name only. */
static NSString *FeatureName(unsigned feature) {
    switch (feature) {
    case SKYRIM_FEATURE_FUZ: return T(@"Fuz Ro D-oh 6.1 (subtitles and lip sync for unvoiced lines)", @"Fuz Ro D-oh 6.1（無音声の台詞に字幕と口の動き）");
    case SKYRIM_FEATURE_PAPYRUSUTIL: return T(@"PapyrusUtil (JsonUtil settings)", @"PapyrusUtil（JsonUtilの設定）");
    case SKYRIM_FEATURE_PRECACHE_KILLER: return @"ShowRaceMenu Precache Killer 1.3";
    case SKYRIM_FEATURE_RACEMENU: return @"RaceMenu";
    case SKYRIM_FEATURE_NIOVERRIDE: return @"NetImmerse Override";
    case SKYRIM_FEATURE_ECE: return @"Enhanced Character Edit";
    case SKYRIM_FEATURE_ENB: return T(@"ENB preset (the ENB DLL itself is never loaded)", @"ENBプリセット（ENB本体のDLLは読み込みません）");
    case SKYRIM_FEATURE_ENHANCED_CAMERA: return @"Enhanced Camera";
    case SKYRIM_FEATURE_SAFETY_LOAD: return T(@"Safety Load 1.2 (loading screens, trial)", @"Safety Load 1.2（ロード中・試験）");
    case SKYRIM_FEATURE_HDT_PHYSICS: return @"HDT Physics Extensions";
    case SKYRIM_FEATURE_HDT_HIGHHEELS: return @"HDT High Heels";
    }
    return @"";
}

@interface SKFeatures : UITableViewController
@property(nonatomic) SkyrimFeatureScan scan;
@property(nonatomic, strong) NSArray<NSNumber *> *found;
@property(nonatomic, copy) NSString *nioverrideDetails;
@end
@implementation SKFeatures
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = T(@"MOD features", @"MODの機能");
    [self reload];
}
- (void)reload {
    EnsureState();
    NSMutableArray<NSString *> *labels = [NSMutableArray array];
    BOOL firstIsOriginal;
    NSArray<NSString *> *roots = DataRootsFor(EnabledPackageIds(SelectedProfile()), labels, &firstIsOriginal);
    SkyrimFeatureScan scan = FeatureScanRoots(roots, firstIsOriginal);
    self.scan = scan;
    self.nioverrideDetails = nil;
    SkyrimFeatureState nioverride = scan.features[SKYRIM_FEATURE_NIOVERRIDE];
    if (nioverride.winner >= 0 && (NSUInteger)nioverride.winner < roots.count) {
        NSUInteger winner = (NSUInteger)nioverride.winner;
        NSMutableString *details = [NSMutableString string];
        if (nioverride.providers > 1)
            [details appendFormat:T(@" Duplicate copies in %u sources.", @" %uか所に重複しています。"),
                                  nioverride.providers];
        [details appendFormat:scan.unreadable_roots
             ? T(@" Highest priority among checked sources: %@.", @" 確認できた中で最優先の提供元：%@。")
             : T(@" Selected source: %@.", @" 選ばれる提供元：%@。"), labels[winner]];
        uint16_t version[4];
        if (skyrim_nioverride_file_version(roots[winner].fileSystemRepresentation, version))
            [details appendFormat:T(@" DLL file version %u.%u.%u.%u.", @" DLLのファイルバージョン %u.%u.%u.%u。"),
                                  (unsigned)version[0], (unsigned)version[1], (unsigned)version[2], (unsigned)version[3]];
        else
            [details appendString:T(@" DLL file version unavailable.", @" DLLのファイルバージョンは不明です。")];
        self.nioverrideDetails = details;
    }
    NSMutableArray *found = [NSMutableArray array];
    for (unsigned f = 0; f < SKYRIM_FEATURE_COUNT; ++f) if (self.scan.features[f].providers) [found addObject:@(f)];
    self.found = found;
    [self.tableView reloadData];
}
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { (void)t; (void)s; return self.found.count ? (NSInteger)self.found.count : 1; }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s {
    (void)t; (void)s;
    return T(@"Detected by file name. File versions appear when available. Switches start off and apply at the next full restart. Enable the MOD's own plugin and files in this profile first.",
             @"ファイル名で検出し、確認できたファイルバージョンを表示します。スイッチは初期状態ではオフで、次回の完全な再起動から反映します。先にこのプロファイルでMODのプラグインとファイルを有効にしてください。");
}
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    (void)t;
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    c.textLabel.numberOfLines = 0; c.detailTextLabel.numberOfLines = 0; c.selectionStyle = UITableViewCellSelectionStyleNone;
    if (!self.found.count) {
        c.textLabel.text = T(@"No supported MOD features found in this profile", @"このプロファイルに対応する機能のMODは見つかりません");
        return c;
    }
    unsigned f = self.found[p.row].unsignedIntValue;
    c.textLabel.text = FeatureName(f);
    BOOL implemented = FeatureImplementedInApp(f);
    NSString *status = implemented ? T(@"Found by file name (version not checked).", @"ファイル名で検出しました（版は未確認）。")
                                   : T(@"Found. Not supported on iPhone yet.", @"検出しました。iPhoneではまだ対応していません。");
#ifdef SKYRIM_IOS_MOD_HOOKS
    /* The opened guest must carry the hook groups the feature needs. */
    const uint32_t needed = skyrim_mod_hook_requirements(f);
    if (implemented && (needed & ~pc_guest_mod_hooks_compiled())) {
        status = T(@"Found. The game build in this app lacks the needed hooks; rebuild the guest with them.",
                   @"検出しました。このアプリのゲーム部分に必要なフックがありません。ゲストの再ビルドが必要です。");
        implemented = NO;
    }
#endif
    unsigned providers = self.scan.features[f].providers;
    if (f == SKYRIM_FEATURE_NIOVERRIDE && self.nioverrideDetails)
        status = [status stringByAppendingString:self.nioverrideDetails];
    else if (providers > 1)
        status = [status stringByAppendingFormat:T(@" %u packages provide it; the later one in the list is used.", @" %u個のパッケージが含みます。一覧で後のものが使われます。"), providers];
    c.detailTextLabel.text = status;
    if (implemented) {
        UISwitch *toggle = [UISwitch new]; toggle.on = FeatureSelected(SelectedProfile(), f); toggle.tag = f;
        toggle.accessibilityLabel = FeatureName(f);
        toggle.accessibilityIdentifier = [@"mod-feature-" stringByAppendingString:@(skyrim_feature_key(f))];
        [toggle addTarget:self action:@selector(toggled:) forControlEvents:UIControlEventValueChanged];
        c.accessoryView = toggle;
    }
    return c;
}
- (void)toggled:(UISwitch *)toggle {
    NSMutableDictionary *profile = SelectedProfile();
    NSString *key = @(skyrim_feature_key((unsigned)toggle.tag) ?: "");
    NSDictionary *previous = [profile[@"features"] isKindOfClass:NSDictionary.class] ? profile[@"features"] : nil;
    NSMutableDictionary *next = [previous mutableCopy] ?: [NSMutableDictionary dictionary];
    if (toggle.on) next[key] = @YES; else [next removeObjectForKey:key];
    profile[@"features"] = next.count ? next : nil;
    if (!SaveState()) {
        profile[@"features"] = previous;
        toggle.on = !toggle.on;
        skyrim_mobile_show_notice(T(@"Could not save the setting.", @"設定を保存できませんでした。"));
        return;
    }
    skyrim_mobile_show_notice(T(@"Saved for the next launch.", @"次回起動から反映するよう保存しました。"));
}
@end
#endif

static NSURL *ModInboxURL(void) {
    NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask].firstObject;
    NSURL *inbox = [documents URLByAppendingPathComponent:@"MOD Inbox" isDirectory:YES];
    if (!inbox || ![NSFileManager.defaultManager createDirectoryAtURL:inbox
        withIntermediateDirectories:YES attributes:nil error:NULL]) return nil;
    struct stat st;
    return !lstat(inbox.fileSystemRepresentation, &st) && S_ISDIR(st.st_mode) ? inbox : nil;
}

static BOOL ModInboxArchive(NSURL *url) {
    NSString *ext = url.pathExtension.lowercaseString;
    if (![@[@"zip", @"rar", @"7z"] containsObject:ext]) return NO;
    struct stat st;
    return !lstat(url.fileSystemRepresentation, &st) && S_ISREG(st.st_mode);
}

@interface SKModInbox : UITableViewController
@property(nonatomic, copy) NSArray<NSURL *> *archives;
@end

@implementation SKModInbox
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"MOD Inbox";
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    NSURL *inbox = ModInboxURL();
    NSArray *entries = inbox ? [NSFileManager.defaultManager contentsOfDirectoryAtURL:inbox
        includingPropertiesForKeys:nil options:NSDirectoryEnumerationSkipsHiddenFiles error:NULL] : nil;
    NSMutableArray *archives = [NSMutableArray array];
    for (NSURL *url in entries) if (ModInboxArchive(url)) [archives addObject:url];
    [archives sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        return [a.lastPathComponent localizedStandardCompare:b.lastPathComponent];
    }];
    self.archives = archives;
    [self.tableView reloadData];
}
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section {
    (void)table; (void)section;
    return MAX((NSInteger)self.archives.count, 1);
}
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)path {
    (void)table;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.textLabel.numberOfLines = 0;
    if (self.archives.count) {
        cell.textLabel.text = self.archives[path.row].lastPathComponent;
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    } else {
        cell.textLabel.text = T(@"Copy a ZIP, RAR or 7z to Files → On My iPhone → SkyrimIOS → MOD Inbox, then reopen this list.",
                               @"「ファイル」→「このiPhone内」→「SkyrimIOS」→「MOD Inbox」にZIP・RAR・7zをコピーし、この一覧を開き直してください。");
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    }
    return cell;
}
- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)path {
    [table deselectRowAtIndexPath:path animated:YES];
    if (!self.archives.count) return;
    NSURL *url = self.archives[path.row];
    [self.navigationController popViewControllerAnimated:YES];
    if (ModInboxArchive(url)) skyrim_mods_import_url(url, YES);
    else Notice(T(@"The selected archive is no longer in MOD Inbox.", @"選んだファイルがMOD Inboxから移動または削除されました。"));
}
@end

@interface SKModHub : UITableViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) NSMutableArray<NSMutableDictionary *> *rows;   /* selected choices and available packages */
@property(nonatomic, strong) NSDictionary<NSString *, NSDictionary *> *packages;
@property(nonatomic, strong) NSDictionary<NSString *, NSString *> *summaries;   /* computed once per reload */
@property(nonatomic) BOOL pickingGenerated;
@end
enum { HubProfile, HubMods, HubAdd, HubTools, HubOptimize, HubSafety, HubShare, HubCount };
enum {
    HubOrderRow, HubConflictRow, HubSavesRow,
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    HubSkyUITrialRow, HubFeaturesRow,
#endif
#ifdef SKYRIM_IOS_MOD_HOOKS
    HubInventoryFitRow,
#endif
#ifdef SKYRIM_IOS_SKSE_DLL_PROBE
    HubDllProbeRow,
#endif
    HubToolCount
};
@implementation SKModHub
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = @"MOD";
    (void)ModInboxURL();
    self.tableView.estimatedRowHeight = 60; self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:T(@"Reorder", @"並べ替え") style:UIBarButtonItemStylePlain target:self action:@selector(toggleReorder)];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(modsChanged:) name:@"SKModsChanged" object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(importNoticeChanged:) name:ImportNoticeChanged object:nil];
    [self reload];
}
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)modsChanged:(NSNotification *)notification {
    (void)notification;
    [self reload];
}
- (void)updateImportPrompt {
    self.navigationItem.prompt = [lastImportNotice componentsSeparatedByString:@"\n"].firstObject;
}
- (void)importNoticeChanged:(NSNotification *)notification {
    (void)notification;
    [self updateImportPrompt];
    if (self.tableView.window && !self.tableView.hasUncommittedUpdates)
        [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:HubAdd] withRowAnimation:UITableViewRowAnimationNone];
}
- (void)reload {
    EnsureState();
    self.rows = [SelectedProfile()[@"packages"] mutableCopy];
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
    /* Available packages are displayed off before the user chooses them. */
    NSDictionary *profile = SelectedProfile();
    for (NSString *pid in byId)
        if (!ProfileHasPackage(profile, pid)) [self.rows addObject:[@{@"id":pid, @"on":@NO} mutableCopy]];
    [self.tableView reloadData];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    RefreshLanguage();
    [self updateReorderTitle];
    [self updateImportPrompt];
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
    case HubAdd: return 5;
    case HubTools: return HubToolCount;
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
    if (s == HubAdd) {
        NSString *help = T(@"If file selection fails, copy the archive to MOD Inbox in the Files app and add it from there.",
                           @"ファイルを選べない場合は、「ファイル」アプリで圧縮ファイルをMOD Inboxにコピーし、ここから追加してください。");
        return lastImportNotice.length ? [NSString stringWithFormat:@"%@\n\n%@", lastImportNotice, help] : help;
    }
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
        NSArray *titles = @[T(@"Add an archive or character preset", @"圧縮ファイル・キャラクタープリセットを追加"), T(@"Add a folder", @"フォルダを選んで追加"), T(@"Find on Nexus Mods", @"Nexus ModsでMODを探す"),
            T(@"Add BodySlide or FNIS output folder", @"BodySlide・FNISの出力フォルダを追加"), T(@"Add from MOD Inbox", @"MOD Inboxから追加")];
        NSArray *icons = @[@"doc.zipper", @"folder", @"safari", @"folder.badge.plus", @"tray.and.arrow.down"];
        c.textLabel.text = titles[p.row]; c.imageView.image = [UIImage systemImageNamed:icons[p.row]];
        break;
    }
    case HubTools:
#ifdef SKYRIM_IOS_SKSE_DLL_PROBE
        if (p.row == HubDllProbeRow) {
            c.textLabel.text = T(@"DLL Compatibility", @"DLL互換性診断");
            c.imageView.image = [UIImage systemImageNamed:@"doc.text.magnifyingglass"];
            c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            break;
        }
#endif
#ifdef SKYRIM_IOS_CLASSIC_SKSE
        if (p.row == HubSkyUITrialRow) {
            c.textLabel.text = T(@"SkyUI 5.1 trial", @"SkyUI 5.1 の試験");
            c.detailTextLabel.text = T(@"Enable SkyUI.esp in this profile, then restart.", @"このプロファイルでSkyUI.espを有効にし、アプリを起動し直してください。");
            NSString *missing = SkyUIRequirementText(SkyUIRequirementsFor(EnabledPackageIds(SelectedProfile())));
            if (missing) c.detailTextLabel.text = missing;
            UISwitch *toggle = [UISwitch new]; toggle.on = SkyUITrialSelected(SelectedProfile());
            toggle.accessibilityLabel = c.textLabel.text;
            toggle.accessibilityIdentifier = @"mod-skyui-trial";
            [toggle addTarget:self action:@selector(skyuiTrialToggled:) forControlEvents:UIControlEventValueChanged];
            c.accessoryView = toggle; c.selectionStyle = UITableViewCellSelectionStyleNone;
            break;
        }
#endif
#ifdef SKYRIM_IOS_MOD_HOOKS
        if (p.row == HubInventoryFitRow) {
            NSDictionary *profile = SelectedProfile();
            c.textLabel.text = T(@"Fit menus to wide screens", @"横長画面にメニューを収める");
            const BOOL supported = (pc_guest_mod_hooks_compiled() & PC_GUEST_MOD_HOOK_INVENTORY_FIT) != 0;
            c.detailTextLabel.text = supported
                ? T(@"Keeps the edges of inventory, magic and similar menus visible. Applies after restart.",
                    @"持ち物や魔法などのメニューを上下が切れないように表示します。再起動後に反映します。")
                : T(@"Requires a compatible guest build.", @"対応するゲーム実行部分が必要です。");
            UISwitch *toggle = [UISwitch new];
            toggle.enabled = supported;
            toggle.on = ![profile[@"inventoryFit"] isEqual:@NO];
            toggle.accessibilityLabel = c.textLabel.text;
            toggle.accessibilityIdentifier = @"mod-skyui-inventory-fit";
            [toggle addTarget:self action:@selector(inventoryFitToggled:) forControlEvents:UIControlEventValueChanged];
            c.accessoryView = toggle; c.selectionStyle = UITableViewCellSelectionStyleNone;
            break;
        }
#endif
#ifdef SKYRIM_IOS_CLASSIC_SKSE
        if (p.row == HubFeaturesRow) {
            c.textLabel.text = T(@"MOD features", @"MODの機能");
            c.imageView.image = [UIImage systemImageNamed:@"slider.horizontal.3"];
            c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
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
- (void)rememberPackageOrderIncluding:(NSDictionary *)choice {
    NSMutableDictionary *profile = SelectedProfile();
    NSMutableSet *ids = [NSMutableSet setWithObject:choice[@"id"]];
    for (NSDictionary *configured in profile[@"packages"]) [ids addObject:configured[@"id"]];
    NSMutableArray *order = [NSMutableArray arrayWithCapacity:ids.count];
    for (NSDictionary *row in self.rows)
        if ([ids containsObject:row[@"id"]]) [order addObject:row];
    profile[@"packages"] = order;
}
- (void)toggled:(UISwitch *)toggle {
    if ((NSUInteger)toggle.tag >= self.rows.count) return;
    NSMutableDictionary *row = self.rows[toggle.tag];
    row[@"on"] = @(toggle.on);
    [self rememberPackageOrderIncluding:row];
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
static BOOL SaveProfileToggle(UISwitch *toggle, NSString *key) {
    NSMutableDictionary *profile = SelectedProfile();
    const BOOL previous = [profile[key] isEqual:@YES];
    profile[key] = toggle.on ? @YES : nil;
    if (SaveState()) return YES;
    profile[key] = previous ? @YES : nil;
    toggle.on = previous;
    return NO;
}
- (void)skyuiTrialToggled:(UISwitch *)toggle {
    if (!SaveProfileToggle(toggle, @"skyuiTrial")) {
        skyrim_mobile_show_notice(T(@"Could not save the SkyUI trial setting.", @"SkyUI試験の設定を保存できませんでした。"));
        return;
    }
    [self.tableView reloadData];
    skyrim_mobile_show_notice(T(@"SkyUI trial setting saved for the next launch.", @"SkyUI試験の設定を保存しました。次回起動から反映します。"));
}
#ifdef SKYRIM_IOS_MOD_HOOKS
- (void)inventoryFitToggled:(UISwitch *)toggle {
    if (SaveProfileToggle(toggle, @"inventoryFit"))
        skyrim_mobile_show_notice(T(@"Menu fit setting saved for the next launch.", @"メニューの表示設定を保存しました。次回起動から反映します。"));
    else
        skyrim_mobile_show_notice(T(@"Could not save the menu fit setting.", @"メニューの表示設定を保存できませんでした。"));
}
#endif
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
    [self rememberPackageOrderIncluding:row];
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
- (void)pick:(NSArray<UTType *> *)types single:(BOOL)single generated:(BOOL)generated {
    self.pickingGenerated = generated;
    BOOL copyFile = ![types containsObject:UTTypeFolder];
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:types asCopy:copyFile];
    picker.delegate = self; picker.allowsMultipleSelection = !single;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)picker didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)picker;
    for (NSURL *url in urls) ImportURL(url, YES, self.pickingGenerated);
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
        if (pkg[@"generatedFor"]) {
            NSDictionary *profile = SelectedProfile();
            NSString *warning = GeneratedOutputWarning(pkg, GeneratedInputFingerprint(profile, EnabledPackageIds(profile)));
            [lines addObject:warning ?: T(@"Added as generated output for this MOD setup.",
                                         @"このMOD構成の生成済み出力として追加されました。")];
        }
#ifdef SKYRIM_IOS_CLASSIC_SKSE
        for (NSString *plugin in pkg[@"plugins"])
            if ([plugin caseInsensitiveCompare:@"XPMSE.esp"] == NSOrderedSame) {
                NSDictionary *profile = SelectedProfile();
                [lines addObjectsFromArray:XpmseRequirementLines(EnabledPackageIds(profile), EffectivePlugins(profile))];
                break;
            }
#endif
        detail.lines = lines; detail.title = pkg[@"name"];
#ifdef SKYRIM_IOS_MOD_HOOKS
        if ((pc_guest_mod_hooks_compiled() & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) && RaceMenuPresetLeaf(pkg)) {
            detail.raceMenuPackage = pkg[@"id"];
            detail.raceMenuProfile = SelectedProfile()[@"id"];
        }
#endif
#ifdef SKYRIM_IOS_SKSE_DLL_PROBE
        NSDictionary *analysis = pkg[@"report"];
        if ([analysis isKindOfClass:NSDictionary.class] && [analysis[@"skseDlls"] unsignedIntValue]) {
            NSString *directory = PackageDir(pkg[@"id"]);
            if (directory) detail.dllProbeFolder = [NSURL fileURLWithPath:directory isDirectory:YES];
        }
#endif
        [self.navigationController pushViewController:detail animated:YES];
        break;
    }
    case HubAdd:
        if (p.row == 0) {
            NSMutableArray *types = [NSMutableArray arrayWithObject:UTTypeZIP];
            for (NSString *ext in @[@"7z", @"rar", @"jslot", @"slot"]) { UTType *u = [UTType typeWithFilenameExtension:ext]; if (u) [types addObject:u]; }
            [self pick:types single:NO generated:NO];
        } else if (p.row == 1) [self pick:@[UTTypeFolder] single:YES generated:NO];
        else if (p.row == 2) [self.navigationController pushViewController:skyrim_mods_browser_controller() animated:YES];
        else if (p.row == 3) [self pick:@[UTTypeFolder] single:YES generated:YES];
        else if (ModInboxURL()) [self.navigationController pushViewController:[[SKModInbox alloc]
            initWithStyle:UITableViewStyleInsetGrouped] animated:YES];
        else Notice(T(@"Could not create MOD Inbox.", @"MOD Inboxを作成できませんでした。"));
        break;
    case HubTools:
#ifdef SKYRIM_IOS_SKSE_DLL_PROBE
        if (p.row == HubDllProbeRow) {
            BOOL japanese = atomic_load_explicit(&languageCache, memory_order_relaxed);
            [self.navigationController pushViewController:skyrim_mod_dll_probe_controller(japanese, nil) animated:YES];
            break;
        }
#endif
#ifdef SKYRIM_IOS_MOD_HOOKS
        if (p.row == HubInventoryFitRow) break;
#endif
#ifdef SKYRIM_IOS_CLASSIC_SKSE
        if (p.row == HubSkyUITrialRow) break;
        if (p.row == HubFeaturesRow) {
            [self.navigationController pushViewController:[[SKFeatures alloc] initWithStyle:UITableViewStyleInsetGrouped] animated:YES];
            break;
        }
#endif
        if (p.row == HubOrderRow) [self.navigationController pushViewController:skyrim_mobile_plugin_controller() animated:YES];
        else if (p.row == HubConflictRow) [self.navigationController pushViewController:[[SKConflicts alloc] initWithStyle:UITableViewStyleInsetGrouped] animated:YES];
        else if (p.row == HubSavesRow) [self.navigationController pushViewController:[[SKSaves alloc] initWithStyle:UITableViewStyleInsetGrouped] animated:YES];
        break;
    case HubOptimize:
        if (p.row == 1) [self chooseLimit:p];
        break;
    case HubShare:
        if (p.row) [self pick:@[UTTypeJSON] single:YES generated:NO];
        else [self share:ExportModList(SelectedProfile()) from:p];
        break;
    }
}
@end

UIViewController *skyrim_mods_hub_controller(void) {
    EnsureState();
    return [[SKModHub alloc] initWithStyle:UITableViewStyleInsetGrouped];
}
