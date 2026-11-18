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
#include "diagnostic_upload.h"
#include "PcGuestABI.h"

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
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            int error = errno;
            close(fd);
            startup_failure([NSString stringWithFormat:@"Cannot read TESV_unpacked.exe for verification: %s", strerror(error)]);
        }
        if (count == 0) break;
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
        if (!skyrim_mobile_prepare_game_language(game.fileSystemRepresentation))
            startup_failure(@"Cannot safely apply the game language to the app profile; the guest was not started");
        // The original game folder, or a copy-on-write clone with the enabled
        // MOD packages laid over Data. The original is never modified.
        NSURL *runtimeGame = [NSURL fileURLWithPath:skyrim_mods_prepare_game(game.path) isDirectory:YES];
        if (!skyrim_mobile_prepare_mods(runtimeGame.fileSystemRepresentation))
            startup_failure(@"Cannot restore the profile plugin list; the guest was not started");
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
        fprintf(stderr, "Guest entry returned %d; no title success was inferred\n", result);
        fflush(stderr);
        return result;
    }
}
