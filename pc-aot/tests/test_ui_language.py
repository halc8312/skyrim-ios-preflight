"""The app UI has its own language; game installations and commercial files are not used."""
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
RESOURCES = PORT / "localization"
LANGUAGES = ("ar", "es", "fr")
QUOTED = r'"(?:[^"\\]|\\.)*"'
PAIR = re.compile(rf"({QUOTED}) = ({QUOTED});")
FORMATS = re.compile(r"%(?:\d+\$)?[-+ #0]*(?:\d+)?(?:\.\d+)?(?:hh|ll|h|l|z)?[@diufxsg%]")


def read_table(path):
    entries = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("/*"):
            continue
        match = PAIR.fullmatch(line)
        if not match:
            raise ValueError(f"{path}: malformed line {line[:25]!r}")
        key, value = map(json.loads, match.groups())
        if key in entries or not value:
            raise ValueError(f"{path}: empty or duplicate entry {key!r}")
        entries[key] = value
    return entries


class AppLanguageTests(unittest.TestCase):
    def test_translation_coverage_and_format_arguments(self):
        files = [PORT / f for f in ("mobile_ui.m", "mod_manager.m", "mod_browser.m", "mod_fomod.m")]
        files.append(ROOT / "experiments/skse-plugin-vm/mod_dll_probe.m")
        inline = re.compile(rf"\b(?:[LT]\(\s*|Text\(\s*[\w.]+\s*,\s*)@({QUOTED})\s*,\s*@")
        keys = {json.loads(raw) for f in files for raw in inline.findall(f.read_text(encoding="utf-8"))}
        # The DLL view also localizes two sets of diagnostic codes supplied by its C core.
        for filename, symbol in (("pe_image.c", "skse_pe_status_text"), ("x86_call.c", "skse_x86_status_text")):
            source = (ROOT / "experiments/skse-plugin-vm" / filename).read_text(encoding="utf-8")
            body = source.split(f"const char *{symbol}(", 1)[1].split("\n}", 1)[0]
            keys.update(json.loads(word) for word in re.findall(QUOTED, body))
        tables = [read_table(RESOURCES / f"{language}.lproj/AppUI.strings") for language in LANGUAGES]
        for language, table in zip(LANGUAGES, tables):
            with self.subTest(language=language):
                self.assertFalse(keys - table.keys(), f"Missing translated UI: {sorted(keys - table.keys())}")
                self.assertEqual(table.keys(), tables[0].keys())
                for key, value in table.items():
                    self.assertEqual(FORMATS.findall(key), FORMATS.findall(value), (language, key))
        for language in ("ja", *LANGUAGES):
            table = read_table(RESOURCES / f"{language}.lproj/InfoPlist.strings")
            self.assertTrue(table["NSLocalNetworkUsageDescription"])

    @unittest.skipUnless(sys.platform == "darwin", "Apple Foundation bundle test")
    def test_preference_migration_and_bundle_lookup(self):
        with tempfile.TemporaryDirectory(prefix="skyrim-ui-localization-") as work:
            contents = Path(work) / "LocaleCheck.app/Contents"
            binary = contents / "MacOS/LocaleCheck"
            binary.parent.mkdir(parents=True)
            shutil.copytree(RESOURCES, contents / "Resources")
            (contents / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleIdentifier": "org.skyrimios.tests.ui-localization",
                "CFBundleExecutable": binary.name, "CFBundlePackageType": "APPL",
                "CFBundleDevelopmentRegion": "en",
            }))
            source = Path(work) / "check.m"
            source.write_text('''#import "ui_language.h"
#include <assert.h>
#include <math.h>
int main(void) { @autoreleasepool {
    assert(SKPreferredUILanguage(@[@"de-DE", @"ar-SA", @"es"]) == SKUIArabic);
    assert(SKPreferredUILanguage(@[@"fr-CA", @"ar"]) == SKUIFrench);
    assert(SKPreferredUILanguage(@[@"en-GB", @"ja"]) == SKUIEnglish);
    assert(SKPreferredUILanguage(@[@"zh-Hant"]) == SKUIEnglish);
    assert(SKUILanguageFromPreference(nil) == SKUIEnglish);
    assert(SKUILanguageFromPreference(@1) == SKUIJapanese);
    assert(SKUILanguageFromPreference(@2) == SKUIArabic);
    assert(SKUILanguageFromPreference(@2.8) == SKUIEnglish);
    assert(SKUILanguageFromPreference(@(NAN)) == SKUIEnglish);
    assert(SKUILanguageFromPreference(@"2") == SKUIEnglish);
    assert(SKUILanguageFromPreference(@(-1)) == SKUIEnglish);
    assert([SKUIString(SKUIEnglish, @"App language", @"アプリの言語") isEqual:@"App language"]);
    assert([SKUIString(SKUIJapanese, @"App language", @"アプリの言語") isEqual:@"アプリの言語"]);
    assert([SKUIString(SKUIArabic, @"App language", nil) isEqual:@"لغة التطبيق"]);
    assert([SKUIString(SKUISpanish, @"App language", nil) isEqual:@"Idioma de la app"]);
    assert([SKUIString(SKUIFrench, @"App language", nil) isEqual:@"Langue de l’app"]);
    assert([SKUIString(SKUIArabic, @"Missing fixture string", nil) isEqual:@"Missing fixture string"]);
    assert([SKUILanguageNames()[SKUIArabic] isEqual:@"العربية"]);
    NSUserDefaults *prefs = NSUserDefaults.standardUserDefaults;
    [prefs setObject:@{@"game_text": @2, @"game_voices": @1} forKey:@"mobile-settings-v1"];
    SKRefreshUILanguage();
    assert(SKCurrentUILanguage() == SKUIEnglish);
    [prefs setObject:@{@"language": @3, @"game_text": @2, @"game_voices": @1} forKey:@"mobile-settings-v1"];
    SKRefreshUILanguage();
    assert(SKCurrentUILanguage() == SKUISpanish);
    NSDictionary *settings = [prefs dictionaryForKey:@"mobile-settings-v1"];
    assert([settings[@"game_text"] integerValue] == 2);
    assert([settings[@"game_voices"] integerValue] == 1);
    [prefs removeObjectForKey:@"mobile-settings-v1"];
} return 0; }
''', encoding="utf-8")
            env = {k: v for k, v in os.environ.items() if k not in ("SDKROOT", "IPHONEOS_DEPLOYMENT_TARGET")}
            subprocess.run(["xcrun", "--sdk", "macosx", "clang", "-fobjc-arc", "-Wall", "-Werror",
                            "-I", str(PORT), str(source), str(PORT / "ui_language.m"), "-framework",
                            "Foundation", "-o", str(binary)], check=True, timeout=60, env=env)
            subprocess.run([str(binary)], check=True, timeout=15, env=env)


if __name__ == "__main__":
    unittest.main()
