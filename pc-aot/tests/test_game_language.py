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
            flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([cc, *flags, "-I", str(PORT), str(PORT / "game_language.c"),
                str(ROOT / "tests/native/test_game_language.c"), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary), str(pack)], check=True, text=True, capture_output=True, timeout=15)
            self.assertIn("pack completeness PASS", result.stdout)
            self.assertIn("profile preservation and switching PASS", result.stdout)

    @unittest.skipUnless(sys.platform == "darwin", "Foundation startup path executes on the Mac validation host")
    def test_actual_startup_keeps_unconfigured_profile_and_atomically_updates_only_profile(self):
        source = (PORT / "mobile_ui.m").read_text()
        function = source[source.index("int skyrim_mobile_prepare_game_language("):
                          source.index("static NSString *gameLanguageFallback(")]
        with tempfile.TemporaryDirectory(prefix="game-language-apple-") as tmp:
            native = Path(tmp) / "startup.m"
            native.write_text(r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdio.h>
#include "game_language.h"
static NSString *profilePath;
static NSDictionary *startup;
static SkyrimGameLanguagePacks gameLanguagePacks;
static SkyrimGameLanguage gameLanguage;
static BOOL gameLanguagePrepared;
''' + function + r'''
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 2);
    NSString *root = [NSString stringWithUTF8String:argv[1]];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *game = [root stringByAppendingPathComponent:@"Game"];
    profilePath = [root stringByAppendingPathComponent:@"Profile"];
    NSString *folder = [profilePath stringByAppendingPathComponent:@"Documents/My Games/Skyrim"];
    assert([fm createDirectoryAtPath:game withIntermediateDirectories:YES attributes:nil error:NULL]);
    assert([fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *target = [folder stringByAppendingPathComponent:@"Skyrim.ini"];
    NSData *original = [@"; existing profile\n[General]\nsLanguage=ENGLISH\nOther=42\n" dataUsingEncoding:NSUTF8StringEncoding];
    assert([original writeToFile:target options:0 error:NULL]);
    // UI language alone must neither touch the profile nor need a default INI.
    startup = @{@"language":@1};
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation));
    assert(gameLanguagePrepared && !gameLanguage.text && !gameLanguage.voices);
    assert([[NSData dataWithContentsOfFile:target] isEqualToData:original]);
    NSDictionary *attributes = [fm attributesOfItemAtPath:target error:NULL];
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation));
    assert([attributes[NSFileModificationDate] isEqual:[fm attributesOfItemAtPath:target error:NULL][NSFileModificationDate]]);
    // Requested Japanese without a pack falls back explicitly, not silently.
    startup = @{@"game_text":@1,@"game_voices":@1};
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation));
    assert(!gameLanguage.text && !gameLanguage.voices && gameLanguage.text_reason && gameLanguage.voices_reason);
    NSString *updated = [NSString stringWithContentsOfFile:target encoding:NSUTF8StringEncoding error:NULL];
    assert([updated containsString:@"sLanguage=ENGLISH"] && [updated containsString:@"Other=42"]);
    assert([updated containsString:@"sFontConfigFile=Interface\\FontConfig.txt"]);
    assert([updated containsString:@"sResourceArchiveList2=Skyrim - Voices.bsa, Skyrim - VoicesExtra.bsa"]);
    // Existing settings survive a preparation failure; originals are read-only.
    NSData *bad = [NSData dataWithBytes:"a\0b" length:3];
    assert([bad writeToFile:target options:0 error:NULL]);
    assert(!skyrim_mobile_prepare_game_language(game.fileSystemRepresentation));
    assert([[NSData dataWithContentsOfFile:target] isEqualToData:bad]);
    assert([fm removeItemAtPath:target error:NULL]);
    NSString *defaults = [game stringByAppendingPathComponent:@"Skyrim_default.ini"];
    assert([original writeToFile:defaults options:0 error:NULL]);
    startup = @{@"game_text":@0};
    assert(skyrim_mobile_prepare_game_language(game.fileSystemRepresentation));
    assert([[NSData dataWithContentsOfFile:defaults] isEqualToData:original]);
    assert([fm fileExistsAtPath:target]);
    puts("startup profile-only changes PASS");
} }
''')
            obj = Path(tmp) / "language.o"
            binary = Path(tmp) / "startup"
            subprocess.run(["clang", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(PORT),
                "-c", str(PORT / "game_language.c"), "-o", str(obj)], check=True, timeout=60)
            subprocess.run(["clang", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-fobjc-arc",
                "-I", str(PORT), str(native), str(obj), "-framework", "Foundation", "-o", str(binary)],
                check=True, timeout=60)
            result = subprocess.run([str(binary), tmp], check=True, text=True, capture_output=True, timeout=15)
            self.assertIn("startup profile-only changes PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
