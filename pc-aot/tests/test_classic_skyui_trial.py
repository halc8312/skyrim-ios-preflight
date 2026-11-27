"""The real MOD list writer and Classic trial selection, with authored data."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from test_mod_requirements import bsa, skse_pex

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
VM = ROOT / "experiments/skse-plugin-vm"


def between(source, first, following):
    start = source.index(first)
    return source[start:source.index(following, start)]


def function(source, signature):
    return between(source, signature, "\n}\n") + "\n}\n"


class ClassicSkyUITrialTests(unittest.TestCase):
    def test_skyui_trial_is_decided_after_guest_binding_and_before_guest_execution(self):
        source = (PORT / "ios_entry.m").read_text(encoding="utf-8")
        order = [source.index(call) for call in (
            "if (pc_guest_open(", "skyrim_mods_prepare_game(", "skyrim_mobile_prepare_mods(",
            "int result = skyrim_ios_runtime_main(",
        )]
        self.assertEqual(order, sorted(order))

    @unittest.skipUnless(sys.platform == "darwin", "real Foundation startup requires the Mac host")
    def test_only_successfully_published_skyui_activates_the_selected_profiles_trial(self):
        manager = (PORT / "mod_manager.m").read_text(encoding="utf-8")
        mobile = (PORT / "mobile_ui.m").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="classic-skyui-") as tmp:
            root = Path(tmp)
            game_data = root / "Game/Data"
            game_data.mkdir(parents=True)
            header = struct.pack("<4sHfII", b"HEDR", 12, 1.7, 0, 0x800)
            (game_data / "sKyUI.EsP").write_bytes(
                struct.pack("<4sIIIIHH", b"TES4", len(header), 0, 0, 0, 43, 0) + header)
            (game_data / "SkyUI.bsa").write_bytes(bsa())
            scripts = game_data / "Scripts"
            scripts.mkdir()
            (scripts / "SKSE.pex").write_bytes(skse_pex())
            for name in ("UI", "UICallback", "Input", "StringUtil"):
                (scripts / f"{name}.pex").write_bytes(b"support marker")
            bad_scripts = root / "Packages/bad-skse/Data/Scripts"
            bad_scripts.mkdir(parents=True)
            (bad_scripts / "SKSE.pex").write_bytes(skse_pex(release=False))
            source = root / "trial.m"
            source.write_text(r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mod_plugins.h"
#include "mod_requirements.h"
#include "PcGuestSkseTrial.h"
static BOOL launchWithoutMods, launchPluginListFailed, launchHasPackages, healthyMarked;
static NSMutableDictionary *state, *launchSetup;
static NSString *activeProfile, *originalGameDirectory, *packagesPath;
static NSString *launchNotice, *profilePath, *modGamePath;
static NSArray<NSDictionary *> *launchPlugins, *modPlugins;
static NSArray<NSString *> *launchPackageIds;
static NSDictionary *frozenFeaturesProfile;
static int bridgeCalls, bridgeSelection, rejectSelection;
int pc_guest_select_classic_skse(int enabled) {
    ++bridgeCalls;
    bridgeSelection = enabled;
    return rejectSelection ? -1 : 0;
}
static NSString *T(NSString *english, NSString *japanese) { (void)japanese; return english; }
static NSString *StatePath(void) { return [profilePath stringByAppendingPathComponent:@"state.json"]; }
static NSString *LaunchPath(void) { return [profilePath stringByAppendingPathComponent:@"launch.json"]; }
static NSString *PackageDir(NSString *pid) { return [packagesPath stringByAppendingPathComponent:pid]; }
static NSDictionary *LoadPackage(NSString *pid) { (void)pid; return nil; }
/* Other MOD features have their own tests. Observe the startup handoff here. */
static void FreezeFeatures(NSDictionary *profile, NSString *game) {
    assert([game isEqualToString:modGamePath]);
    frozenFeaturesProfile = profile;
}
#define L T
''' + between(manager, "static id MutableJSON(", "static BOOL IsBenchmarkProfile(")
    + function(manager, "static BOOL AnyOn(")
    + function(manager, "static NSArray<NSString *> *DataRootsFor(")
    + function(manager, "static SkyrimSkyUIRequirements SkyUIRequirementsFor(")
    + function(manager, "static NSString *SkyUIRequirementText(")
    + function(manager, "static BOOL SkyUITrialSelected(")
    + function(manager, "BOOL skyrim_mods_prepare_skyui_trial(")
    + function(manager, "static void RestoreLastGoodSetup(")
    + between(manager, "BOOL skyrim_mods_manages_plugins(", "NSArray<NSDictionary *> *skyrim_mods_plugin_choices(")
    + function(manager, "void skyrim_mods_frame(")
    + between(mobile, "typedef enum {\n    SKModNoticeNone,", "\n\n/* Keep the launch finding") + "\n"
    + between(mobile, "static int readPluginHeader(", "static NSString *modStateText(") + r'''
@interface TrialSwitch : NSObject
@property(nonatomic) BOOL on;
@end
@implementation TrialSwitch @end
#define UISwitch TrialSwitch
@interface TrialHub : NSObject
@property(nonatomic, readonly) TrialHub *tableView;
- (void)reloadData;
- (void)skyuiTrialToggled:(UISwitch *)toggle;
@end
static NSString *shownNotice;
static void skyrim_mobile_show_notice(NSString *message) { shownNotice = message; }
''' + function(manager, "static BOOL SaveProfileToggle(") + r'''
@implementation TrialHub
- (TrialHub *)tableView { return self; }
- (void)reloadData {}
''' + function(manager, "- (void)skyuiTrialToggled:") + r'''
@end

static void start(NSString *game, int expected) {
    launchPluginListFailed = NO;
    healthyMarked = NO;
    assert(WriteJSON(@{@"healthy":@NO}, LaunchPath()));
    launchSetup = [@{@"packages":@[], @"plugins":launchPlugins} mutableCopy];
    launchNotice = nil;
    frozenFeaturesProfile = nil;
    int before = bridgeCalls;
    assert(skyrim_mobile_prepare_mods(game.fileSystemRepresentation));
    assert(bridgeCalls == before + 1 && bridgeSelection == expected);
    assert(SkyUITrialSelected(launchSetup) == expected);
    assert(frozenFeaturesProfile == RunningProfile());
}

int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 2);
    NSString *root = [NSString stringWithUTF8String:argv[1]];
    NSString *game = [root stringByAppendingPathComponent:@"Game"];
    originalGameDirectory = game;
    packagesPath = [root stringByAppendingPathComponent:@"Packages"];
    launchPackageIds = @[];
    profilePath = [root stringByAppendingPathComponent:@"Profile"];
    NSString *folder = [profilePath stringByAppendingPathComponent:@"Documents/Skyrim"];
    NSString *list = [folder stringByAppendingPathComponent:@"plugins.txt"];
    NSFileManager *fm = NSFileManager.defaultManager;
    assert([fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSData *original = [@"Skyrim.esm\r\n" dataUsingEncoding:NSUTF8StringEncoding];
    assert([original writeToFile:list options:0 error:NULL]);
    launchPlugins = @[@{@"name":@"SkyUI.esp", @"on":@YES}];

    NSMutableDictionary *running = [@{@"id":@"default"} mutableCopy];
    NSMutableDictionary *other = [@{@"id":@"second"} mutableCopy];
    state = [@{@"profiles":@[running, other], @"next":@"second"} mutableCopy];
    activeProfile = @"default";
    TrialHub *hub = [TrialHub new];
    TrialSwitch *toggle = [TrialSwitch new];
    assert(!SkyUITrialSelected(running) && !SkyUITrialSelected(other));
    toggle.on = YES;
    [hub skyuiTrialToggled:toggle];
    assert(SkyUITrialSelected(other) && !SkyUITrialSelected(running) && shownNotice.length);
    assert(!bridgeCalls && ![fm fileExistsAtPath:[profilePath stringByAppendingPathComponent:@".skyrimios-plugin-baseline-v1"]]);
    NSArray *savedProfiles = MutableJSON([NSData dataWithContentsOfFile:StatePath()])[@"profiles"];
    assert(!SkyUITrialSelected(savedProfiles[0]) && SkyUITrialSelected(savedProfiles[1]));
    assert([fm removeItemAtPath:StatePath() error:NULL]);
    assert([fm createDirectoryAtPath:StatePath() withIntermediateDirectories:NO attributes:nil error:NULL]);
    toggle.on = NO;
    [hub skyuiTrialToggled:toggle];
    assert(toggle.on && SkyUITrialSelected(other) && [shownNotice containsString:@"Could not save"]);
    assert([fm removeItemAtPath:StatePath() error:NULL]);
    start(game, 0);
    activeProfile = @"second";
    start(game, 1);
    assert(!launchNotice && SkyUIRequirementsFor(launchPackageIds).status == SKYRIM_SKYUI_REQ_OK);
    launchPackageIds = @[@"bad-skse"];
    start(game, 1);
    assert([launchNotice containsString:@"Scripts/SKSE.pex is not the SKSE 1.7.3 script."]);
    assert(SkyUIRequirementsFor(launchPackageIds).status != SKYRIM_SKYUI_REQ_OK);
    launchPackageIds = @[];
    toggle.on = NO;
    [hub skyuiTrialToggled:toggle];
    int before = bridgeCalls;
    assert(scanModPlugins(NO, NULL));
    assert(bridgeCalls == before && bridgeSelection == 1);
    start(game, 0);
    activeProfile = @"default";
    state[@"next"] = activeProfile;
    toggle.on = YES;
    [hub skyuiTrialToggled:toggle];
    start(game, 1);
    NSString *published = [NSString stringWithContentsOfFile:list encoding:NSUTF8StringEncoding error:NULL];
    assert([published containsString:@"sKyUI.EsP"]);
    skyrim_mods_frame(1800);
    NSDictionary *knownGood = running[@"lastGood"];
    assert(SkyUITrialSelected(knownGood));
    [running removeObjectForKey:@"skyuiTrial"];
    RestoreLastGoodSetup(running);
    assert(SkyUITrialSelected(running));

    // Safe mode keeps the preference for a future launch and restores the list.
    launchWithoutMods = YES;
    start(game, 0);
    assert(SkyUITrialSelected(running));
    assert([[NSData dataWithContentsOfFile:list] isEqualToData:original]);
    launchWithoutMods = NO;

    // A previous list or a failed write must never activate the trial.
    assert([fm removeItemAtPath:list error:NULL]);
    assert([fm createDirectoryAtPath:list withIntermediateDirectories:NO attributes:nil error:NULL]);
    start(game, 0);
    assert(SkyUITrialSelected(running) && launchNotice.length);
    assert(launchPluginListFailed);
    skyrim_mods_frame(1800);
    assert(running[@"lastGood"] == knownGood);
    assert([MutableJSON([NSData dataWithContentsOfFile:LaunchPath()])[@"healthy"] boolValue]);
    assert([fm removeItemAtPath:list error:NULL]);

    NSString *plugin = [game stringByAppendingPathComponent:@"Data/sKyUI.EsP"];
    NSData *valid = [NSData dataWithContentsOfFile:plugin];
    assert(valid.length && [[@"broken" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:plugin options:0 error:NULL]);
    start(game, 0); // The invalid but enabled file never enters the published order.
    assert([valid writeToFile:plugin options:0 error:NULL]);
    rejectSelection = 1;
    int callsBeforeFailure = bridgeCalls;
    launchSetup = [NSMutableDictionary dictionary];
    frozenFeaturesProfile = nil;
    assert(!skyrim_mobile_prepare_mods(game.fileSystemRepresentation));
    assert(bridgeCalls == callsBeforeFailure + 1 && !SkyUITrialSelected(launchSetup) && !frozenFeaturesProfile);
    rejectSelection = 0;
    launchPlugins = @[@{@"name":@"SkyUI.esp", @"on":@NO}];
    start(game, 0);
    launchPlugins = @[@{@"name":@"SkyUI.esp", @"on":@YES}];
    assert([fm removeItemAtPath:plugin error:NULL]);
    start(game, 0);
    assert(![fm fileExistsAtPath:list]);

    // A healthy setup from an earlier build had no trial key.
    running[@"lastGood"] = @{@"packages":@[], @"plugins":@[]};
    RestoreLastGoodSetup(running);
    assert(!SkyUITrialSelected(running));
    puts("Classic SkyUI trial startup and recovery PASS");
} }
''', encoding="utf-8")
            flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(PORT), "-I", str(VM),
                     "-I", str(ROOT / "ios/private-abi")]
            objects = []
            for c_source in (PORT / "mod_plugins.c", PORT / "mod_requirements.c", PORT / "mod_analyze.c",
                             VM / "classic_pex.c"):
                obj = root / f"{c_source.stem}.o"
                subprocess.run(["clang", *flags, "-c", str(c_source), "-o", str(obj)], check=True, timeout=60)
                objects.append(str(obj))
            binary = root / "trial"
            subprocess.run(["clang", *flags, "-DSKYRIM_IOS_CLASSIC_SKSE=1", "-fobjc-arc", str(source),
                            *objects, "-framework", "Foundation", "-o", str(binary)],
                           check=True, timeout=60)
            result = subprocess.run([str(binary), str(root)], check=True, text=True,
                                    capture_output=True, timeout=15)
            self.assertIn("Classic SkyUI trial startup and recovery PASS", result.stdout)
