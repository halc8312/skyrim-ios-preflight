"""Real MOD plugin header/order code; authored headers only, no game assets or UIKit claims."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


class ModPluginTests(unittest.TestCase):
    def test_ui_formats_the_recorded_notice_when_displayed(self):
        ui = (PORT / "mobile_ui.m").read_text(encoding="utf-8")
        scan = ui[ui.index("static int scanModPlugins("):ui.index("static NSString *modStateText(")]
        for assignment in ("SKModNoticeNoMemory, 0", "SKModNoticePublishFailed, 0",
                           "SKModNoticeDlcReadFailed, 0", "SKModNoticeBlocked, blocked",
                           "SKModNoticeNone, 0"):
            self.assertIn("(SKModNotice){" + assignment + "}", scan)
        self.assertNotIn("modNotice = L(", scan)
        self.assertIn("if (section==8) return modNoticeText() ?: L(", ui)
        self.assertIn("skyrim_mods_launch_notice() ?: @\"\", modNoticeText() ?: @\"\"", ui)

    @unittest.skipUnless(sys.platform == "darwin", "Apple Foundation host required")
    def test_existing_notice_and_plugin_state_follow_the_current_app_language(self):
        ui = (PORT / "mobile_ui.m").read_text(encoding="utf-8")

        def function(start):
            begin = ui.index(start)
            return ui[begin:ui.index("\n}", begin) + 2]

        end = ui.index("static NSString *modNoticeText(")
        start = ui.rindex("typedef enum {", 0, ui.index("} SKModNoticeKind;"))
        notice = ui[start:end] + function("static NSString *modNoticeText(")
        header = ui[ui.index("@implementation SKSettings"):]
        header = header[header.index("- (NSString *)tableView:(UITableView *)table titleForHeaderInSection:"):]
        header = header[:header.index("\n}") + 2]
        pieces = ['''#import <Foundation/Foundation.h>
#include <assert.h>
#include <math.h>
#include "mod_plugins.h"
@class UITableView;
static NSMutableDictionary *settings;
''', function("static double value("), function("static NSString *L("), notice,
            function("static NSString *modStateText("),
            "@interface HeaderProbe : NSObject @end\n@implementation HeaderProbe", header, "@end",
            '''int main(void) { @autoreleasepool {
        settings = [@{@"game_text": @1, @"game_voices": @1} mutableCopy];
        HeaderProbe *headers = [HeaderProbe new];
        NSDictionary *loaded = @{@"state": @(SKYRIM_PLUGIN_LOAD)};
        assert(modNoticeText() == nil);
        modNotice = (SKModNotice){SKModNoticeBlocked, 3};
        for (NSNumber *selection in @[@0, @1, @0]) {
            settings[@"language"] = selection;
            BOOL ja = selection.boolValue;
            assert([modNoticeText() isEqualToString:(ja
                ? @"3個のMODを読み込めませんでした。設定の「MOD」で理由を確認できます。"
                : @"3 MOD file(s) could not be loaded. See Settings > MOD.")]);
            assert([modStateText(loaded) isEqualToString:(ja ? @"読み込み済み" : @"Loaded")]);
            assert([[headers tableView:nil titleForHeaderInSection:5]
                isEqualToString:(ja ? @"言語 / Language" : @"Language / 言語")]);
        }
        settings[@"language"] = @1;
        modNotice = (SKModNotice){SKModNoticeNoMemory, 0};
        assert([modNoticeText() hasPrefix:@"メモリ不足"]);
        settings[@"language"] = @0;
        assert([modNoticeText() hasPrefix:@"Not enough memory"]);
        modNotice = (SKModNotice){SKModNoticePublishFailed, 0};
        assert([modNoticeText() hasPrefix:@"Could not update"]);
        settings[@"language"] = @1;
        assert([modNoticeText() hasPrefix:@"MODの一覧を更新できなかった"]);
        modNotice = (SKModNotice){SKModNoticeDlcReadFailed, 0};
        assert([modNoticeText() hasPrefix:@"以前のDLC設定を確認できなかった"]);
        settings[@"language"] = @0;
        assert([modNoticeText() hasPrefix:@"Could not read the previous DLC choices"]);
    } return 0; }''']
        with tempfile.TemporaryDirectory(prefix="mod-ui-language-") as tmp:
            native = Path(tmp) / "language.m"
            binary = Path(tmp) / "language"
            native.write_text("\n".join(pieces), encoding="utf-8")
            env = {k: v for k, v in os.environ.items() if k not in ("SDKROOT", "IPHONEOS_DEPLOYMENT_TARGET")}
            subprocess.run(["xcrun", "--sdk", "macosx", "clang", "-fobjc-arc", "-Wall", "-Werror",
                            "-I", str(PORT), str(native), "-framework", "Foundation", "-o", str(binary)],
                           check=True, timeout=60, env=env)
            subprocess.run([str(binary)], check=True, timeout=10, env=env)

    def test_real_header_parsing_order_and_plugin_list(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required; run on WSL/Mac")
        with tempfile.TemporaryDirectory(prefix="mod-plugins-") as tmp:
            binary = Path(tmp) / "mod_plugins"
            flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([cc, *flags, "-I", str(PORT), str(PORT / "mod_plugins.c"),
                str(ROOT / "tests/native/test_mod_plugins.c"), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary), tmp], check=True, text=True, capture_output=True, timeout=30)
            for line in ("header parsing PASS", "names PASS", "ordering PASS",
                         "plugins.txt PASS", "case-insensitive paths PASS", "profile restore PASS",
                         "profile DLC inheritance PASS", "usleep dlc masters PASS", "hair pack masters PASS"):
                self.assertIn(line, result.stdout)

    def test_guest_reads_plugins_txt_from_the_profile_documents_folder(self):
        """The guest's SHGetFolderPathA returns Documents for every CSIDL, so the
        classic engine's LOCAL_APPDATA\\Skyrim\\plugins.txt lands under the profile."""
        misc = ROOT / "vendor/skydream/runtime/misc.c"
        if not misc.exists():
            self.skipTest("vendor/skydream checkout required")
        text = misc.read_text(encoding="utf-8")
        shim = text[text.index("SHIM(SHGetFolderPathA)"):text.index("SHIM(SHCreateDirectoryExA)")]
        self.assertIn('"C:\\\\Users\\\\Player\\\\Documents"', shim)
        self.assertNotIn("ARG(1)", shim.split("*/", 1)[1])  # csidl is ignored
        ui = (PORT / "mobile_ui.m").read_text(encoding="utf-8")
        self.assertIn("skyrim_plugin_apply_list(profilePath.fileSystemRepresentation", ui)
        # The native test above checks creation, rollback and exact bytes at
        # profile/Documents/Skyrim/plugins.txt using this production writer.
        entry = (PORT / "ios_entry.m").read_text(encoding="utf-8")
        self.assertIn("skyrim_mobile_initialize(profile.fileSystemRepresentation)", entry)
        self.assertIn("(char *)profile.fileSystemRepresentation, NULL", entry)


if __name__ == "__main__":
    unittest.main()
