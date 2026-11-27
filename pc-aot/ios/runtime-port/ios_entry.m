/* SPDX-License-Identifier: GPL-3.0-only
 * SDL2main owns UIApplicationMain. This function is called by the SDL UIKit
 * delegate after its application startup. No guest bytecode is executed by iOS.
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <CommonCrypto/CommonDigest.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ios_runtime.h"
#include "mobile_ui.h"
#include "mod_manager.h"
#include "device_policy.h"
#include "cpu_sampler.h"
#include "display_timing.h"
#include "diagnostic_upload.h"
#include "PcGuestABI.h"
#ifdef SKYRIM_IOS_MOD_HOOKS
#include "mod_camera_host.h"
#include "mod_ece_tri_host.h"
#include "mod_loading_menu_host.h"
#endif
#if defined(SKYRIM_IOS_CLASSIC_SETTINGS_PROBE) || defined(SKYRIM_IOS_CLASSIC_PAPYRUSUTIL)
#define SKYRIM_IOS_CLASSIC_SETTINGS_INPUT 1
#include "classic_settings.h"
#include <time.h>

static uint8_t classic_bootstrap[8];
static bool classic_input_verified;
#endif
#if defined(SKYRIM_IOS_MOD_HOOKS) && defined(SKYRIM_IOS_SKSE_DLL_PROBE)
#include "mod_dll_probe.h"
#endif
#ifdef SKYRIM_IOS_CLASSIC_SETTINGS_PROBE

static void probe_classic_settings(void) {
    if (!classic_input_verified) return;
    SkyrimPapyrusInputs selected;
    const int acquired = skyrim_mods_acquire_papyrus_inputs(&selected);
    if (acquired) {
        if (acquired != ENOENT) fprintf(stderr, "IOS_CLASSIC_SETTINGS_PROBE acquisition=%d\n", acquired);
        return;
    }

    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    SkseSettingsInputs inputs = {
        .dll = selected.dll, .pex = selected.pex, .dll_size = selected.dll_size, .pex_size = selected.pex_size,
        .game_fd = selected.game_fd, .settings_fd = selected.settings_fd, .epoch = 1,
        .uptime_ms = (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000)
    };
    memcpy(inputs.profile, selected.profile, sizeof inputs.profile);
    memcpy(inputs.bootstrap, classic_bootstrap, sizeof inputs.bootstrap);
    SkseClassicSettings *settings = NULL;
    SkseSettingsStatus status = skse_classic_settings_open(&inputs, &settings);
    skyrim_mods_release_papyrus_inputs(&selected);
    fprintf(stderr, "IOS_CLASSIC_SETTINGS_PROBE open_status=%u\n", (unsigned)status);
    if (status == SKSE_SETTINGS_OK) {
        static const char filename[] = "../WearableLanternsData/common", key[] = "auto_load";
        const SkseClassicScalar arguments[] = {
            {.type = 2, .text = filename, .text_size = sizeof filename},
            {.type = 2, .text = key, .text_size = sizeof key},
            {.type = 3}
        };
        SkseClassicScalarResult value;
        SkseX86Result trace = {0};
        status = skse_classic_settings_prepare(settings, inputs.epoch, arguments, inputs.uptime_ms, &value, &trace);
        fprintf(stderr, "IOS_CLASSIC_SETTINGS_PROBE prepare_status=%u work=%u\n", (unsigned)status, trace.work);
    }
    skse_classic_settings_close(settings);
}
#endif

#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
#include "x86.h"
#include "classic_papyrusutil.h"
#include "mod_features.h"
#include "PcGuestModHooksABI.h"
/* Plan P3-2: one settings scope for the run, opened before the guest starts. */
static SkyrimPapyrusUtilSettings papyrusutil_scope;

static bool wearable_profile_assets(const SkyrimPapyrusInputs *selected) {
    static const unsigned char esp_digest[CC_SHA256_DIGEST_LENGTH] = {
        0x8e, 0x6c, 0x17, 0x00, 0x6b, 0x8f, 0x57, 0xb3,
        0x57, 0xde, 0x2e, 0xd7, 0x7f, 0xd1, 0x2e, 0x8c,
        0xfc, 0x69, 0x09, 0x81, 0x86, 0x75, 0x27, 0x20,
        0x50, 0xaf, 0xb2, 0x1b, 0xac, 0x77, 0xf2, 0x08
    };
    static const unsigned char pex_digest[CC_SHA256_DIGEST_LENGTH] = {
        0xbd, 0xb7, 0xa7, 0xde, 0xbb, 0xbb, 0x46, 0x23,
        0x20, 0xec, 0xfb, 0x1c, 0x3f, 0x31, 0x7a, 0xb7,
        0x53, 0xa4, 0x88, 0xbc, 0xb5, 0xee, 0x1a, 0xe6,
        0x4f, 0x71, 0x45, 0xf4, 0x9d, 0x18, 0x55, 0x13
    };
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    return selected->wearable_plugin && selected->wearable_plugin_size == 110384 &&
        CC_SHA256(selected->wearable_plugin, (CC_LONG)selected->wearable_plugin_size, digest) &&
        memcmp(digest, esp_digest, sizeof digest) == 0 &&
        selected->storage_pex && selected->storage_pex_size == 19558 &&
        CC_SHA256(selected->storage_pex, (CC_LONG)selected->storage_pex_size, digest) &&
        memcmp(digest, pex_digest, sizeof digest) == 0;
}

static void close_papyrusutil_scope(void) {
    skse_classic_settings_close(papyrusutil_scope.settings);
    if (papyrusutil_scope.storage_pex) {
        close(papyrusutil_scope.game_fd);
        free(papyrusutil_scope.storage_pex);
    }
    papyrusutil_scope = (SkyrimPapyrusUtilSettings){0};
}

static void publish_papyrusutil(void) {
    if (!classic_input_verified || !skyrim_mods_feature_enabled(SKYRIM_FEATURE_PAPYRUSUTIL)) return;
    SkyrimPapyrusInputs selected;
    const int acquired = skyrim_mods_acquire_papyrus_inputs(&selected);
    if (acquired) {
        fprintf(stderr, "IOS_PAPYRUSUTIL_OPEN acquisition=%d\n", acquired);
        return;
    }
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    SkseSettingsInputs inputs = {
        .dll = selected.dll, .pex = selected.pex, .dll_size = selected.dll_size, .pex_size = selected.pex_size,
        .game_fd = selected.game_fd, .settings_fd = selected.settings_fd, .epoch = 1,
        .uptime_ms = (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000)
    };
    memcpy(inputs.profile, selected.profile, sizeof inputs.profile);
    memcpy(inputs.bootstrap, classic_bootstrap, sizeof inputs.bootstrap);
    SkseClassicSettings *settings = NULL;
    SkseSettingsStatus status = skse_classic_settings_open(&inputs, &settings);
    if (status != SKSE_SETTINGS_OK) {
        skyrim_mods_release_papyrus_inputs(&selected);
        fprintf(stderr, "IOS_PAPYRUSUTIL_OPEN status=%u\n", (unsigned)status);
        return;
    }
    papyrusutil_scope = (SkyrimPapyrusUtilSettings){
        .settings = settings, .epoch = inputs.epoch, .uptime_ms = inputs.uptime_ms, .game_fd = -1
    };
    const bool lantern_assets = (pc_guest_mod_hooks_selected() & PC_GUEST_MOD_HOOK_FRESH_LOAD) &&
        wearable_profile_assets(&selected);
    if (selected.wearable_plugin) skyrim_papyrusutil_report_lantern(SKYRIM_LANTERN_ASSETS, lantern_assets);
    if (lantern_assets) {
        papyrusutil_scope.storage_pex = selected.storage_pex;
        papyrusutil_scope.storage_pex_size = selected.storage_pex_size;
        papyrusutil_scope.game_fd = selected.game_fd;
        selected.storage_pex = NULL;
        selected.game_fd = -1;
    }
    skyrim_mods_release_papyrus_inputs(&selected);
    SkyrimPapyrusUtilBackend backend;
    skyrim_papyrusutil_settings_backend(&papyrusutil_scope, &backend);
    if (skyrim_papyrusutil_publish(&backend, 1) != 0) close_papyrusutil_scope();
}

static void retire_papyrusutil(void) {
    if (!papyrusutil_scope.settings) return;
    skyrim_papyrusutil_report();
    if (!skse_classic_values_retire(1)) return;
    close_papyrusutil_scope();
}
#endif

__attribute__((noreturn)) static void startup_failure(NSString *message) {
    NSLog(@"SkyrimIOS startup failed: %@", message);
    fprintf(stderr, "FATAL iOS startup: %s\n", message.UTF8String);
    fflush(stderr);
    abort();
}

static NSURL *directory(NSSearchPathDirectory kind, NSString *suffix) {
    NSURL *base = [[NSFileManager defaultManager] URLsForDirectory:kind
                                                       inDomains:NSUserDomainMask].firstObject;
    if (!base) startup_failure(@"Cannot resolve app container directory");
    NSURL *result = [base URLByAppendingPathComponent:suffix isDirectory:YES];
    NSError *error = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtURL:result
                                withIntermediateDirectories:YES attributes:nil error:&error])
        startup_failure([NSString stringWithFormat:@"Cannot create %@: %@", suffix, error]);
    return result;
}

static NSURL *persistent_shader_cache(NSURL *fallback) {
    if (!skyrim_mobile_shader_option(SKYRIM_SHADER_PERSISTENT)) return fallback;
    NSFileManager *manager = NSFileManager.defaultManager;
    NSURL *base = [manager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *target = [base URLByAppendingPathComponent:@"SkyrimIOS/ShaderCache" isDirectory:YES];
    NSError *error = nil;
    BOOL created = target && [manager createDirectoryAtURL:target withIntermediateDirectories:YES attributes:nil error:&error];
    BOOL excluded = created && [target setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&error];
    fprintf(stderr, "IOS_PIPE_CACHE event=storage persistent_requested=1 selected=%u backup_excluded=%u error=%ld\n",
        (unsigned)excluded, (unsigned)excluded, (long)error.code);
    // This is only an acceleration hint. A failed optional location must not
    // prevent gameplay, delete old cache data, or add it to device backups.
    return excluded ? target : fallback;
}

static void begin_log(void) {
    NSURL *logs = directory(NSLibraryDirectory, @"Logs/SkyrimIOS");
    NSString *name = [NSString stringWithFormat:@"run-%.0f-%d-%@.log",
                     NSDate.date.timeIntervalSince1970, getpid(), NSUUID.UUID.UUIDString];
    const char *path = [[logs URLByAppendingPathComponent:name] fileSystemRepresentation];
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) startup_failure([NSString stringWithFormat:@"Cannot create run log: %s", strerror(errno)]);
    if (dup2(fd, STDERR_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0) {
        close(fd);
        startup_failure(@"Cannot redirect runtime log");
    }
    close(fd);
    skyrim_diagnostics_set_log_path(path);
    setvbuf(stderr, NULL, _IOLBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    NSLog(@"SkyrimIOS log: %s", path);
    NSString *build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"];
    fprintf(stderr, "SkyrimIOS PC2011 build=%s; session gameplay requires observation\n",
            build.UTF8String ?: "unknown");
}

static void set_default(const char *name, const char *value) {
    if (setenv(name, value, 0) != 0)
        startup_failure([NSString stringWithFormat:@"Cannot set %s: %s", name, strerror(errno)]);
}

static unsigned hex_digit(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return 256;
}

static void verify_guest_input(NSURL *game) {
    id configured = [NSBundle.mainBundle objectForInfoDictionaryKey:@"PcGuestExpectedInputSHA256"];
    if (![configured isKindOfClass:NSString.class])
        startup_failure(@"PcGuestExpectedInputSHA256 is missing from the locally assembled app");
    const char *hex = [(NSString *)configured UTF8String];
    if (!hex || strlen(hex) != CC_SHA256_DIGEST_LENGTH * 2)
        startup_failure(@"PcGuestExpectedInputSHA256 must contain exactly 64 hexadecimal characters");
    unsigned char expected[CC_SHA256_DIGEST_LENGTH];
    for (size_t i = 0; i < sizeof expected; ++i) {
        unsigned hi = hex_digit((unsigned char)hex[2 * i]);
        unsigned lo = hex_digit((unsigned char)hex[2 * i + 1]);
        if (hi > 15 || lo > 15)
            startup_failure(@"PcGuestExpectedInputSHA256 contains a non-hexadecimal character");
        expected[i] = (unsigned char)((hi << 4) | lo);
    }
    NSURL *input = [game URLByAppendingPathComponent:@"TESV_unpacked.exe"];
    int fd = open(input.fileSystemRepresentation, O_RDONLY);
    if (fd < 0)
        startup_failure([NSString stringWithFormat:@"Cannot verify TESV_unpacked.exe: %s", strerror(errno)]);
    CC_SHA256_CTX context;
    if (CC_SHA256_Init(&context) != 1) {
        close(fd);
        startup_failure(@"Cannot initialize guest-input SHA256 verification");
    }
    unsigned char buffer[64 * 1024];
#ifdef SKYRIM_IOS_CLASSIC_SETTINGS_INPUT
    SkseSettingsBootstrap settings_bootstrap = {0};
    uint64_t input_offset = 0;
#endif
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            int error = errno;
            close(fd);
            startup_failure([NSString stringWithFormat:@"Cannot read TESV_unpacked.exe for verification: %s", strerror(error)]);
        }
        if (count == 0) break;
#ifdef SKYRIM_IOS_CLASSIC_SETTINGS_INPUT
        skse_classic_settings_scan_bootstrap(&settings_bootstrap, input_offset, buffer, (size_t)count);
        input_offset += (size_t)count;
#endif
        if (CC_SHA256_Update(&context, buffer, (CC_LONG)count) != 1) {
            close(fd);
            startup_failure(@"Guest-input SHA256 verification failed while reading");
        }
    }
    if (close(fd) != 0) startup_failure(@"Cannot close the verified guest input");
    unsigned char actual[CC_SHA256_DIGEST_LENGTH];
    if (CC_SHA256_Final(actual, &context) != 1)
        startup_failure(@"Cannot finish guest-input SHA256 verification");
    unsigned mismatch = 0;
    for (size_t i = 0; i < sizeof actual; ++i) mismatch |= actual[i] ^ expected[i];
    if (mismatch)
        startup_failure(@"TESV_unpacked.exe SHA256 does not match this locally assembled app; guest was not loaded");
#ifdef SKYRIM_IOS_MOD_HOOKS
    const bool diagnose = skyrim_mobile_verbose() != 0;
    if (diagnose) skyrim_camera_app_configure(hex);
    skyrim_ece_tri_allow_input(hex, diagnose);
    skyrim_loading_menu_app_configure(hex, diagnose);
#endif
#ifdef SKYRIM_IOS_CLASSIC_SETTINGS_INPUT
    classic_input_verified = skse_classic_settings_copy_bootstrap(&settings_bootstrap, actual, classic_bootstrap);
#endif
#if defined(SKYRIM_IOS_MOD_HOOKS) && defined(SKYRIM_IOS_SKSE_DLL_PROBE)
#ifdef SKYRIM_IOS_CLASSIC_SETTINGS_INPUT
    skyrim_mod_dll_probe_set_game_input(input, actual, classic_input_verified ? classic_bootstrap : NULL);
#else
    skyrim_mod_dll_probe_set_game_input(input, actual, NULL);
#endif
#endif
    skyrim_ios_stage("guest-input-sha256-verified");
}

int SDL_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        if (![NSThread isMainThread]) startup_failure(@"SDL UIKit entry did not run on the main thread");
        begin_log();
        skyrim_ios_stage("sdl-application-entry");
        skyrim_device_platform_initialize();
        // Observation only (presented time, GPU time); prints what it installed.
        (void)skyrim_display_timing_install();
        NSURL *game = directory(NSDocumentDirectory, @"Skyrim");
        NSURL *defaultProfile = directory(NSApplicationSupportDirectory, @"SkyrimIOS/Profile");
        NSURL *cache = directory(NSCachesDirectory, @"SkyrimIOS/DXVK");
        // MOD profiles: the default keeps the existing saves; others have their
        // own. May ask the safe-mode question before anything else starts.
        NSString *profileDirectory = skyrim_mods_select_profile(defaultProfile.path);
        if (!profileDirectory) startup_failure(@"Cannot open the selected MOD profile; the guest was not started");
        NSURL *profile = [NSURL fileURLWithPath:profileDirectory isDirectory:YES];
        skyrim_mobile_initialize(profile.fileSystemRepresentation);
        for (unsigned i = 0; i < SKYRIM_SHADER_OPTION_COUNT; ++i) {
            int enabled = skyrim_mobile_shader_option(i);
            if (setenv(skyrim_shader_option_environment(i), enabled ? "1" : "0", 1))
                startup_failure(@"Cannot initialize shader preparation option");
            fprintf(stderr, "IOS_SHADER_OPTION option=%u enabled=%u\n", i, (unsigned)enabled);
        }
        // Off unless the startup switch set SKYRIM_IOS_CPU_SAMPLE=1; reports its state either way.
        (void)skyrim_cpu_sampler_start();
        NSURL *pipelineCache = persistent_shader_cache(cache);
        if (pipelineCache != cache) {
            // Import only a validated pipeline-v1.cache when the destination
            // is absent. The old cache and DXVK state recipes are preserved.
            set_default("SKYRIM_VK_PIPELINE_CACHE_IMPORT_PATH", cache.fileSystemRepresentation);
        }
        verify_guest_input(game);
        NSURL *framework = [NSBundle.mainBundle.privateFrameworksURL
                             URLByAppendingPathComponent:@"SkyrimGuest.framework/SkyrimGuest"];
        if (!framework || ![[NSFileManager defaultManager] isReadableFileAtPath:framework.path])
            startup_failure(@"Bundled SkyrimGuest.framework is missing or unreadable");
        if (pc_guest_open(framework.fileSystemRepresentation) != 0)
            startup_failure(@"Private guest framework binding failed; see preceding diagnostics");
        skyrim_ios_stage("private-guest-bound");
        // The original game folder, or a copy-on-write clone with the enabled
        // MOD packages laid over Data. The original is never modified.
        const BOOL arabicText = skyrim_mobile_wants_arabic_game_text();
        NSString *effectiveGame = skyrim_mods_prepare_game(game.path, arabicText);
        unsigned arabicMod = skyrim_mods_arabic_text_status(effectiveGame);
        if (!skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, arabicMod))
            startup_failure(@"Cannot safely apply the game language to the app profile; the guest was not started");
        if (arabicText && !skyrim_mobile_uses_arabic_game_text())
            effectiveGame = skyrim_mods_exclude_failed_arabic_text(effectiveGame);
        NSURL *runtimeGame = [NSURL fileURLWithPath:effectiveGame isDirectory:YES];
        if (!skyrim_mobile_prepare_mods(runtimeGame.fileSystemRepresentation))
            startup_failure(@"Cannot restore the profile plugin list; the guest was not started");
#ifdef SKYRIM_IOS_CLASSIC_SETTINGS_PROBE
        probe_classic_settings();
#endif
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
        publish_papyrusutil();
#endif
        set_default("LB_LOG", skyrim_mobile_verbose() ? "1" : "0");
        set_default("DXVK_LOG_PATH", cache.fileSystemRepresentation);
        set_default("DXVK_STATE_CACHE_PATH", cache.fileSystemRepresentation);
        set_default("SKYRIM_VK_PIPELINE_CACHE_PATH", pipelineCache.fileSystemRepresentation);
        set_default("SKYDREAM_VSYNC", "1");
        fprintf(stderr, "Game input: %s\nProfile: %s\n", game.fileSystemRepresentation,
                profile.fileSystemRepresentation);
        char *runtime_argv[] = {"SkyrimIOS", (char *)runtimeGame.fileSystemRepresentation,
                                (char *)profile.fileSystemRepresentation, NULL};
        int result = skyrim_ios_runtime_main(3, runtime_argv);
#ifdef SKYRIM_IOS_MOD_HOOKS
        skyrim_ece_tri_retire();
#endif
#ifdef SKYRIM_IOS_CLASSIC_SKSE
        skyrim_mods_report_features_at_exit();
#endif
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
        retire_papyrusutil();
#endif
        fprintf(stderr, "Guest entry returned %d; no title success was inferred\n", result);
        fflush(stderr);
        return result;
    }
}
