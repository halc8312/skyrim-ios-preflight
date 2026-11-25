"""Real startup-only pack/profile code; authored data, no game assets or UIKit claims."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


class GameLanguageTests(unittest.TestCase):
    def test_real_pack_validation_and_profile_transform(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required; run on WSL/Mac")
        with tempfile.TemporaryDirectory(prefix="game-language-") as tmp:
            binary = Path(tmp) / "language"
            pack = Path(tmp) / "game"
            pack.mkdir()
            # Match the app's GNU C11 mode. Darwin's strict POSIX namespace
            # hides O_NOFOLLOW, which the production pack reader requires.
            flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([cc, *flags, "-I", str(PORT), str(PORT / "game_language.c"),
                str(ROOT / "tests/native/test_game_language.c"), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary), str(pack)], check=True, text=True, capture_output=True, timeout=15)
            self.assertIn("pack completeness PASS", result.stdout)
            self.assertIn("missing template deferral and recovery PASS", result.stdout)
            self.assertIn("profile preservation and switching PASS", result.stdout)
            self.assertIn("Arabic MOD English alias and preference recovery PASS", result.stdout)

    def test_real_guest_reads_the_same_case_sensitive_profile_we_select(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required; run on WSL/Mac")
        kernel = (ROOT / "vendor/skydream/runtime/kernel32.c").read_text()
        resolver = kernel[kernel.index("typedef struct { char dir[1024];"):
                          kernel.index("static void mkdirs_for(")]
        shims = (ROOT / "vendor/skydream/runtime/skyrim_shims.c").read_text()
        reader = shims[shims.index("typedef struct { char *sec, *key, *val;"):
                       shims.index("/* Copies a double-NUL list")]
        with tempfile.TemporaryDirectory(prefix="game-language-paths-") as tmp:
            root = Path(tmp)
            (root / "CaseProbe").write_text("authored")
            if (root / "caseprobe").exists():
                self.skipTest("Volume is case-insensitive; run coexistence regression on WSL's Linux filesystem")
            profile = root / "Profile"
            folder = profile / "Documents/My Games/Skyrim"
            folder.mkdir(parents=True)
            native = root / "paths.c"
            native.write_text(r'''
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "game_language.h"
#define GAME_SKYRIM 1
#include "game.h"
#ifdef __APPLE__
#define HOST_STAT_NSEC(st) ((st).st_mtimespec.tv_nsec)
#else
#define HOST_STAT_NSEC(st) ((st).st_mtim.tv_nsec)
#endif
static char g_game_dir[1024], g_save_dir[1024];
static const char *k_win_game_prefix = GAME_WIN_PREFIX;
static const char *k_win_user_prefix = "\\users\\player\\";
''' + resolver + reader + (ROOT / "tests/native/test_game_language_paths.c").read_text())
            binary = root / "paths"
            flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([cc, *flags, "-I", str(PORT), "-I", str(ROOT / "vendor/skydream/runtime"),
                str(native), str(PORT / "game_language.c"), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary), str(profile), str(folder)], check=True,
                text=True, capture_output=True, timeout=15)
            self.assertIn("real guest case-sensitive INI path PASS", result.stdout)

    @unittest.skipUnless(sys.platform == "darwin", "Foundation startup path executes on the Mac validation host")
    def test_actual_startup_keeps_unconfigured_profile_and_atomically_updates_only_profile(self):
        source = (PORT / "mobile_ui.m").read_text()
        function = source[source.index("static BOOL applyGameLanguageProfile("):
                          source.index("static NSString *gameLanguageFallback(")]
        with tempfile.TemporaryDirectory(prefix="game-language-apple-") as tmp:
            native = Path(tmp) / "startup.m"
            native.write_text(r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "game_language.h"
static NSString *profilePath;
static NSDictionary *startup;
static SkyrimGameLanguagePacks gameLanguagePacks;
static SkyrimGameLanguage gameLanguage;
static BOOL gameLanguagePrepared;
''' + function + r'''
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 2);
    assert(skyrim_mobile_classic_text_language() == NULL);
    NSString *root = [NSString stringWithUTF8String:argv[1]];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *game = [root stringByAppendingPathComponent:@"Game"];
    profilePath = [root stringByAppendingPathComponent:@"Profile"];
    NSString *folder = [profilePath stringByAppendingPathComponent:@"Documents/My Games/Skyrim"];
    assert([fm createDirectoryAtPath:game withIntermediateDirectories:YES attributes:nil error:NULL]);
    assert([fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *target = [folder stringByAppendingPathComponent:@"Skyrim.INI"];
    NSString *defaults = [game stringByAppendingPathComponent:@"Skyrim_Default.ini"];
    NSData *original = [@"; existing profile\n[General]\nsLanguage=ENGLISH\nOther=42\n" dataUsingEncoding:NSUTF8StringEncoding];
    // A fresh copy with only the executable and Data must pass language preparation.
    startup = @{@"game_text":@1,@"game_voices":@1};
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_OFF));
    assert(gameLanguagePrepared && gameLanguage.requested_text && gameLanguage.requested_voices);
    assert(!gameLanguage.text && !gameLanguage.voices);
    assert(!strcmp(skyrim_mobile_classic_text_language(), "ENGLISH"));
    assert(gameLanguage.text_reason == SKYRIM_GAME_MISSING_DEFAULT_INI &&
           gameLanguage.voices_reason == SKYRIM_GAME_MISSING_DEFAULT_INI);
    assert(![fm fileExistsAtPath:target] && ![fm fileExistsAtPath:defaults]);
    // Restore just the template and relaunch with the same preferences. The
    // authored game has no Japanese packs, so ordinary pack selection resumes.
    assert([original writeToFile:defaults options:0 error:NULL]);
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_OFF));
    assert(gameLanguage.requested_text && gameLanguage.requested_voices);
    assert(gameLanguage.text_reason == SKYRIM_GAME_PACK_MISSING &&
           gameLanguage.voices_reason == SKYRIM_GAME_PACK_MISSING);
    assert([[NSData dataWithContentsOfFile:defaults] isEqualToData:original]);
    assert([fm fileExistsAtPath:target] && [fm removeItemAtPath:defaults error:NULL]);
    assert([original writeToFile:target options:0 error:NULL]);
    // UI language alone must neither touch the profile nor need a default INI.
    startup = @{@"language":@1};
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_OFF));
    assert(skyrim_mobile_classic_text_language() == NULL);
    assert(gameLanguagePrepared && !gameLanguage.text && !gameLanguage.voices);
    assert([[NSData dataWithContentsOfFile:target] isEqualToData:original]);
    NSDictionary *attributes = [fm attributesOfItemAtPath:target error:NULL];
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_OFF));
    assert([attributes[NSFileModificationDate] isEqual:[fm attributesOfItemAtPath:target error:NULL][NSFileModificationDate]]);
    // The reviewed Arabic MOD applies even without a persisted game language choice.
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_READY));
    assert(gameLanguage.arabic_mod == SKYRIM_ARABIC_MOD_READY);
    assert(!strcmp(skyrim_mobile_classic_text_language(), "ENGLISH"));
    assert(!startup[@"game_text"]);
    // Requested Japanese without a pack falls back explicitly, not silently.
    startup = @{@"game_text":@1,@"game_voices":@1};
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_OFF));
    assert(!gameLanguage.text && !gameLanguage.voices && gameLanguage.text_reason && gameLanguage.voices_reason);
    NSString *updated = [NSString stringWithContentsOfFile:target encoding:NSUTF8StringEncoding error:NULL];
    assert([updated containsString:@"sLanguage=ENGLISH"] && [updated containsString:@"Other=42"]);
    assert([updated containsString:@"sFontConfigFile=Interface\\FontConfig.txt"]);
    assert([updated containsString:@"sResourceArchiveList2=Skyrim - Voices.bsa, Skyrim - VoicesExtra.bsa"]);
    // Existing settings survive a preparation failure; originals are read-only.
    NSData *bad = [NSData dataWithBytes:"a\0b" length:3];
    assert([bad writeToFile:target options:0 error:NULL]);
    assert(!skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_OFF));
    assert(!gameLanguagePrepared);
    assert(skyrim_mobile_classic_text_language() == NULL);
    assert([[NSData dataWithContentsOfFile:target] isEqualToData:bad]);
    assert([fm removeItemAtPath:target error:NULL]);
    assert([original writeToFile:defaults options:0 error:NULL]);
    startup = @{@"game_text":@0};
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_OFF));
    assert([[NSData dataWithContentsOfFile:defaults] isEqualToData:original]);
    assert([fm fileExistsAtPath:target]);
    NSString *lower = [folder stringByAppendingPathComponent:@"Skyrim.ini"];
    if (![fm fileExistsAtPath:lower]) {
        NSData *unused = [@"[General]\nsLanguage=JAPANESE\nUnused=99\n" dataUsingEncoding:NSUTF8StringEncoding];
        assert([unused writeToFile:lower options:0 error:NULL]);
        assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_OFF));
        assert([[NSData dataWithContentsOfFile:lower] isEqualToData:unused]);
        NSString *current = [NSString stringWithContentsOfFile:target encoding:NSUTF8StringEncoding error:NULL];
        assert([current containsString:@"sLanguage=ENGLISH"] && [current containsString:@"Other=42"]);
        assert([fm removeItemAtPath:target error:NULL]);
        assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation, SKYRIM_ARABIC_MOD_OFF));
        assert(![fm fileExistsAtPath:target]);
        current = [NSString stringWithContentsOfFile:lower encoding:NSUTF8StringEncoding error:NULL];
        assert([current containsString:@"sLanguage=ENGLISH"] && [current containsString:@"Unused=99"]);
        puts("Foundation case-sensitive selection PASS");
    } else puts("Foundation case-sensitive selection SKIP: case-insensitive volume; other startup contracts checked");
    // The pack validator is also exercised in the native pack test. With its
    // ready result, the Classic dictionary follows game text even with English UI.
    startup = @{@"language":@0, @"game_text":@1};
    SkyrimGameLanguagePacks ready = {SKYRIM_GAME_PACK_READY, SKYRIM_GAME_PACK_MISSING};
    gameLanguage = skyrim_game_language_select(SKYRIM_GAME_JAPANESE, SKYRIM_GAME_ENGLISH, ready, SKYRIM_ARABIC_MOD_OFF);
    assert(!strcmp(skyrim_mobile_classic_text_language(), "JAPANESE"));
    puts("startup profile-only changes PASS");
} }
''')
            obj = Path(tmp) / "language.o"
            binary = Path(tmp) / "startup"
            subprocess.run(["clang", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(PORT),
                "-c", str(PORT / "game_language.c"), "-o", str(obj)], check=True, timeout=60)
            subprocess.run(["clang", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-fobjc-arc",
                "-DSKYRIM_IOS_CLASSIC_SKSE=1",
                "-I", str(PORT), str(native), str(obj), "-framework", "Foundation", "-o", str(binary)],
                check=True, timeout=60)
            result = subprocess.run([str(binary), tmp], check=True, text=True, capture_output=True, timeout=15)
            self.assertIn("startup profile-only changes PASS", result.stdout)
            if "selection SKIP:" in result.stdout:
                print("Foundation case-sensitive selection SKIP: case-insensitive volume; other startup contracts passed")


if __name__ == "__main__":
    unittest.main()
