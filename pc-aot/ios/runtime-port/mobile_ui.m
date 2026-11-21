/* SPDX-License-Identifier: GPL-3.0-only
 * Native controls are outside the D3D9 render pass. No game-memory hooks,
 * guest addresses, process snapshots or original installation edits. */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <dispatch/dispatch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include "mobile_ui.h"
#include "mobile_core.h"
#include "autosave_preferences.h"
#include "ios_runtime.h"
#include "performance_metrics.h"
#include "lock_metrics.h"
#include "pipeline_cache_control.h"
#include "device_policy.h"
#include "game_language.h"
#include "mod_plugins.h"
#include "mod_manager.h"
#include "diagnostic_upload.h"
#include "diagnostic_schema.h"
#include "lifecycle_runtime.h"

static NSMutableDictionary *settings;
static NSDictionary *startup;
static NSString *profilePath;
static __weak UIWindow *hostWindow;
static uint64_t frameCount;
static UIBackgroundTaskIdentifier cacheBackgroundTask;
static uint64_t cacheCheckpointTicket;
static BOOL cacheCheckpointQueued;
static dispatch_queue_t cacheCheckpointQueue;
static BOOL shaderSettingsVisible, cacheForegroundQueued;
static SkyrimGameLanguagePacks gameLanguagePacks;
static SkyrimGameLanguage gameLanguage;
static BOOL gameLanguagePrepared;

// kind: 0 background, 1 settings; phase: 0 entered, 1 save returned.
// Log on the worker before save, not only in the eventual main-queue callback:
// an app exit/suspension can otherwise hide which boundary was reached.
static void logCacheWorker(uint64_t ticket, unsigned kind, unsigned phase,
                           uint64_t queued, uint64_t entered) {
    const int savedErrno = errno;
    const uint64_t now = skyrim_perf_now_ns();
    fprintf(stderr, "IOS_PIPE_CACHE event=worker ticket=%llu kind=%u phase=%u queue_wait_ns=%llu elapsed_ns=%llu monotonic_ns=%llu\n",
        (unsigned long long)ticket, kind, phase,
        (unsigned long long)(queued && entered >= queued ? entered - queued : 0),
        (unsigned long long)(entered && now >= entered ? now - entered : 0),
        (unsigned long long)now);
    errno = savedErrno;
}

static void shaderSettingsPermission(BOOL visible) {
    shaderSettingsVisible = visible;
    skyrim_pipeline_checkpoint_foreground_permission(visible &&
        skyrim_mobile_shader_option(SKYRIM_SHADER_SAVE_IN_SETTINGS) &&
        UIApplication.sharedApplication.applicationState == UIApplicationStateActive);
}

static void checkpointWhileSettingsVisible(void) {
    if (!shaderSettingsVisible || cacheForegroundQueued ||
        !skyrim_mobile_shader_option(SKYRIM_SHADER_SAVE_IN_SETTINGS) ||
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    cacheForegroundQueued = YES;
    const uint64_t queued = skyrim_perf_now_ns();
    dispatch_async(cacheCheckpointQueue, ^{
        const uint64_t entered = skyrim_perf_now_ns();
        // Eligibility and registry admission run on this worker, never UIKit.
        uint64_t ticket = skyrim_pipeline_checkpoint_foreground_begin();
        if (ticket) {
            logCacheWorker(ticket, 1, 0, queued, entered);
            skyrim_pipeline_checkpoint_save(ticket);
            logCacheWorker(ticket, 1, 1, queued, entered);
        }
        dispatch_async(dispatch_get_main_queue(), ^{ cacheForegroundQueued = NO; });
    });
}

// Finish source: 0 completed, 1 resumed, 2 background allowance expired.
static void finishCacheCheckpoint(uint64_t ticket, unsigned source) {
    const BOOL cancel = source != 0;
    fprintf(stderr, "IOS_PIPE_CACHE event=finish ticket=%llu source=%u current=%u task_valid=%u\n",
        (unsigned long long)ticket, source, (unsigned)(ticket == cacheCheckpointTicket),
        (unsigned)(cacheBackgroundTask != UIBackgroundTaskInvalid));
    if (cancel) skyrim_pipeline_checkpoint_cancel(ticket);
    if (ticket != cacheCheckpointTicket) return;
    UIBackgroundTaskIdentifier task = cacheBackgroundTask;
    cacheBackgroundTask = UIBackgroundTaskInvalid;
    cacheCheckpointQueued = NO;
    if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task];
}

static void preparePipelineCacheCheckpoint(void) {
    if (cacheBackgroundTask != UIBackgroundTaskInvalid) {
        fprintf(stderr, "IOS_PIPE_CACHE event=prepare ticket=%llu task_valid=1 reused=1\n", (unsigned long long)cacheCheckpointTicket);
        return;
    }
    const uint64_t ticket = skyrim_pipeline_checkpoint_begin();
    cacheCheckpointTicket = ticket;
    cacheBackgroundTask = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Pipeline cache" expirationHandler:^{
        finishCacheCheckpoint(ticket, 2);
    }];
    fprintf(stderr, "IOS_PIPE_CACHE event=prepare ticket=%llu task_valid=%u reused=0\n",
        (unsigned long long)ticket, (unsigned)(cacheBackgroundTask != UIBackgroundTaskInvalid));
    if (cacheBackgroundTask == UIBackgroundTaskInvalid) {
        skyrim_pipeline_checkpoint_cancel(ticket); return;
    }
}

static void checkpointPipelineCache(void) {
    unsigned reason = cacheBackgroundTask == UIBackgroundTaskInvalid ? 1 : cacheCheckpointQueued ? 2 : 0;
    fprintf(stderr, "IOS_PIPE_CACHE event=queue ticket=%llu reason=%u\n", (unsigned long long)cacheCheckpointTicket, reason);
    if (reason) return;
    cacheCheckpointQueued = YES;
    const uint64_t ticket = cacheCheckpointTicket;
    const uint64_t queued = skyrim_perf_now_ns();
    dispatch_async(cacheCheckpointQueue, ^{
        const uint64_t entered = skyrim_perf_now_ns();
        logCacheWorker(ticket, 0, 0, queued, entered);
        skyrim_pipeline_checkpoint_save(ticket);
        logCacheWorker(ticket, 0, 1, queued, entered);
        dispatch_async(dispatch_get_main_queue(), ^{ finishCacheCheckpoint(ticket, 0); });
    });
}
static int renderWidth, renderHeight;
static unsigned iniCount;
static NSTimeInterval mobileTime(void) { return (double)skyrim_mobile_now()/1e9; }
static struct { const char *section, *key; char value[48]; } ini[40];
static double value(NSString *key, double fallback) {
    id number = settings[key];
    return [number isKindOfClass:NSNumber.class] && isfinite([number doubleValue]) ? [number doubleValue] : fallback;
}
static void logDiagnosticSettings(void) {
    /* Restart-only values describe this running session, not pending changes.
     * The language result record separately reports the actual pack fallback. */
    const char *keys[] = {"fps", "thermal", "language", "verbose", "hud", "resolution",
        "texture", "anisotropy", "fxaa", "msaa", "shadow", "shadowDistance", "grass",
        "objects", "water", "decals", "autosave", "game_text", "game_voices"};
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        NSString *key = [NSString stringWithUTF8String:keys[i]];
        BOOL live = i <= SKYRIM_DIAG_SETTING_LANGUAGE || i == SKYRIM_DIAG_SETTING_HUD;
        double fallback = (i >= SKYRIM_DIAG_SETTING_TEXTURE && i <= SKYRIM_DIAG_SETTING_AUTOSAVE) ? -1 :
            i == SKYRIM_DIAG_SETTING_HUD ? 1 : 0;
        NSNumber *n = (live ? settings : startup)[key];
        double v = n ? n.doubleValue : fallback;
        if (isfinite(v) && fabs(v) <= 1e9)
            fprintf(stderr, "IOS_DIAG_SETTING key=%u value=%.6f\n", i, v);
    }
}
/* UI language is explicit and independent of device/game language. Missing or
 * invalid values use English, including upgrades from Japanese-only builds. */
static NSString *L(NSString *english, NSString *japanese) {
    return value(@"language", 0) == 1 ? japanese : english;
}
static double clamp(double x, double lo, double hi) { return fmin(hi, fmax(lo, x)); }
static void persist(void) {
    [NSUserDefaults.standardUserDefaults setObject:settings forKey:@"mobile-settings-v1"];
    skyrim_mobile_set_limit((unsigned)value(@"fps", 0), value(@"thermal", 0) != 0);
}
static void addINI(const char *section, const char *key, double v) {
    if (iniCount >= sizeof ini / sizeof ini[0]) abort();
    ini[iniCount].section = section; ini[iniCount].key = key;
    snprintf(ini[iniCount].value, sizeof ini[iniCount].value, "%.4f", v);
    ++iniCount;
}
static void integerINI(const char *section, const char *key, double v) {
    addINI(section, key, v);
    snprintf(ini[iniCount - 1].value, sizeof ini[0].value, "%d", (int)v);
}
/* The explicit selection whitelist is shared with the settings screen. */
static NSArray<NSDictionary *> *graphicsRows(void) {
    return @[
      @{@"key":@"resolution", @"title":L(@"Render height", @"描画の高さ"), @"values":@[@0,@360,@540,@720,@900,@1080,@2160], @"labels":@[L(@"Keep current", @"維持"),@"360p",@"540p",@"720p",@"900p",@"1080p",L(@"Native pixels", @"画面の実ピクセル数")]},
      @{@"key":@"texture", @"title":L(@"Textures", @"テクスチャ"), @"values":@[@-1,@2,@1,@0], @"labels":@[L(@"Keep current", @"維持"),L(@"Low", @"低"),L(@"Medium", @"中"),L(@"Original resolution", @"元の解像度")]},
      @{@"key":@"anisotropy", @"title":L(@"Anisotropic filtering", @"異方性フィルター"), @"values":@[@-1,@1,@2,@4,@8,@16]},
      @{@"key":@"fxaa", @"title":@"FXAA", @"values":@[@-1,@0,@1], @"labels":@[L(@"Keep current", @"維持"),L(@"Off", @"オフ"),L(@"On", @"オン")]},
      @{@"key":@"msaa", @"title":@"MSAA", @"values":@[@-1,@0,@2,@4,@8], @"labels":@[L(@"Keep current", @"維持"),L(@"Off", @"オフ"),@"2×",@"4×",@"8×"]},
      @{@"key":@"shadow", @"title":L(@"Shadow resolution", @"影の解像度"), @"values":@[@-1,@512,@1024,@2048,@4096]},
      @{@"key":@"shadowDistance", @"title":L(@"Shadow distance", @"影の距離"), @"values":@[@-1,@1500,@2000,@3000,@6000,@8000]},
      @{@"key":@"grass", @"title":L(@"Grass distance", @"草の表示距離"), @"values":@[@-1,@0,@1000,@3000,@6000,@9000]},
      @{@"key":@"objects", @"title":L(@"Object distance multiplier", @"物体の表示距離倍率"), @"values":@[@-1,@2.5,@5,@8,@12,@15]},
      @{@"key":@"water", @"title":L(@"Landscape reflections on water", @"水面への風景反射"), @"values":@[@-1,@0,@1], @"labels":@[L(@"Keep current", @"維持"),L(@"Off", @"省略"),L(@"On", @"表示")]},
      @{@"key":@"decals", @"title":L(@"Decals", @"血痕・着弾跡"), @"values":@[@-1,@0,@1], @"labels":@[L(@"Keep current", @"維持"),L(@"Off", @"オフ"),L(@"On", @"オン")]}
    ];
}
static NSArray<NSDictionary *> *shaderRows(void) {
    NSArray *titles = @[L(@"Parallel shader preparation", @"シェーダー準備を並列化"),
        L(@"Prioritize gameplay preparation", @"プレイ中に必要な準備を優先"),
        L(@"Save cache while settings are open", @"設定画面でキャッシュを保存"),
        L(@"Keep shader cache in app storage", @"シェーダーキャッシュをアプリ内に保持"),
        L(@"Reuse cache after compatible updates", @"互換性のある更新後もキャッシュを再利用"),
        L(@"Separate waits for drawing preparation", @"描画の準備待ちを分ける")];
    NSMutableArray *rows = [NSMutableArray arrayWithCapacity:SKYRIM_SHADER_OPTION_COUNT];
    for (unsigned i = 0; i < SKYRIM_SHADER_OPTION_COUNT; ++i)
        [rows addObject:@{@"key":[NSString stringWithUTF8String:skyrim_shader_option_key(i)],
            @"title":titles[i], @"values":@[@0,@1], @"default":@0,
            @"labels":@[L(@"Off", @"オフ"),L(@"On", @"オン")]}];
    return rows;
}
void skyrim_mobile_initialize(const char *profile) {
    NSCAssert(NSThread.isMainThread, @"mobile startup must run on main");
    /* UIKit exports this value; it is not a C constant initializer. Set it on
     * the main thread before creating the overlay and its lifecycle observers. */
    static dispatch_once_t cacheInitialization;
    dispatch_once(&cacheInitialization, ^{
        cacheBackgroundTask = UIBackgroundTaskInvalid;
        cacheCheckpointQueue = dispatch_queue_create("SkyrimIOS.shader-cache",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    });
    profilePath = [NSString stringWithUTF8String:profile];
    id saved = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"mobile-settings-v1"];
    settings = [NSMutableDictionary dictionary];
    if ([saved isKindOfClass:NSDictionary.class])
        for (NSString *key in saved)
            if ([key isKindOfClass:NSString.class] && [saved[key] isKindOfClass:NSNumber.class] && isfinite([saved[key] doubleValue])) settings[key] = saved[key];
    for (NSDictionary *row in graphicsRows()) {
        NSNumber *choice = settings[row[@"key"]];
        if (choice && ![row[@"values"] containsObject:choice]) [settings removeObjectForKey:row[@"key"]];
    }
    for (NSDictionary *row in shaderRows()) {
        NSNumber *choice = settings[row[@"key"]];
        if (choice && ![row[@"values"] containsObject:choice]) [settings removeObjectForKey:row[@"key"]];
    }
    if (![@[@0,@24,@30,@40,@60] containsObject:@(value(@"fps",0))]) [settings removeObjectForKey:@"fps"];
    if (![@[@0,@1] containsObject:@(value(@"language",0))]) [settings removeObjectForKey:@"language"];
    if (![@[@0,@1] containsObject:@(value(@"game_text",0))]) [settings removeObjectForKey:@"game_text"];
    if (![@[@0,@1] containsObject:@(value(@"game_voices",0))]) [settings removeObjectForKey:@"game_voices"];
    if (![@[@-1,@0,@1] containsObject:@(value(@"autosave",-1))]) [settings removeObjectForKey:@"autosave"];
    persist(); startup = [settings copy];
    logDiagnosticSettings();
    iniCount = 0;
    const char *section, *key, *setting;
    const double autosave = startup[@"autosave"] ? [startup[@"autosave"] doubleValue] : -1;
    for (unsigned i = 0; skyrim_autosave_ini(autosave, i, &section, &key, &setting); ++i)
        integerINI(section, key, setting[0] == '1');
    struct { const char *option, *section, *key; int integer; } map[] = {
        {"texture","Display","iTexMipMapSkip",1}, {"anisotropy","Display","iMaxAnisotropy",1},
        {"fxaa","Display","bFXAAEnabled",1}, {"msaa","Display","iMultiSample",1},
        {"shadow","Display","iShadowMapResolution",1}, {"shadowDistance","Display","fShadowDistance",0},
        {"grass","Grass","fGrassStartFadeDistance",0}, {"objects","LOD","fLODFadeOutMultObjects",0},
        {"objects","LOD","fLODFadeOutMultActors",0}, {"objects","LOD","fLODFadeOutMultItems",0},
        {"water","Water","bReflectLODLand",1}, {"water","Water","bReflectLODObjects",1},
        {"water","Water","bReflectLODTrees",1}, {"decals","Decals","bDecals",1},
        {"decals","Decals","bSkinnedDecals",1}
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; ++i) {
        NSNumber *v = startup[[NSString stringWithUTF8String:map[i].option]];
        if (!v || v.doubleValue < 0) continue;
        if (map[i].integer) integerINI(map[i].section,map[i].key,v.doubleValue);
        else addINI(map[i].section,map[i].key,v.doubleValue);
    }
    if (startup[@"texture"] && [startup[@"texture"] intValue] >= 0)
        integerINI("Display", "iTexMipMapMinimum", 0);
    if (startup[@"decals"] && [startup[@"decals"] intValue] >= 0) {
        int enabled = [startup[@"decals"] intValue] != 0;
        integerINI("Display","iMaxDecalsPerFrame",enabled ? 30 : 0);
        integerINI("Display","iMaxSkinDecalsPerFrame",enabled ? 10 : 0);
        integerINI("Decals","uMaxDecals",enabled ? 100 : 0);
        integerINI("Decals","uMaxSkinDecals",enabled ? 50 : 0);
        integerINI("Decals","uMaxSkinDecalPerActor",enabled ? 20 : 0);
    }
}
static BOOL applyGameLanguageProfile(const char *gameDirectory) {
    NSString *folder = [profilePath stringByAppendingPathComponent:@"Documents/My Games/Skyrim"];
    char profileFile[PATH_MAX], defaultFile[PATH_MAX];
    SkyrimGameIniSource origin = skyrim_game_language_plan_profile(gameDirectory,
        folder.fileSystemRepresentation, &gameLanguage, profileFile, defaultFile, sizeof profileFile);
    if (origin == SKYRIM_PROFILE_INI_ERROR) return NO;
    if (origin == SKYRIM_PROFILE_INI_DEFERRED) return YES;
    BOOL exists = origin == SKYRIM_PROFILE_INI_EXISTING;
    NSString *target = [NSString stringWithUTF8String:profileFile];
    NSString *sourcePath = exists ? target : [NSString stringWithUTF8String:defaultFile];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSError *error = nil;
    NSDictionary *attributes = [fm attributesOfItemAtPath:sourcePath error:&error];
    if (!attributes || ![attributes[NSFileType] isEqual:NSFileTypeRegular] ||
        [attributes[NSFileSize] unsignedLongLongValue] > 1024 * 1024) return NO;
    NSData *source = [NSData dataWithContentsOfFile:sourcePath options:0 error:&error];
    if (!source) return NO;
    size_t count = 0;
    char *bytes = skyrim_game_language_ini(source.length ? source.bytes : "", source.length, gameLanguage, &count);
    if (!bytes) return NO;
    NSData *updated = [NSData dataWithBytesNoCopy:bytes length:count freeWhenDone:YES];
    if ((!exists || ![source isEqualToData:updated]) &&
        (![fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:&error] ||
         ![updated writeToFile:target options:NSDataWritingAtomic error:&error])) return NO;
    return YES;
}
int skyrim_mobile_prepare_game_language(const char *gameDirectory) {
    NSCAssert(NSThread.isMainThread, @"game language is selected before guest startup");
    skyrim_game_language_probe(gameDirectory, &gameLanguagePacks);
    gameLanguage = skyrim_game_language_select([startup[@"game_text"] unsignedIntValue],
        [startup[@"game_voices"] unsignedIntValue], gameLanguagePacks);
    // Choosing only the app's display language leaves the game's profile untouched.
    BOOL explicitSelection = startup[@"game_text"] != nil || startup[@"game_voices"] != nil;
    gameLanguagePrepared = !explicitSelection || applyGameLanguageProfile(gameDirectory);
    fprintf(stderr, "IOS_GAME_LANGUAGE requested_text=%u text=%u text_reason=%u requested_voices=%u voices=%u voices_reason=%u\n",
        gameLanguage.requested_text, gameLanguage.text, gameLanguage.text_reason,
        gameLanguage.requested_voices, gameLanguage.voices, gameLanguage.voices_reason);
    return gameLanguagePrepared;
}
#ifdef SKYRIM_IOS_CLASSIC_SKSE
const char *skyrim_mobile_classic_text_language(void) {
    if (!gameLanguagePrepared || (!startup[@"game_text"] && !startup[@"game_voices"])) return NULL;
    return gameLanguage.text == SKYRIM_GAME_JAPANESE ? "JAPANESE" : "ENGLISH";
}
#endif

static NSString *gameLanguageFallback(void) {
    if (!gameLanguagePrepared) return nil;
    if (gameLanguage.text_reason == SKYRIM_GAME_MISSING_DEFAULT_INI ||
        gameLanguage.voices_reason == SKYRIM_GAME_MISSING_DEFAULT_INI)
        return L(@"English is in use. Copy Skyrim_default.ini next to TESV_unpacked.exe in this app’s Skyrim folder, then restart to apply your language settings.",
                 @"今回は英語を使用します。選んだ言語を反映するには、このアプリの「Skyrim」フォルダでTESV_unpacked.exeと同じ場所にSkyrim_default.iniをコピーし、起動し直してください。");
    if (gameLanguage.text_reason && gameLanguage.voices_reason)
        return L(@"Japanese text and voices are not ready. English is in use. Install a complete, supported Japanese pack and restart.",
                 @"日本語のテキスト・音声を利用できないため英語で起動しました。対応する日本語パックを導入して再起動してください。");
    if (gameLanguage.text_reason)
        return L(@"The Japanese text pack is missing or incomplete. English text is in use.",
                 @"日本語テキストのパックが未導入または不完全なため、英語で表示しています。");
    if (gameLanguage.voices_reason)
        return L(@"Japanese voices are not ready. English voices are in use.",
                 @"日本語音声を利用できないため、英語音声で再生しています。");
    return nil;
}
/* ---- Classic MOD plugins ------------------------------------------------- */
static NSArray<NSDictionary *> *modPlugins;
static NSString *modGamePath;
static NSString *modNotice;
/* Choices are stored per profile by mod_manager.m. Rows for plugins that
 * are absent this launch (package off, safe mode) are kept, after the
 * present ones, so they come back unchanged. */
static NSArray<NSDictionary *> *savedModChoices(void) {
    NSMutableArray *result = [NSMutableArray array];
    for (id item in skyrim_mods_plugin_choices())
        if ([item isKindOfClass:NSDictionary.class] && [item[@"name"] isKindOfClass:NSString.class])
            [result addObject:@{@"name":item[@"name"], @"on":@([item[@"on"] boolValue])}];
    return result;
}
static void saveModChoices(NSArray *rows) {
    NSMutableArray *out = [NSMutableArray arrayWithArray:rows];
    NSMutableSet *present = [NSMutableSet set];
    for (NSDictionary *row in rows) [present addObject:[row[@"name"] lowercaseString]];
    for (NSDictionary *row in savedModChoices())
        if (![present containsObject:[row[@"name"] lowercaseString]]) [out addObject:row];
    skyrim_mods_set_plugin_choices(out);
}
static int readPluginHeader(NSString *path, SkyrimPluginHeader *header) {
    memset(header, 0, sizeof *header);
    FILE *f = fopen(path.fileSystemRepresentation, "rb");
    if (!f) return SKYRIM_PLUGIN_HEADER_TRUNCATED;
    unsigned char first[8];
    size_t got = fread(first, 1, sizeof first, f);
    size_t need = skyrim_plugin_header_size(first, got);
    int status;
    if (got < sizeof first) status = SKYRIM_PLUGIN_HEADER_TRUNCATED;
    else if (memcmp(first, "TES4", 4)) status = SKYRIM_PLUGIN_HEADER_NOT_TES4;
    else if (!need) status = SKYRIM_PLUGIN_HEADER_TOO_LARGE;
    else {
        unsigned char *bytes = malloc(need);
        if (!bytes) status = SKYRIM_PLUGIN_HEADER_NO_MEMORY;
        else {
            memcpy(bytes, first, sizeof first);
            size_t rest = fread(bytes + sizeof first, 1, need - sizeof first, f);
            status = skyrim_plugin_parse_header(bytes, sizeof first + rest, header);
            free(bytes);
        }
    }
    fclose(f);
    return status;
}
/* Called before guest entry, or read-only when the player opens the list. */
static int scanModPlugins(BOOL publish, BOOL *skyuiListed) {
    const char *gameDirectory = modGamePath.fileSystemRepresentation;
    modPlugins = @[];
    if (skyuiListed) *skyuiListed = NO;
    int restored = 1;
    char resolved[PATH_MAX];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *data = nil;
    if (gameDirectory && skyrim_plugin_child_path(gameDirectory, "Data", resolved, sizeof resolved) == 1)
        data = [NSString stringWithUTF8String:resolved];
    NSArray<NSString *> *files = data ? [fm contentsOfDirectoryAtPath:data error:NULL] : nil;
    if (!files) {
        fprintf(stderr, "IOS_MODS found=0 data_folder=0\n");
        if (publish) {
            skyrim_mods_plugin_list_failed();
            skyrim_mods_no_plugin_loaded();
        }
        return !publish || skyrim_plugin_apply_list(profilePath.fileSystemRepresentation, NULL, 0) == 0;
    }
    unsigned inheritedDlc = 0;
    int dlcError = skyrim_plugin_read_profile_dlc(profilePath.fileSystemRepresentation, &inheritedDlc);
    if (publish && dlcError) {
        skyrim_mods_plugin_list_failed();
        modNotice = L(@"Could not read the previous DLC choices. The current plugin list is in use.",
                      @"以前のDLC設定を確認できなかったため、現在のプラグイン一覧を使用します。");
        fprintf(stderr, "IOS_MODS dlc_choices_read=%d\n", dlcError);
        return 1;
    }
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (NSString *name in files) {
        NSString *ext = name.pathExtension.lowercaseString;
        if (!([ext isEqualToString:@"esp"] || [ext isEqualToString:@"esm"]) || skyrim_plugin_is_base(name.UTF8String)) continue;
        NSDictionary *attributes = [fm attributesOfItemAtPath:[data stringByAppendingPathComponent:name] error:NULL];
        if ([attributes[NSFileType] isEqual:NSFileTypeRegular]) [names addObject:name];
    }
    const size_t count = names.count;
    SkyrimPluginHeader *headers = calloc(count ? count : 1, sizeof *headers);
    int *status = calloc(count ? count : 1, sizeof *status);
    if (!headers || !status) {
        if (publish) skyrim_mods_plugin_list_failed();
        free(headers); free(status);
        modNotice = L(@"Not enough memory to read the MOD list.", @"メモリ不足のためMODの一覧を読めませんでした。");
        return 1;
    }
    NSMutableDictionary<NSString *, NSNumber *> *index = [NSMutableDictionary dictionary];
    for (size_t i = 0; i < count; ++i) {
        status[i] = readPluginHeader([data stringByAppendingPathComponent:names[i]], &headers[i]);
        index[names[i]] = @(i);
    }
    /* User order first (exact names that still exist), then new files. */
    NSMutableArray<NSDictionary *> *rows = [NSMutableArray array];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    /* Saved names match case-insensitively: package files keep their own case. */
    NSMutableDictionary<NSString *, NSString *> *byLower = [NSMutableDictionary dictionary];
    for (NSString *name in names) byLower[name.lowercaseString] = name;
    for (NSDictionary *choice in skyrim_mods_launch_plugin_choices()) {
        NSString *name = byLower[[choice[@"name"] lowercaseString]];
        if (name && ![seen containsObject:name]) {
            [rows addObject:@{@"name":name, @"on":choice[@"on"]}]; [seen addObject:name];
        }
    }
    NSMutableArray<NSString *> *added = [NSMutableArray array];
    for (NSString *name in names) if (![seen containsObject:name]) [added addObject:name];
    [added sortUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
        size_t ia = index[a].unsignedLongValue, ib = index[b].unsignedLongValue;
        int ra = skyrim_plugin_default_rank(a.UTF8String, status[ia] == SKYRIM_PLUGIN_HEADER_OK && (headers[ia].flags & 1));
        int rb = skyrim_plugin_default_rank(b.UTF8String, status[ib] == SKYRIM_PLUGIN_HEADER_OK && (headers[ib].flags & 1));
        return ra != rb ? (ra < rb ? NSOrderedAscending : NSOrderedDescending) : [a caseInsensitiveCompare:b];
    }];
    for (NSString *name in added) {
        BOOL inherited = (skyrim_plugin_dlc_bit(name.UTF8String) & inheritedDlc) != 0;
        [rows addObject:@{@"name":name, @"on":@(inherited)}];
    }

    SkyrimPluginEntry *entries = calloc(rows.count ? rows.count : 1, sizeof *entries);
    size_t *order = calloc(rows.count ? rows.count : 1, sizeof *order);
    SkyrimPluginState *state = calloc(rows.count ? rows.count : 1, sizeof *state);
    int *problem = calloc(rows.count ? rows.count : 1, sizeof *problem);
    BOOL anyChoice = NO;
    size_t loaded = 0;
    int published = 0;
    if (entries && order && state && problem) {
        for (NSUInteger r = 0; r < rows.count; ++r) {
            size_t i = index[rows[r][@"name"]].unsignedLongValue;
            entries[r] = (SkyrimPluginEntry){names[i].UTF8String, [rows[r][@"on"] boolValue], status[i], &headers[i]};
            anyChoice |= entries[r].enabled;
        }
        loaded = skyrim_plugin_order(entries, rows.count, order, state, problem);
        if (publish) {
            if (loaded) {
                size_t bytes = 0;
                char *text = skyrim_plugin_list_text(entries, order, loaded, &bytes);
                published = text && skyrim_plugin_apply_list(profilePath.fileSystemRepresentation, text, bytes) == 0;
                free(text);
                if (published && skyuiListed) {
                    for (size_t i = 0; i < loaded; ++i) {
                        if (strcasecmp(entries[order[i]].name, "SkyUI.esp") == 0) {
                            *skyuiListed = YES;
                            break;
                        }
                    }
                }
            } else {
                restored = skyrim_plugin_apply_list(profilePath.fileSystemRepresentation, NULL, 0) == 0;
                published = restored;
                skyrim_mods_no_plugin_loaded();
            }
        }
        NSMutableArray *snapshot = [NSMutableArray arrayWithCapacity:rows.count];
        unsigned blocked = 0;
        for (NSUInteger r = 0; r < rows.count; ++r) {
            size_t i = index[rows[r][@"name"]].unsignedLongValue;
            const char *master = problem[r] >= 0 ? skyrim_plugin_master(&headers[i], (unsigned)problem[r]) : NULL;
            BOOL isMaster = status[i] == SKYRIM_PLUGIN_HEADER_OK && (headers[i].flags & 1);
            [snapshot addObject:@{@"name":rows[r][@"name"], @"on":rows[r][@"on"], @"state":@((int)state[r]),
                @"master":@(isMaster), @"problem":master ? @(master) : @""}];
            if (entries[r].enabled && state[r] != SKYRIM_PLUGIN_LOAD) ++blocked;
        }
        modPlugins = snapshot;
        if (publish && !published)
            modNotice = L(@"Could not update the MOD list. The previous list is in use.", @"MODの一覧を更新できなかったため、前回の設定で起動しました。");
        else if (blocked)
            modNotice = [NSString stringWithFormat:L(@"%u MOD file(s) could not be loaded. See Settings > MOD.", @"%u個のMODを読み込めませんでした。設定の「MOD」で理由を確認できます。"), blocked];
        fprintf(stderr, "IOS_MODS found=%lu enabled_loaded=%lu blocked=%u published=%d managed=%d\n",
            (unsigned long)rows.count, (unsigned long)loaded, blocked, published, (int)anyChoice);
    } else {
        modNotice = L(@"Not enough memory to read the MOD list.", @"メモリ不足のためMODの一覧を読めませんでした。");
    }
    if (publish && !published) skyrim_mods_plugin_list_failed();
    for (size_t i = 0; i < count; ++i) skyrim_plugin_header_free(&headers[i]);
    free(headers); free(status); free(entries); free(order); free(state); free(problem);
    return restored;
}
int skyrim_mobile_prepare_mods(const char *gameDirectory) {
    NSCAssert(NSThread.isMainThread, @"MOD selection is published before guest startup");
    modGamePath = gameDirectory ? [NSString stringWithUTF8String:gameDirectory] : nil;
    modPlugins = nil; modNotice = nil;
    BOOL *skyuiResult = NULL;
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    BOOL skyuiListed = NO;
    skyuiResult = &skyuiListed;
#endif
    int prepared = skyrim_mods_manages_plugins() ? scanModPlugins(YES, skyuiResult) :
        skyrim_plugin_apply_list(profilePath.fileSystemRepresentation, NULL, 0) == 0;
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    if (prepared) return skyrim_mods_prepare_skyui_trial(skyuiListed);
#endif
    return prepared;
}
static NSString *modStateText(NSDictionary *row) {
    switch ([row[@"state"] intValue]) {
    case SKYRIM_PLUGIN_LOAD: return L(@"Loaded", @"読み込み中");
    case SKYRIM_PLUGIN_OFF: return L(@"Off", @"オフ");
    case SKYRIM_PLUGIN_UNREADABLE: return L(@"Unreadable: not a classic (LE) plugin?", @"読めません（LE用ではない可能性）");
    case SKYRIM_PLUGIN_BAD_NAME: return L(@"Rename using English letters and digits only", @"ファイル名を半角英数字に変えてください");
    case SKYRIM_PLUGIN_DUPLICATE: return L(@"Same name exists with different capitals", @"大文字・小文字だけ違う同名ファイルがあります");
    case SKYRIM_PLUGIN_MISSING_MASTER: return [NSString stringWithFormat:L(@"Needs %@ (missing or off)", @"%@ が必要です（未導入かオフ）"), row[@"problem"]];
    case SKYRIM_PLUGIN_MASTER_AFTER: return [NSString stringWithFormat:L(@"ESM needs ESP %@", @"ESMがESPの %@ を必要としています"), row[@"problem"]];
    case SKYRIM_PLUGIN_CYCLE: return L(@"Circular requirement", @"互いに必要としあっています");
    case SKYRIM_PLUGIN_LIMIT: return L(@"Over the 253-plugin limit", @"253個の上限を超えています");
    }
    return @"";
}

int skyrim_mobile_verbose(void) { return [startup[@"verbose"] boolValue]; }
int skyrim_mobile_shader_option(unsigned option) {
    const char *key = skyrim_shader_option_key(option);
    return key && skyrim_shader_option_enabled([startup[[NSString stringWithUTF8String:key]] doubleValue]);
}
void skyrim_mobile_render_size(int *width, int *height) {
    int selected = [startup[@"resolution"] intValue];
    if (!selected) return;
    int w = *width, h = *height;
    skyrim_ios_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) return;
    renderHeight = selected >= 2160 ? h : MIN(h, selected);
    renderHeight = MAX(2, renderHeight & ~1);
    renderWidth = MAX(2, ((int)lround((double)w * renderHeight / h)) & ~1);
    *width = renderWidth; *height = renderHeight;
}
int skyrim_mobile_graphics_changed(void) { return iniCount || [startup[@"resolution"] intValue]; }
int skyrim_mobile_ini(unsigned index, const char **section, const char **key, const char **out) {
    /* Called once at startup, before the translated entry and its threads. */
    static int initialized;
    if (!initialized) {
        initialized = 1;
        if (renderWidth && renderHeight) {
            integerINI("Display", "iSize W", renderWidth);
            integerINI("Display", "iSize H", renderHeight);
        }
        fprintf(stderr,"IOS_GRAPHICS override_count=%u resolution=%dx%d\n",iniCount,renderWidth,renderHeight);
    }
    if (index >= iniCount) return 0;
    *section = ini[index].section; *key = ini[index].key; *out = ini[index].value;
    return 1;
}
void skyrim_mobile_frame(void) { __atomic_add_fetch(&frameCount,1,__ATOMIC_RELAXED); }

@class SKOverlay;
static SKOverlay *overlay;
/* Reasons the game is paused for a menu (SKYRIM_MENU_PAUSE_* flags). */
static unsigned menuPauseReasons;
@interface SKPadControl : UIView
@property(nonatomic) NSInteger code;
@property(nonatomic,copy) NSString *caption;
@property(nonatomic,strong) UIImage *symbol;
@property(nonatomic,strong) UITouch *finger;
@property(nonatomic) BOOL editing;
@property(nonatomic) CGPoint stick;
@end
@interface SKOverlay : UIView
@property(nonatomic,strong) NSMutableArray<SKPadControl *> *controls;
@property(nonatomic,strong) NSMutableDictionary *positions;
@property(nonatomic,strong) UITouch *lookFinger;
@property(nonatomic) CGPoint lookPoint;
@property(nonatomic) NSTimeInterval lookTime;
@property(nonatomic) BOOL editing;
@property(nonatomic,strong) UILabel *hud;
@property(nonatomic,strong) UILabel *notice;
@property(nonatomic) NSTimeInterval noticeUntil;
@property(nonatomic,strong) UIButton *settingsButton;
@property(nonatomic,strong) UIButton *saveButton;
@property(nonatomic,strong) UIButton *keyboardButton;
@property(nonatomic,strong) NSDictionary *saveBaseline;
@property(nonatomic) NSTimeInterval saveDeadline;
- (void)clearInput;
- (void)message:(NSString *)text;
- (void)openSettings;
- (void)beginLayout;
- (void)endLayout;
- (void)refreshLanguage;
- (void)logLayout;
@end

@implementation SKPadControl
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.backgroundColor=UIColor.clearColor; self.opaque=NO;
        self.multipleTouchEnabled=YES; self.exclusiveTouch=NO;
        self.isAccessibilityElement=YES;
    }
    return self;
}
- (void)drawRect:(CGRect)rect {
    CGRect circle=CGRectInset(self.bounds,2,2);
    UIBezierPath *path=[UIBezierPath bezierPathWithRoundedRect:circle cornerRadius:self.code==SKYRIM_PAD_MOVE ? self.bounds.size.width/2 : 14];
    [[UIColor colorWithWhite:self.finger ? 0.45 : 0.08 alpha:0.78] setFill]; [path fill];
    [[UIColor colorWithWhite:1 alpha:self.editing ? 1 : 0.65] setStroke]; path.lineWidth=self.editing ? 2.5 : 1; [path stroke];
    if (self.code==SKYRIM_PAD_MOVE) {
        CGFloat size=self.bounds.size.width*0.4;
        CGPoint center=CGPointMake(CGRectGetMidX(self.bounds)+self.stick.x*self.bounds.size.width*.28,
                                  CGRectGetMidY(self.bounds)-self.stick.y*self.bounds.size.height*.28);
        [[UIColor colorWithWhite:0.85 alpha:0.65] setFill];
        [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(center.x-size/2,center.y-size/2,size,size)] fill];
    } else {
        const CGFloat badgeHeight=14;
        CGFloat side=MIN(self.bounds.size.width,self.bounds.size.height)*.48;
        if(self.symbol) {
            CGSize size=self.symbol.size;
            CGFloat scale=side/MAX(size.width,size.height);
            CGRect dst=CGRectMake((self.bounds.size.width-size.width*scale)/2,
                (self.bounds.size.height-badgeHeight-size.height*scale)/2+2,size.width*scale,size.height*scale);
            [self.symbol drawInRect:dst];
        }
        NSMutableParagraphStyle *style=[NSMutableParagraphStyle new]; style.alignment=NSTextAlignmentCenter;
        NSDictionary *attrs=@{NSFontAttributeName:[UIFont systemFontOfSize:self.symbol ? 10 : 18 weight:UIFontWeightSemibold],NSForegroundColorAttributeName:UIColor.whiteColor,NSParagraphStyleAttributeName:style};
        CGFloat y=self.symbol ? self.bounds.size.height-badgeHeight-3 : (self.bounds.size.height-22)/2;
        [self.caption drawInRect:CGRectMake(2,y,self.bounds.size.width-4,22) withAttributes:attrs];
    }
}
- (void)updateStick:(UITouch *)touch {
    CGPoint p=[touch locationInView:self]; CGFloat radius=self.bounds.size.width*.36;
    float x=(p.x-CGRectGetMidX(self.bounds))/radius, y=(CGRectGetMidY(self.bounds)-p.y)/radius;
    float length=hypotf(x,y); if (length>1) {x/=length;y/=length;}
    self.stick=CGPointMake(x,y); skyrim_mobile_move(x,y,clamp(value(@"deadzone",.10),0,.3));
    [self setNeedsDisplay];
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.editing || self.finger) return;
    self.finger=touches.anyObject;
    if (self.code==SKYRIM_PAD_MOVE) [self updateStick:self.finger]; else skyrim_mobile_button((unsigned)self.code,1);
    [self setNeedsDisplay];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.editing && self.code==SKYRIM_PAD_MOVE && self.finger && [touches containsObject:self.finger]) [self updateStick:self.finger];
}
- (void)finishTouches:(NSSet<UITouch *> *)touches cancelled:(BOOL)cancelled {
    if (!self.finger || ![touches containsObject:self.finger]) return;
    self.finger=nil;
    if (self.code==SKYRIM_PAD_MOVE) {self.stick=CGPointZero;skyrim_mobile_move(0,0,0);}
    else if (cancelled) skyrim_mobile_button_cancel((unsigned)self.code);
    else skyrim_mobile_button((unsigned)self.code,0);
    [self setNeedsDisplay];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self finishTouches:touches cancelled:NO];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self finishTouches:touches cancelled:YES];
}
@end

@interface SKSettings : UITableViewController
@property(nonatomic) BOOL sendingDiagnostics;
@property(nonatomic, strong) NSString *diagnosticStatus;
@end
static NSArray *presets(void) { return @[L(@"Minimal", @"最軽量"),L(@"Performance", @"パフォーマンス"),L(@"Balanced", @"バランス"),L(@"High", @"高画質"),L(@"Ultra", @"最高画質")]; }
static NSString *choiceLabel(NSDictionary *row, NSNumber *choice) {
    NSUInteger i=[row[@"values"] indexOfObject:choice];
    if (i!=NSNotFound && row[@"labels"]) return row[@"labels"][i];
    if (choice.doubleValue < 0) return L(@"Keep current", @"維持");
    return choice.stringValue;
}

/* Settings > MOD: on/off and order for plugins found at this launch. States
 * describe this launch; changes are saved immediately and apply next launch. */
@interface SKMods : UITableViewController
@property(nonatomic, strong) NSMutableArray<NSMutableDictionary *> *rows;
@end
@implementation SKMods
- (void)viewDidLoad {
    [super viewDidLoad]; self.title=@"MOD";
    self.tableView.estimatedRowHeight=60; self.tableView.rowHeight=UITableViewAutomaticDimension;
    if (!modPlugins) scanModPlugins(NO, NULL);
    self.rows=[NSMutableArray array];
    for (NSDictionary *row in modPlugins) [self.rows addObject:[row mutableCopy]];
    if (self.rows.count)
        self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:L(@"Reorder", @"並べ替え") style:UIBarButtonItemStylePlain target:self action:@selector(toggleReorder)];
}
- (void)toggleReorder {
    [self setEditing:!self.editing animated:YES];
    self.navigationItem.rightBarButtonItem.title=self.editing ? L(@"Done", @"完了") : L(@"Reorder", @"並べ替え");
}
- (void)persist {
    saveModChoices(self.rows);
    [overlay message:L(@"MOD list saved. Save your game, then fully close and reopen the app.", @"MODの設定を保存しました。ゲーム内で保存し、アプリを完全終了して開き直すと反映します")];
}
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section { return self.rows.count ? (NSInteger)self.rows.count : 1; }
- (NSString *)tableView:(UITableView *)table titleForHeaderInSection:(NSInteger)section {
    return L(@"Plugins (.esp / .esm) — applies after restart", @"プラグイン（.esp／.esm） — 次回起動から反映");
}
- (NSString *)tableView:(UITableView *)table titleForFooterInSection:(NSInteger)section {
    return L(@"Import a classic Skyrim (2011/LE) package under Settings → MOD. Masters load first, followed by their dependents. The selection applies after restarting. Keep a separate save when trying a MOD.",
             @"「設定 → MOD」でSkyrim 2011年版（LE）用のパッケージを追加できます。必要なマスターを先に読み込みます。変更は次回起動時に反映されます。MODを試すときはセーブを分けてください。");
}
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)p {
    UITableViewCell *cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.numberOfLines=2; cell.detailTextLabel.numberOfLines=3;
    cell.detailTextLabel.textColor=UIColor.secondaryLabelColor;
    cell.selectionStyle=UITableViewCellSelectionStyleNone;
    if (!self.rows.count) {
        cell.textLabel.text=L(@"No .esp / .esm files found", @".esp／.esmファイルはありません");
        cell.detailTextLabel.text=L(@"Use Add in the MOD screen, then restart the app.", @"MOD画面から追加した後、アプリを完全終了して開き直してください。");
        return cell;
    }
    NSMutableDictionary *row=self.rows[p.row];
    cell.textLabel.text=row[@"name"];
    NSString *kind=[row[@"master"] boolValue] ? @"ESM" : @"ESP";
    NSString *detail=[NSString stringWithFormat:@"%@ · %@", kind, modStateText(row)];
    if ([row[@"on"] boolValue] != ([row[@"state"] intValue] != SKYRIM_PLUGIN_OFF))
        detail=[detail stringByAppendingFormat:@"\n%@", [row[@"on"] boolValue] ? L(@"Turns on after restart", @"次回起動からオン") : L(@"Turns off after restart", @"次回起動からオフ")];
    cell.detailTextLabel.text=detail;
    UISwitch *toggle=[UISwitch new];
    toggle.on=[row[@"on"] boolValue]; toggle.tag=p.row;
    toggle.accessibilityLabel=row[@"name"];
    [toggle addTarget:self action:@selector(toggled:) forControlEvents:UIControlEventValueChanged];
    cell.accessoryView=toggle;
    return cell;
}
- (void)toggled:(UISwitch *)toggle {
    if (toggle.tag < 0 || (NSUInteger)toggle.tag >= self.rows.count) return;
    self.rows[toggle.tag][@"on"]=@(toggle.on);
    [self persist];
    [self.tableView reloadRowsAtIndexPaths:@[[NSIndexPath indexPathForRow:toggle.tag inSection:0]] withRowAnimation:UITableViewRowAnimationNone];
}
- (BOOL)tableView:(UITableView *)table canMoveRowAtIndexPath:(NSIndexPath *)p { return self.rows.count > 1; }
- (UITableViewCellEditingStyle)tableView:(UITableView *)table editingStyleForRowAtIndexPath:(NSIndexPath *)p { return UITableViewCellEditingStyleNone; }
- (BOOL)tableView:(UITableView *)table shouldIndentWhileEditingRowAtIndexPath:(NSIndexPath *)p { return NO; }
- (void)tableView:(UITableView *)table moveRowAtIndexPath:(NSIndexPath *)from toIndexPath:(NSIndexPath *)to {
    NSMutableDictionary *row=self.rows[from.row];
    [self.rows removeObjectAtIndex:from.row]; [self.rows insertObject:row atIndex:to.row];
    [self persist];
    dispatch_async(dispatch_get_main_queue(), ^{ [self.tableView reloadData]; });  /* refresh switch tags */
}
@end

@implementation SKSettings
- (void)viewDidLoad {
    [super viewDidLoad]; self.title=L(@"SkyrimIOS Settings", @"SkyrimIOS 設定");
    self.tableView.estimatedRowHeight=56; self.tableView.rowHeight=UITableViewAutomaticDimension;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:L(@"Done", @"完了") style:UIBarButtonItemStyleDone target:self action:@selector(close)];
}
- (void)close { [overlay clearInput]; [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    /* Wait for a completed navigation transition; an interactive pop can be cancelled. */
    skyrim_mobile_menu_pause(SKYRIM_MENU_PAUSE_MODS, 0);
    shaderSettingsPermission(YES);
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.tableView reloadData];
}
- (void)viewWillDisappear:(BOOL)animated { shaderSettingsPermission(NO); [super viewWillDisappear:animated]; }
- (NSInteger)numberOfSectionsInTableView:(UITableView *)table { return 9; }
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section {
    return section==0 ? 4 : section==1 ? 5 : section==2 ? graphicsRows().count+1 : section==3 ? 2 : section==5 ? 3 : section==6 ? shaderRows().count : 1;
}
- (NSString *)tableView:(UITableView *)table titleForHeaderInSection:(NSInteger)section {
    return @[L(@"Frame rate & temperature", @"フレームレートと発熱"),L(@"Touch controls", @"タッチ操作"),L(@"Graphics — applies after restart", @"画質 — 次回起動から反映"),L(@"Control layout", @"ボタン配置"),L(@"Saving — applies after restart", @"保存 — 次回起動から反映"),@"Language",L(@"Shader trials — applies after restart", @"シェーダー改善の比較 — 次回起動から反映"),L(@"Diagnostics", @"診断"),L(@"MOD — applies after restart", @"MOD — 次回起動から反映")][section];
}
- (NSString *)tableView:(UITableView *)table titleForFooterInSection:(NSInteger)section {
    if (section==8) return modNotice ?: L(@"Import a classic Skyrim package or folder here. Apply your selection by restarting.", @"2011年版（LE）用のパッケージやフォルダはここから追加できます。変更は次回起動時に反映されます。");
    if (section==7) return L(@"Send performance, settings and numeric error records from this launch and the previous launch to your paired PC. Game data and saves are excluded. Open the game system menu first; keep the PC awake and Tailscale connected. After receipt, tell us in the chat that you sent it.",
        @"今回と前回の起動の性能・設定・エラー番号を、登録した自分のPCへ送ります。ゲームデータやセーブは送りません。先にゲームのシステムメニューを開き、PCを起動してTailscaleを接続してください。送信できたら、このチャットで知らせてください。");
    if (section==0) return L(@"FPS is a limit, not a target guarantee. A 30 FPS limit can reduce heat. Thermal protection caps FPS at 30 / 24 when iOS reports serious / critical heat. Detailed logs apply after restart.", @"FPSは上限です。発熱を抑えるなら30を目安に。自動抑制はiOSが高温を報告した時に30／24へ上限を下げます。詳細ログは次回起動から反映。");
    if (section==1) return L(@"Drag outside the buttons to look. Use the arrows and LB / RB to navigate menus, and LT / RT for the left and right hands. Hold L+R to use both hands together.", @"ボタン以外をドラッグして視点移動。十字キーとLB／RBでメニュー移動、LT／RTで左右の手を操作します。「両手」を長押しすると左右の手を同時に使えます。");
    if (section==2) return L(@"Save in the game, then fully close and reopen the app to apply graphics changes. Keep current stops future overrides; it does not restore old values. Ultra may reduce frame rate.", @"変更後はゲーム内で保存し、アプリを完全終了して開き直してください。「維持」は以降の上書きを止める選択で、以前の値には戻しません。最高画質でも滑らかに動く保証はありません。");
    if (section==3) return L(@"These settings do not pause the game. Open the game system menu first. Quick save sends F5 and checks for a changed save file; it is not an emulator save state.", @"設定画面だけではゲームは一時停止しません。先にシステムメニューを開いてください。クイックセーブはゲームのF5入力を送り、保存ファイルの更新を確認します。実行状態を丸ごと復元するステートセーブではありません。");
    if (section==4) return L(@"Save in the game, then fully close and reopen the app to apply. Off disables saves on pause, travel, waiting and rest; save manually while it is off. Keep current preserves the current values, including a previous Off choice. The game menu can still change them during play.", @"ゲーム内で保存し、アプリを完全終了して開き直すと反映します。オフではメニュー・移動・待機・休息時の自動保存を止めるため、手動で保存してください。「維持」は以前のオフも含めた現在値を保ちます。プレイ中はゲーム内設定からも変更できます。");
    if (section==5) {
        NSString *description = L(@"App language changes immediately. Game text and voices apply after a full app restart and require the corresponding installed pack. Unavailable packs use English.",
            @"アプリの言語はすぐに反映します。ゲームのテキスト・音声はアプリの完全終了・再起動後に反映します。対応パックが未導入・未対応の場合は英語になります。");
        NSString *fallback = gameLanguageFallback();
        return fallback ? [description stringByAppendingFormat:@"\n%@", fallback] : description;
    }
    if (section==6) return L(@"Experimental comparisons: change one option at a time, save your game and restart the app. Preparation may affect heat and startup time. Cache saving is limited to this settings screen, which does not pause the game; open the game system menu first. Incompatible caches are rebuilt. Turning storage off preserves the saved files.", @"比較用の機能です。1項目ずつ変更し、ゲーム内で保存してアプリを再起動してください。発熱や起動時間が変わる場合があります。キャッシュ保存はこの設定画面内だけですが、ゲームは一時停止しないため先にゲームのシステムメニューを開いてください。互換性のないキャッシュは作り直します。保持をオフにしても保存ファイルは削除しません。");
    return nil;
}
- (NSDictionary *)row:(NSIndexPath *)p {
    if (p.section==6) return shaderRows()[p.row];
    if (p.section==0) return @[
       @{@"key":@"fps",@"title":L(@"FPS limit", @"FPS上限"),@"values":@[@0,@24,@30,@40,@60],@"labels":@[L(@"No additional limit", @"追加制限なし"),@"24",@"30",@"40",@"60"]},
       @{@"key":@"thermal",@"title":L(@"Thermal protection", @"高温時の自動抑制"),@"values":@[@0,@1],@"labels":@[L(@"Off", @"オフ"),L(@"On", @"オン")]},
       @{@"key":@"hud",@"title":L(@"Show FPS & thermal state", @"FPS・発熱状態の表示"),@"values":@[@0,@1],@"labels":@[L(@"Off", @"オフ"),L(@"On", @"オン")],@"default":@1},
       @{@"key":@"verbose",@"title":L(@"Detailed diagnostics", @"詳細診断ログ"),@"values":@[@0,@1],@"labels":@[L(@"Off (normal)", @"オフ（通常）"),L(@"On (diagnostics)", @"オン（調査用）")]}
    ][p.row];
    if (p.section==1) return @[
       @{@"key":@"sensitivity",@"title":L(@"Look sensitivity", @"視点感度"),@"values":@[@.25,@.5,@.75,@1,@1.5,@2,@3,@4],@"default":@1.5},
       @{@"key":@"invert",@"title":L(@"Invert vertical look", @"視点の上下反転"),@"values":@[@0,@1],@"labels":@[L(@"Off", @"オフ"),L(@"On", @"オン")]},
       @{@"key":@"deadzone",@"title":L(@"Movement deadzone", @"移動スティックの遊び"),@"values":@[@0,@.05,@.10,@.15,@.2,@.3],@"default":@.1},
       @{@"key":@"size",@"title":L(@"Control size", @"ボタンの大きさ"),@"values":@[@.8,@1,@1.2,@1.4],@"default":@1},
       @{@"key":@"opacity",@"title":L(@"Control opacity", @"ボタンの不透明度"),@"values":@[@.25,@.4,@.6,@.8,@1],@"default":@.8}
    ][p.row];
    if (p.section==4) return @{@"key":@"autosave",@"title":L(@"Autosave", @"オートセーブ"),@"values":@[@-1,@0,@1],@"labels":@[L(@"Keep current", @"現在の設定を維持"),L(@"Off", @"オフ"),L(@"On", @"オン")],@"default":@-1};
    if (p.section==5) return @[
        @{@"key":@"language",@"title":L(@"App language", @"アプリの言語"),@"values":@[@0,@1],@"labels":@[@"English",@"日本語"]},
        @{@"key":@"game_text",@"title":L(@"Game text — after restart", @"ゲームのテキスト — 次回起動"),@"values":@[@0,@1],@"labels":@[@"English",@"日本語"]},
        @{@"key":@"game_voices",@"title":L(@"Game voices — after restart", @"ゲームの音声 — 次回起動"),@"values":@[@0,@1],@"labels":@[@"English",@"日本語"]}
    ][p.row];
    if (p.section==2 && p.row) return graphicsRows()[p.row-1];
    return nil;
}
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)p {
    UITableViewCell *cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.textLabel.numberOfLines=2; cell.detailTextLabel.font=[UIFont systemFontOfSize:14];
    NSDictionary *r=[self row:p];
    if (p.section==8) {
        cell.textLabel.text=L(@"Manage MODs", @"MODの管理");
        cell.imageView.image=[UIImage systemImageNamed:@"puzzlepiece.extension"];
        cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;
        return cell;
    }
    if (p.section==7) {
        cell.textLabel.text=L(@"Send diagnostics to PC", @"診断ログをPCへ送信");
        cell.imageView.image=[UIImage systemImageNamed:@"arrow.up.doc"];
        cell.accessoryType=UITableViewCellAccessoryNone;
        cell.detailTextLabel.text=self.sendingDiagnostics ? L(@"Sending…", @"送信中…") :
            self.diagnosticStatus ?: (skyrim_diagnostics_available() ? nil : L(@"Not paired", @"未接続"));
        return cell;
    } else if (r) {
        cell.textLabel.text=r[@"title"];
        NSNumber *choice=settings[r[@"key"]] ?: r[@"default"] ?: (p.section==2 ? [r[@"values"] firstObject] : @0);
        cell.detailTextLabel.text=choiceLabel(r,choice);
    } else if(p.section==2) {cell.textLabel.text=L(@"Graphics preset", @"画質プリセット");cell.detailTextLabel.text=L(@"Choose", @"選択");}
    else cell.textLabel.text=p.row ? L(@"Reset control layout", @"配置を初期状態に戻す") : L(@"Edit control layout", @"ボタンをドラッグして配置");
    cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}
- (void)applyPreset:(NSUInteger)p {
    /* Each tuple uses only existing SkyrimPrefs keys; no shader or game-code patch. */
    const double v[5][11]={{360,2,1,0,0,512,1500,0,2.5,0,0},
        {540,1,2,1,0,1024,2000,1000,5,0,1},{720,0,4,1,0,1024,3000,3000,8,1,1},
        {900,0,8,1,2,2048,6000,6000,12,1,1},{2160,0,16,0,4,4096,8000,9000,15,1,1}};
    NSArray *rows=graphicsRows();
    for (NSUInteger i=0;i<rows.count;++i) settings[rows[i][@"key"]]=@(v[p][i]);
    persist(); [self.tableView reloadData]; [overlay message:L(@"Graphics saved. Applies after restart.", @"画質を保存しました。次回起動から反映します")];
}
- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)p {
    [table deselectRowAtIndexPath:p animated:YES];
    if (p.section==8) {
        [self.navigationController pushViewController:skyrim_mods_hub_controller() animated:YES];
        return;
    }
    if (p.section==7) {
        if (self.sendingDiagnostics) return;
        self.sendingDiagnostics=YES; [table reloadRowsAtIndexPaths:@[p] withRowAnimation:UITableViewRowAnimationNone];
        logDiagnosticSettings();
        [overlay message:L(@"Preparing diagnostics…", @"診断ログを準備しています…")];
        __weak SKSettings *weakSettings=self;
        skyrim_diagnostics_send(^(SkyrimDiagnosticStatus status, NSString *receipt) {
            NSString *message;
            switch (status) {
            case SkyrimDiagnosticSent:
                message=[NSString stringWithFormat:L(@"Received by PC (%@). Let us know in the chat.", @"PCで受信しました（%@）。チャットで知らせてください。"), [receipt substringToIndex:8]]; break;
            case SkyrimDiagnosticNotConfigured:
                message=L(@"This app is not paired with your PC yet.", @"このアプリにはPCの送信先がまだ登録されていません。"); break;
            case SkyrimDiagnosticBusy:
                message=L(@"Diagnostics are already being sent.", @"診断ログを送信中です。"); break;
            case SkyrimDiagnosticCaptureFailed:
                message=L(@"Could not prepare the report. The original logs are still on this iPhone.", @"診断ログを準備できませんでした。元のログはiPhoneに残っています。"); break;
            case SkyrimDiagnosticConnectionFailed:
                message=L(@"Could not confirm receipt. Check the PC and Tailscale, then tap again to retry the same report.", @"受信を確認できませんでした。PCとTailscaleの接続を確認し、もう一度押すと同じログを再送します。"); break;
            default:
                message=L(@"The PC did not confirm this report. Tell us in the chat; the logs are still on this iPhone.", @"PCから正しい受信確認が届きませんでした。ログはiPhoneに残っています。チャットで知らせてください。"); break;
            }
            SKSettings *controller=weakSettings;
            controller.sendingDiagnostics=NO;
            controller.diagnosticStatus=status==SkyrimDiagnosticSent ? L(@"Received", @"受信済み") : L(@"Not received", @"未受信");
            [controller.tableView reloadData];
            [overlay message:message];
        });
        return;
    }
    if (p.section==3 && !p.row) {
        [self dismissViewControllerAnimated:YES completion:^{[overlay beginLayout];}]; return;
    }
    UIAlertController *sheet=[UIAlertController alertControllerWithTitle:p.section==3 ? L(@"Reset control layout?", @"ボタン配置を戻しますか？") : (p.section==2 && !p.row ? L(@"Graphics preset", @"画質プリセット") : [self row:p][@"title"]) message:nil preferredStyle:UIAlertControllerStyleActionSheet];
    if (p.section==3) {
        [sheet addAction:[UIAlertAction actionWithTitle:L(@"Reset layout", @"配置をリセット") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action){
            [overlay.positions removeAllObjects]; [NSUserDefaults.standardUserDefaults removeObjectForKey:@"mobile-layout-v1"]; [overlay setNeedsLayout];
        }]];
    } else if (p.section==2 && !p.row) {
        for (NSUInteger i=0;i<presets().count;++i) [sheet addAction:[UIAlertAction actionWithTitle:presets()[i] style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){[self applyPreset:i];}]];
    } else {
        NSDictionary *row=[self row:p];
        for (NSNumber *choice in row[@"values"]) [sheet addAction:[UIAlertAction actionWithTitle:choiceLabel(row,choice) style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){
            settings[row[@"key"]]=choice; persist(); [overlay refreshLanguage]; [overlay setNeedsLayout];
            if ([row[@"key"] isEqualToString:@"autosave"])
                [overlay message:L(@"Autosave setting saved. Applies after restart.", @"自動保存の設定を保存しました。次回起動から反映します")];
            if (p.section==6)
                [overlay message:L(@"Shader option saved. Applies after restart.", @"シェーダー設定を保存しました。次回起動から反映します")];
            if (p.section==5 && p.row) {
                unsigned status = p.row == 1 ? gameLanguagePacks.text_status : gameLanguagePacks.voices_status;
                [overlay message:choice.intValue && status != SKYRIM_GAME_PACK_READY ?
                    L(@"Japanese is not ready for this option. English will be used until a supported pack is installed; restart after installation.",
                      @"この項目の日本語パックは未導入・未対応のため英語になります。対応パック導入後に再起動してください。") :
                    L(@"Game language saved. Fully close and reopen the app to apply.", @"ゲームの言語を保存しました。アプリを完全終了して開き直すと反映します。")];
            }
            self.title=L(@"SkyrimIOS Settings", @"SkyrimIOS 設定");
            self.navigationItem.rightBarButtonItem.title=L(@"Done", @"完了"); [table reloadData];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:L(@"Cancel", @"キャンセル") style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView=[table cellForRowAtIndexPath:p];
    sheet.popoverPresentationController.sourceRect=[table cellForRowAtIndexPath:p].bounds;
    [self presentViewController:sheet animated:YES completion:nil];
}
@end

@implementation SKOverlay
- (UIButton *)utility:(NSString *)symbol label:(NSString *)label action:(SEL)action {
    UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];
    [b setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
    b.tintColor=UIColor.whiteColor; b.accessibilityLabel=label;
    b.backgroundColor=[UIColor colorWithWhite:.1 alpha:.75]; b.layer.cornerRadius=10;
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside]; [self addSubview:b]; return b;
}
- (void)refreshLanguage {
    NSArray *labels=@[L(@"Up",@"上"),L(@"Down",@"下"),L(@"Left",@"左"),L(@"Right",@"右"),
        L(@"System menu",@"システムメニュー"),L(@"Wait",@"待機"),L(@"Sneak",@"隠密"),L(@"Change view",@"視点切替"),
        L(@"Sprint / previous tab",@"走る／前のタブ"),L(@"Shout / next tab",@"シャウト／次のタブ"),@"",@"",
        L(@"Use / confirm",@"使う／決定"),L(@"Menu / back",@"メニュー／戻る"),L(@"Draw / sheathe weapon",@"武器を構える／しまう"),
        L(@"Jump",@"ジャンプ"),L(@"Left hand / block",@"左手／防御"),L(@"Right hand / attack",@"右手／攻撃"),L(@"Move",@"移動"),L(@"Use both hands",@"左右の手を同時に使う")];
    for(SKPadControl *c in self.controls) {
        if(c.code==SKYRIM_PAD_BOTH_HANDS) c.caption=L(@"L+R",@"両手");
        c.accessibilityLabel=[NSString stringWithFormat:@"%@ %@",labels[c.code],c.caption];
        [c setNeedsDisplay];
    }
    self.settingsButton.accessibilityLabel=self.editing ? L(@"Finish layout",@"配置完了") : L(@"Settings",@"設定");
    [self.settingsButton setImage:[UIImage systemImageNamed:self.editing ? @"checkmark" : @"gearshape"] forState:UIControlStateNormal];
    self.saveButton.accessibilityLabel=L(@"Quick save",@"クイック保存");
    self.keyboardButton.accessibilityLabel=L(@"Keyboard",@"文字入力");
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.backgroundColor=UIColor.clearColor; self.opaque=NO; self.multipleTouchEnabled=YES;
        self.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        self.controls=[NSMutableArray array]; self.positions=[NSMutableDictionary dictionary];
        id saved=[NSUserDefaults.standardUserDefaults dictionaryForKey:@"mobile-layout-v1"];
        if ([saved isKindOfClass:NSDictionary.class]) [self.positions addEntriesFromDictionary:saved];
        NSArray *captions=@[@"↑",@"↓",@"←",@"→",@"START",@"BACK",@"L3",@"R3",@"LB",@"RB",@"",@"",@"A",@"B",@"X",@"Y",@"LT",@"RT",@"",@"L+R"];
        NSArray *symbols=@[@"arrowtriangle.up.fill",@"arrowtriangle.down.fill",@"arrowtriangle.left.fill",@"arrowtriangle.right.fill",
            @"line.3.horizontal",@"clock",@"figure.walk",@"eye",@"figure.run",@"waveform",@"",@"",
            @"hand.tap",@"arrow.uturn.backward",@"shield.lefthalf.filled",@"arrow.up",@"hand.raised",@"scope",@"",@"hands.sparkles"];
        for (NSInteger i=0;i<captions.count;++i) {
            if (i==10 || i==11) continue;
            SKPadControl *c=[[SKPadControl alloc] initWithFrame:CGRectZero]; c.code=i; c.caption=captions[i];
            if([symbols[i] length]) c.symbol=[[UIImage systemImageNamed:symbols[i]] imageWithTintColor:UIColor.whiteColor renderingMode:UIImageRenderingModeAlwaysOriginal];
            UIPanGestureRecognizer *pan=[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(moveControl:)]; pan.enabled=NO; [c addGestureRecognizer:pan];
            [self.controls addObject:c]; [self addSubview:c];
        }
        self.settingsButton=[self utility:@"gearshape" label:L(@"Settings", @"設定") action:@selector(openSettings)];
        self.saveButton=[self utility:@"square.and.arrow.down" label:L(@"Quick save", @"クイック保存") action:@selector(quicksave)];
        self.keyboardButton=[self utility:@"keyboard" label:L(@"Keyboard", @"文字入力") action:@selector(keyboard)];
        [self refreshLanguage];
        self.hud=[UILabel new]; self.hud.font=[UIFont monospacedDigitSystemFontOfSize:11 weight:UIFontWeightMedium];
        self.hud.textColor=UIColor.whiteColor; self.hud.backgroundColor=[UIColor colorWithWhite:0 alpha:.5]; self.hud.layer.cornerRadius=5; self.hud.clipsToBounds=YES; [self addSubview:self.hud];
        self.notice=[UILabel new]; self.notice.font=[UIFont systemFontOfSize:13 weight:UIFontWeightMedium]; self.notice.textColor=UIColor.whiteColor;
        self.notice.backgroundColor=[UIColor colorWithWhite:0 alpha:.8]; self.notice.textAlignment=NSTextAlignmentCenter; self.notice.numberOfLines=2; self.notice.hidden=YES; [self addSubview:self.notice];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(resignActive:) name:UIApplicationWillResignActiveNotification object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(becomeActive:) name:UIApplicationDidBecomeActiveNotification object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(enterBackground:) name:UIApplicationDidEnterBackgroundNotification object:nil];
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    static const float xy[SKYRIM_PAD_CONTROL_COUNT][2]={{.34,.60},{.34,.80},{.29,.70},{.39,.70},{.62,.07},{.48,.07},{.34,.94},{.70,.94},{.12,.36},{.75,.36},{0,0},{0,0},{.87,.79},{.94,.61},{.80,.61},{.87,.43},{.77,.19},{.93,.19},{.13,.73},{.63,.45}};
    CGRect area=UIEdgeInsetsInsetRect(self.bounds,self.safeAreaInsets);
    CGFloat base=clamp(area.size.height*.13,40,58)*clamp(value(@"size",1),.8,1.4);
    for (SKPadControl *c in self.controls) {
        CGFloat x=xy[c.code][0],y=xy[c.code][1]; id point=self.positions[@(c.code).stringValue];
        if ([point isKindOfClass:NSArray.class] && [point count]==2 && [point[0] isKindOfClass:NSNumber.class] && [point[1] isKindOfClass:NSNumber.class] && isfinite([point[0] doubleValue]) && isfinite([point[1] doubleValue])) {x=[point[0] doubleValue];y=[point[1] doubleValue];}
        CGFloat w=c.code==SKYRIM_PAD_MOVE ? base*2.2 : c.code==4 || c.code==5 ? base*1.15 : base;
        CGFloat h=c.code==SKYRIM_PAD_MOVE ? w : base;
        CGFloat cx=clamp(area.origin.x+x*area.size.width,area.origin.x+w/2,CGRectGetMaxX(area)-w/2);
        CGFloat cy=clamp(area.origin.y+y*area.size.height,area.origin.y+h/2,CGRectGetMaxY(area)-h/2);
        c.frame=CGRectMake(cx-w/2,cy-h/2,w,h); c.alpha=self.editing ? 1 : clamp(value(@"opacity",.8),.25,1); [c setNeedsDisplay];
    }
    CGFloat y=area.origin.y+5;
    self.saveButton.frame=CGRectMake(area.origin.x+6,y,44,44);
    self.keyboardButton.frame=CGRectMake(area.origin.x+58,y,44,44);
    self.settingsButton.frame=CGRectMake(CGRectGetMaxX(area)-50,y,44,44);
    self.hud.frame=CGRectMake(area.origin.x+6,y+50,230,22);
    self.notice.frame=CGRectMake(area.origin.x+area.size.width*.22,CGRectGetMaxY(area)-64,area.size.width*.56,52);
    [self logLayout];
}
/* Numbers only, once per distinct layout: where the top-edge controls end, to tell
 * the overlay apart from the game's own menu scaling when a menu looks clipped. */
- (void)logLayout {
    UIEdgeInsets safe=self.safeAreaInsets;
    CGFloat topMax=CGRectGetMaxY(self.hud.frame);
    for (SKPadControl *c in self.controls) if (c.code==4 || c.code==5) topMax=MAX(topMax,CGRectGetMaxY(c.frame));
    topMax=MAX(topMax,MAX(CGRectGetMaxY(self.saveButton.frame),MAX(CGRectGetMaxY(self.keyboardButton.frame),CGRectGetMaxY(self.settingsButton.frame))));
    NSString *line=[NSString stringWithFormat:@"IOS_OVERLAY_LAYOUT schema=1 view_pt=%ldx%ld safe=%ld,%ld,%ld,%ld top_controls_max_y=%ld scale=%ld",
        lround(self.bounds.size.width),lround(self.bounds.size.height),lround(safe.top),lround(safe.left),lround(safe.bottom),lround(safe.right),
        lround(topMax),lround(self.window.screen.scale*100)];
    static NSString *last;
    if ([line isEqualToString:last]) return;
    last=line;
    fprintf(stderr,"%s\n",line.UTF8String);
}
- (void)message:(NSString *)text {self.notice.text=text;self.noticeUntil=mobileTime()+7;self.notice.hidden=NO;}
- (void)resignActive:(NSNotification *)note { skyrim_lifecycle_request(0); [self clearInput]; skyrim_pipeline_checkpoint_foreground_permission(0); skyrim_ios_metrics_active(0); preparePipelineCacheCheckpoint(); }
- (void)becomeActive:(NSNotification *)note { skyrim_lifecycle_request(menuPauseReasons == 0); finishCacheCheckpoint(cacheCheckpointTicket, 1); shaderSettingsPermission(shaderSettingsVisible); skyrim_ios_metrics_active(menuPauseReasons == 0); }
- (void)enterBackground:(NSNotification *)note { skyrim_lifecycle_did_background(); checkpointPipelineCache(); }
- (void)clearInput {
    self.lookFinger=nil;
    for (SKPadControl *c in self.controls) {c.finger=nil;c.stick=CGPointZero;[c setNeedsDisplay];}
    skyrim_mobile_clear();
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.editing || self.lookFinger) return;
    self.lookFinger=touches.anyObject;self.lookPoint=[self.lookFinger locationInView:self];self.lookTime=self.lookFinger.timestamp;
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.editing || !self.lookFinger || ![touches containsObject:self.lookFinger]) return;
    CGPoint p=[self.lookFinger locationInView:self]; NSTimeInterval t=self.lookFinger.timestamp;
    /* Repeated/predicted timestamps cannot replace a valid velocity with zero.
     * Use the displacement across the delivered batch; do not replay coalesced
     * samples as fresh input after a game-thread stall. */
    if(t<=self.lookTime) return;
    skyrim_mobile_look(p.x-self.lookPoint.x,p.y-self.lookPoint.y,t-self.lookTime,self.bounds.size.height,clamp(value(@"sensitivity",1.5),.25,4),value(@"invert",0)!=0);
    self.lookPoint=p;self.lookTime=t;
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.lookFinger && [touches containsObject:self.lookFinger]) {self.lookFinger=nil;skyrim_mobile_look_end();}
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {[self touchesEnded:touches withEvent:event];}
- (void)openSettings {
    [self clearInput];
    if(self.editing) {[self endLayout];return;}
    UIViewController *root=self.window.rootViewController;
    if(root.presentedViewController) return;
    SKSettings *controller=[[SKSettings alloc] initWithStyle:UITableViewStyleInsetGrouped];
    UINavigationController *nav=[[UINavigationController alloc] initWithRootViewController:controller];
    nav.modalPresentationStyle=UIModalPresentationFormSheet; nav.modalInPresentation=YES;
    [root presentViewController:nav animated:YES completion:nil];
}
- (void)beginLayout {
    [self clearInput]; self.editing=YES;
    for(SKPadControl *c in self.controls) {c.editing=YES;c.gestureRecognizers.firstObject.enabled=YES;}
    [self refreshLanguage];
    self.saveButton.enabled=self.keyboardButton.enabled=NO;
    [self message:L(@"Drag controls to reposition. Tap the checkmark at the top right to save.", @"ボタンと移動スティックをドラッグ。右上のチェックマークで保存")];
    [self setNeedsLayout];
}
- (void)endLayout {
    self.editing=NO;[self clearInput];
    for(SKPadControl *c in self.controls) {c.editing=NO;c.gestureRecognizers.firstObject.enabled=NO;}
    [NSUserDefaults.standardUserDefaults setObject:self.positions forKey:@"mobile-layout-v1"];
    [self refreshLanguage];self.saveButton.enabled=self.keyboardButton.enabled=YES;
    [self message:L(@"Control layout saved.", @"ボタン配置を保存しました")];[self setNeedsLayout];
}
- (void)moveControl:(UIPanGestureRecognizer *)pan {
    if(!self.editing) return;
    UIView *c=pan.view; CGPoint d=[pan translationInView:self];[pan setTranslation:CGPointZero inView:self];
    CGRect area=UIEdgeInsetsInsetRect(self.bounds,self.safeAreaInsets);
    if(area.size.width<1 || area.size.height<1) return;
    CGPoint center=CGPointMake(clamp(c.center.x+d.x,area.origin.x+c.bounds.size.width/2,CGRectGetMaxX(area)-c.bounds.size.width/2),clamp(c.center.y+d.y,area.origin.y+c.bounds.size.height/2,CGRectGetMaxY(area)-c.bounds.size.height/2));
    c.center=center; self.positions[@(((SKPadControl *)c).code).stringValue]=@[@((center.x-area.origin.x)/area.size.width),@((center.y-area.origin.y)/area.size.height)];
}
- (void)keyboard { [self clearInput];extern int platform_text_input(int);platform_text_input(1); }
- (NSDictionary *)saveFiles {
    NSString *path=[profilePath stringByAppendingPathComponent:@"Documents/My Games/Skyrim/Saves"];
    NSMutableDictionary *found=[NSMutableDictionary dictionary];
    for(NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:path error:NULL]) {
        if(![name.pathExtension.lowercaseString isEqualToString:@"ess"] || ![name.lowercaseString containsString:@"quicksave"]) continue;
        NSDictionary *attrs=[NSFileManager.defaultManager attributesOfItemAtPath:[path stringByAppendingPathComponent:name] error:NULL];
        if([attrs[NSFileType] isEqual:NSFileTypeRegular]) found[name]=@[attrs[NSFileModificationDate],attrs[NSFileSize]];
    }
    return found;
}
- (void)quicksave {
    [self clearInput];self.saveBaseline=[self saveFiles];self.saveDeadline=mobileTime()+12;
    skyrim_mobile_quicksave_key();[self message:L(@"Quick save (F5) sent. Checking for a changed save file.", @"クイックセーブ（F5）を送信。保存ファイルの更新を確認中")];
}
@end

/* Uses the inactive-app pause: the guest's main thread parks in a live run
 * loop and GPU work is drained, so UIKit keeps the main thread to itself.
 * Never resumes while the app itself is inactive. Main thread only. */
void skyrim_mobile_menu_pause(unsigned reason, int on) {
    if (!NSThread.isMainThread) return;
    unsigned before = menuPauseReasons;
    menuPauseReasons = on ? (menuPauseReasons | reason) : (menuPauseReasons & ~reason);
    if ((before == 0) == (menuPauseReasons == 0)) return;
    if (menuPauseReasons) { [overlay clearInput]; skyrim_ios_metrics_active(0); skyrim_lifecycle_request(0); return; }
    if (UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;   /* becomeActive resumes */
    skyrim_lifecycle_request(1); skyrim_ios_metrics_active(1);
}
void skyrim_mobile_show_notice(NSString *text) { if (text.length) [overlay message:text]; }
NSArray<NSString *> *skyrim_mobile_loaded_plugins(void) {
    NSMutableArray *out = [NSMutableArray arrayWithObjects:@"Skyrim.esm", @"Update.esm", nil];
    /* The guest reads this profile's list. It may predate the MOD manager
     * or have been restored after a failed deployment. A stale entry is
     * only available when the running Data view contains the named file. */
    char docs[PATH_MAX], folder[PATH_MAX], path[PATH_MAX], data[PATH_MAX];
    if (!profilePath || skyrim_plugin_child_path(profilePath.fileSystemRepresentation, "Documents", docs, sizeof docs) != 1 ||
        skyrim_plugin_child_path(docs, "Skyrim", folder, sizeof folder) != 1 ||
        skyrim_plugin_child_path(folder, "plugins.txt", path, sizeof path) != 1 ||
        skyrim_plugin_child_path(modGamePath.fileSystemRepresentation, "Data", data, sizeof data) != 1) return out;
    NSString *file = [NSString stringWithUTF8String:path];
    NSDictionary *attrs = [NSFileManager.defaultManager attributesOfItemAtPath:file error:NULL];
    if (![attrs[NSFileType] isEqual:NSFileTypeRegular] || [attrs[NSFileSize] unsignedLongLongValue] > 1024 * 1024) return out;
    NSString *text = [NSString stringWithContentsOfFile:file encoding:NSWindowsCP1252StringEncoding error:NULL];
    for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *name = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        const char *plugin = name.UTF8String;
        if (!skyrim_plugin_is_base(plugin) && skyrim_plugin_file_present(data, plugin)) [out addObject:name];
    }
    return out;
}
UIViewController *skyrim_mobile_plugin_controller(void) { return [[SKMods alloc] initWithStyle:UITableViewStyleInsetGrouped]; }

void skyrim_mobile_install(void *window) {
    NSCAssert(NSThread.isMainThread,@"UIKit overlay must run on main");
    SDL_SysWMinfo info;SDL_VERSION(&info.version);
    if(!SDL_GetWindowWMInfo(window,&info) || info.subsystem!=SDL_SYSWM_UIKIT) {
        fprintf(stderr,"FATAL Cannot attach native touch controls: %s\n",SDL_GetError());abort();
    }
    UIWindow *native=info.info.uikit.window;
    hostWindow=native;
    UIView *root=native.rootViewController.view;
    if(!root) {fprintf(stderr,"FATAL SDL UIKit root view missing\n");abort();}
    overlay=[[SKOverlay alloc] initWithFrame:root.bounds];[root addSubview:overlay];
    [overlay message:L(@"Drag an empty area to look. Open the gear for sensitivity, layout and graphics.", @"空いている場所をドラッグして視点移動。右上の設定で感度・配置・画質を変更")];
    NSString *fallback = gameLanguageFallback();
    NSMutableArray *notices = [NSMutableArray array];
    for (NSString *n in @[fallback ?: @"", skyrim_mods_launch_notice() ?: @"", modNotice ?: @"", skyrim_mods_save_notice() ?: @""]) if (n.length) [notices addObject:n];
    if (notices.count) [overlay message:[notices componentsJoinedByString:@"\n"]];
}
void skyrim_mobile_tick(void) {
    if(!overlay || !NSThread.isMainThread) return;
    @autoreleasepool {
    /* SDL 2.32 replaces rootViewController.view when creating its Metal view
     * (SDL_uikitview.setSDLWindow). Reattach to that new view, not the detached
     * initial SDL view. Never derive the window from an already detached pad. */
    UIView *current=hostWindow.rootViewController.view;
    if(current && overlay.superview!=current) {
        [overlay clearInput];[overlay removeFromSuperview];
        overlay.frame=current.bounds;[current addSubview:overlay];[overlay setNeedsLayout];
    }
    /* No state capture/draw through the game's graphics pipeline. */
    if(overlay.superview.subviews.lastObject!=overlay) [overlay.superview bringSubviewToFront:overlay];
    static uint64_t lastFrames,logFrames;static NSTimeInterval lastTime,lastLog,lastSaveCheck,lastCacheCheck;
    NSTimeInterval now=mobileTime();
    if(now-lastTime<.5) return;
    if (now-lastCacheCheck>=5) { lastCacheCheck=now; checkpointWhileSettingsVisible(); }
    uint64_t frames=__atomic_load_n(&frameCount,__ATOMIC_RELAXED);
    skyrim_mods_frame(frames);
    double fps=lastTime ? (frames-lastFrames)/(now-lastTime) : 0;
    lastFrames=frames;lastTime=now;
    NSProcessInfoThermalState thermal=NSProcessInfo.processInfo.thermalState;
    skyrim_mobile_set_thermal((unsigned)thermal);
    NSArray *names=@[L(@"Nominal", @"通常"),L(@"Fair", @"温かい"),L(@"Serious", @"高温"),L(@"Critical", @"非常に高温")];
    unsigned cap=skyrim_mobile_fps_limit();
    overlay.hud.hidden=value(@"hud",1)==0;
    overlay.hud.text=[NSString stringWithFormat:L(@" %.1f FPS  %@  Cap %@", @" %.1f FPS  %@  上限%@"),fps,names[MIN((NSUInteger)thermal,3)],cap ? @(cap).stringValue : @"—"];
    overlay.notice.hidden=now>overlay.noticeUntil;
    if(now-lastLog>=5) {
        uint64_t count,total,maximum,interval;
        skyrim_ios_take_frame_metrics(&count,&total,&maximum,&interval);
        double average=lastLog ? (frames-logFrames)/(now-lastLog) : 0;
        fprintf(stderr,"IOS_PERF frames=%llu fps=%.2f cap=%u thermal=%ld present_host_avg_ms=%.2f present_host_max_ms=%.2f frame_gap_max_ms=%.2f\n",(unsigned long long)frames,average,cap,(long)thermal,count ? (double)total/count/1e6 : 0,(double)maximum/1e6,(double)interval/1e6);lastLog=now;logFrames=frames;
        skyrim_perf_log_window();
        skyrim_lock_log_window();
        skyrim_device_platform_refresh();
        skyrim_device_platform_log();
    }
    if(overlay.saveDeadline && now-lastSaveCheck>=1) {
        NSDictionary *current=[overlay saveFiles];lastSaveCheck=now;
        BOOL updated=NO;
        for(NSString *name in current)
            if([current[name][1] unsignedLongLongValue]>0 && ![current[name] isEqual:overlay.saveBaseline[name]]) updated=YES;
        if(updated) {
            [overlay message:L(@"Quick save file update confirmed.", @"クイックセーブのファイル更新を確認しました")];overlay.saveDeadline=0;
        } else if(now>=overlay.saveDeadline) {
            [overlay message:L(@"Save not confirmed. Open the game menu, then SYSTEM → SAVE.", @"保存は未確認です。システム → SYSTEM → SAVEから保存してください")];overlay.saveDeadline=0;
        }
    }
    }
}
