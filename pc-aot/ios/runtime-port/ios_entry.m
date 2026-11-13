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
#include "device_policy.h"
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
    setvbuf(stderr, NULL, _IOLBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    NSLog(@"SkyrimIOS log: %s", path);
    fprintf(stderr, "SkyrimIOS PC2011 build3 mobile controls; session gameplay requires observation\n");
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
        NSURL *profile = directory(NSApplicationSupportDirectory, @"SkyrimIOS/Profile");
        NSURL *cache = directory(NSCachesDirectory, @"SkyrimIOS/DXVK");
        skyrim_mobile_initialize(profile.fileSystemRepresentation);
        verify_guest_input(game);
        NSURL *framework = [NSBundle.mainBundle.privateFrameworksURL
                             URLByAppendingPathComponent:@"SkyrimGuest.framework/SkyrimGuest"];
        if (!framework || ![[NSFileManager defaultManager] isReadableFileAtPath:framework.path])
            startup_failure(@"Bundled SkyrimGuest.framework is missing or unreadable");
        if (pc_guest_open(framework.fileSystemRepresentation) != 0)
            startup_failure(@"Private guest framework binding failed; see preceding diagnostics");
        skyrim_ios_stage("private-guest-bound");
        set_default("LB_LOG", skyrim_mobile_verbose() ? "1" : "0");
        set_default("DXVK_LOG_PATH", cache.fileSystemRepresentation);
        set_default("DXVK_STATE_CACHE_PATH", cache.fileSystemRepresentation);
        set_default("SKYRIM_VK_PIPELINE_CACHE_PATH", cache.fileSystemRepresentation);
        set_default("SKYDREAM_VSYNC", "1");
        fprintf(stderr, "Game input: %s\nProfile: %s\n", game.fileSystemRepresentation,
                profile.fileSystemRepresentation);
        char *runtime_argv[] = {"SkyrimIOS", (char *)game.fileSystemRepresentation,
                                (char *)profile.fileSystemRepresentation, NULL};
        int result = skyrim_ios_runtime_main(3, runtime_argv);
        fprintf(stderr, "Guest entry returned %d; no title success was inferred\n", result);
        fflush(stderr);
        return result;
    }
}
