"""Prepare a simulator-only Xcode project and deterministic authored UI inputs."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import re
import struct
import subprocess
import sys
import uuid
from pathlib import Path
from xml.etree import ElementTree as ET
from zipfile import ZIP_DEFLATED, ZipFile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PORT = ROOT / "ios/runtime-port"
APP_ID = "org.skyrimios.tests.modscreens"
APP = "ModScreensHost"
TEST = "ModScreensUITests"
SCHEME = "ModScreens"
DLL_PROBE = ROOT / "experiments/skse-plugin-vm"
COSAVE = ROOT / "experiments/skse-cosave"


def fixtures(directory):
    inputs = directory / "Imports"
    common = "interface/screens/shared.txt"
    folder_file = inputs / "AuthoredFolder" / "Data" / common
    folder_file.parent.mkdir(parents=True)
    folder_file.write_text("authored folder\n", encoding="utf-8")
    generated_file = inputs / "AuthoredGenerated" / "Data" / "meshes" / "authored" / "body.nif"
    generated_file.parent.mkdir(parents=True)
    generated_file.write_bytes(b"authored output fixture")
    plugin_body = struct.pack("<4sHfII", b"HEDR", 12, 1.7, 0, 0x800)
    plugin_header = struct.pack("<4sIIIIHH", b"TES4", len(plugin_body), 0, 0, 0, 43, 0)
    (inputs / "AuthoredGenerated/Data/AuthoredGenerated.esp").write_bytes(plugin_header + plugin_body)
    with ZipFile(inputs / "AuthoredZip.zip", "w", ZIP_DEFLATED) as archive:
        archive.writestr("Data/" + common, b"authored zip\n")
        archive.writestr("Data/AuthoredPrerequisite.esp", plugin_header + plugin_body)
        for name, pointers in (("se", 8), ("le", 4)):
            archive.writestr(f"Data/meshes/authored/{name}.hkx",
                             struct.pack("<IIII4BIIIII16sII", 0x57E0E057, 0x10C0C010, 0, 8, pointers, 1, 0, 1,
                                         3, 2, 0, 0, 75, b"hk_2010.2.0-r1" + bytes((0, 0xFF)), 0, 0xFFFFFFFF))
    dlc_conditions = """<config><moduleName>Authored DLC check</moduleName><installSteps>
        <installStep name="Default DLC baseline"><visible operator="And">
            <fileDependency file="Dawnguard.esm" state="Active"/>
            <fileDependency file="Dragonborn.esm" state="Missing"/>
            <fileDependency file="HearthFires.esm" state="Missing"/>
        </visible><optionalFileGroups><group name="DLC" type="SelectAny"><plugins>
            <plugin name="Installed and enabled DLC only"/>
        </plugins></group></optionalFileGroups></installStep>
        <installStep name="Separate profile baseline"><visible operator="And">
            <fileDependency file="Dawnguard.esm" state="Missing"/>
            <fileDependency file="Dragonborn.esm" state="Missing"/>
            <fileDependency file="HearthFires.esm" state="Missing"/>
        </visible><optionalFileGroups><group name="DLC" type="SelectAny"><plugins>
            <plugin name="No DLC inherited from Default"/>
        </plugins></group></optionalFileGroups></installStep></installSteps></config>"""
    with ZipFile(inputs / "AuthoredDlcConditions.zip", "w", ZIP_DEFLATED) as archive:
        archive.writestr("fomod/ModuleConfig.xml", dlc_conditions)
    xml = """<config><moduleName>Authored installer</moduleName>
        <requiredInstallFiles><folder source="samples/empty" destination=""/></requiredInstallFiles><installSteps>
        <installStep name="Choose a colour"><optionalFileGroups><group name="Colour" type="SelectExactlyOne"><plugins>
        <plugin name="Red"><description>Install the red sample file.</description><files><file source="samples/red.txt" destination="interface/screens/choice.txt"/></files></plugin>
        <plugin name="Blue"><files><file source="samples/blue.txt" destination="interface/screens/choice.txt"/></files></plugin>
        </plugins></group><group name="Extras" type="SelectAny"><plugins>
        <plugin name="Unavailable sample"><files><file source="samples/not-in-package.txt" destination="interface/screens/missing.txt" priority="100"/></files></plugin>
        </plugins></group></optionalFileGroups></installStep></installSteps>
        <conditionalFileInstalls><patterns><pattern><dependencies operator="And">
        <fileDependency file="Skyrim.esm" state="Active"/>
        <fileDependency file="AuthoredPrerequisite.esp" state="Missing"/>
        </dependencies><files><file source="samples/profile.txt" destination="interface/screens/profile.txt"/>
        </files></pattern></patterns></conditionalFileInstalls></config>"""
    missing_required = xml.replace('</requiredInstallFiles>',
        '<file source="samples/required.txt" destination="interface/screens/missing.txt" priority="100"/></requiredInstallFiles>')
    for name, config in (("AuthoredInstaller.zip", xml), ("AuthoredMissingRequired.zip", missing_required)):
        with ZipFile(inputs / name, "w", ZIP_DEFLATED) as archive:
            archive.writestr("fomod/ModuleConfig.xml", config)
            archive.writestr("samples/red.txt", b"red\n")
            archive.writestr("samples/blue.txt", b"blue\n")
            archive.writestr("samples/profile.txt", b"authored prerequisite disabled in target profile\n")
            archive.writestr("samples/empty/", b"")
    expanded_xml = (xml.replace("Choose a colour", "Choose an expanded-folder sample")
                    .replace("screens/choice.txt", "screens/folder-choice.txt")
                    .replace("screens/profile.txt", "screens/folder-profile.txt"))
    for name in ("AuthoredFolderInstaller", "AuthoredCancelledInstaller", "AuthoredBrokenInstaller", "AuthoredMissingChoiceInstaller"):
        folder = inputs / name
        (folder / "fomod").mkdir(parents=True)
        (folder / "fomod/ModuleConfig.xml").write_text(
            "<incomplete" if name == "AuthoredBrokenInstaller" else expanded_xml, encoding="utf-8")
        (folder / "samples/empty").mkdir(parents=True)
        (folder / "samples/red.txt").write_bytes(b"expanded red\n")
        (folder / "samples/blue.txt").write_bytes(b"expanded blue\n")
        (folder / "samples/profile.txt").write_bytes(b"expanded target profile\n")
        bypass = folder / "Data/interface/screens/unselected-folder-file.txt"
        bypass.parent.mkdir(parents=True)
        bypass.write_bytes(b"only the installer may choose package files\n")
    if os.environ.get("SKYRIM_IOS_SKSE_DLL_PROBE") == "1":
        sys.path.insert(0, str(DLL_PROBE))
        from test_probe_directory import helper, primary
        with ZipFile(inputs / "AuthoredDLL.zip", "w", ZIP_DEFLATED) as archive:
            archive.writestr("Data/SKSE/Plugins/Paired.dll", primary())
            archive.writestr("Data/SKSE/Plugins/OwnedWorker.dll", helper())

    original = directory / "Original/Data/interface/screens/owned.txt"
    original.parent.mkdir(parents=True)
    original.write_bytes(b"synthetic base file\n")
    for name in ("Dawnguard.esm", "Dragonborn.esm"):
        (directory / "Original/Data" / name).write_bytes(
            struct.pack("<4sIIIIHH", b"TES4", len(plugin_body), 1, 0, 0, 43, 0) + plugin_body)
    (directory / "initial-plugins.txt").write_bytes(b"# authored baseline\r\nDawnguard.esm\r\nDragonborn.esm\r\nHearthFires.esm\r\n")
    (directory / "initial-mod-state.json").write_text(json.dumps({
        "version": 1, "next": "default", "profiles": [{"id": "default", "name": "Default", "dir": "Profile",
            "packages": [], "plugins": [{"name": "Dragonborn.esm", "on": False}]}]}), encoding="utf-8")
    def wstr(value):
        return struct.pack("<H", len(value)) + value
    header = (struct.pack("<II", 9, 1) + wstr(b"Fixture") + struct.pack("<I", 1)
              + wstr(b"Room") + wstr(b"Day 1") + wstr(b"Fixture")
              + struct.pack("<HffQII", 0, 0, 1, 0, 1, 1))
    plugins = b"\x03" + wstr(b"Dawnguard.esm") + wstr(b"HearthFires.esm") + wstr(b"UnavailableFixture.esp")
    save = (b"TESV_SAVEGAME" + struct.pack("<I", len(header)) + header + b"\0"*3
            + bytes([74]) + struct.pack("<I", len(plugins)) + plugins)
    (directory / "Authored Save.ess").write_bytes(save)
    if os.environ.get("SKYRIM_IOS_CLASSIC_SKSE") == "1":
        companion = b"SKSE" + struct.pack("<10I", 1, 0x01070030, 0x09200000, 1, 0x4242, 1, 13, 0x41544144, 1, 1)
        (directory / "Authored Save.skse").write_bytes(companion + b"X")


def stable_id(name):
    return hashlib.sha256(name.encode()).hexdigest()[:24].upper()


def plist_string(value):
    """Serialize the small OpenStep property-list subset used by Xcode."""
    if isinstance(value, list):
        return "(" + ", ".join(plist_string(x) for x in value) + ")"
    if isinstance(value, dict):
        pairs = (f"{plist_string(k)} = {plist_string(v)};" for k, v in value.items())
        return "{\n" + "\n".join(pairs) + "\n}"
    return json.dumps(str(value), ensure_ascii=False)


def scheme(path):
    base = path / "xcshareddata/xcschemes"
    base.mkdir(parents=True)
    root = ET.Element("Scheme", LastUpgradeVersion="1600", version="1.7")
    def reference(parent, target):
        extension = "app" if target == APP else "xctest"
        ET.SubElement(parent, "BuildableReference", BuildableIdentifier="primary",
                      BlueprintIdentifier=stable_id(target), BuildableName=f"{target}.{extension}",
                      BlueprintName=target, ReferencedContainer="container:ModScreens.xcodeproj")
    action = ET.SubElement(root, "BuildAction", parallelizeBuildables="YES", buildImplicitDependencies="YES")
    entries = ET.SubElement(action, "BuildActionEntries")
    for target in (APP, TEST):
        entry = ET.SubElement(entries, "BuildActionEntry", buildForTesting="YES",
                              buildForRunning="YES" if target == APP else "NO",
                              buildForProfiling="NO", buildForArchiving="NO", buildForAnalyzing="NO")
        reference(entry, target)
    test = ET.SubElement(root, "TestAction", buildConfiguration="Debug",
                         selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB",
                         selectedLauncherIdentifier="Xcode.DebuggerFoundation.Launcher.LLDB",
                         shouldUseLaunchSchemeArgsEnv="YES")
    item = ET.SubElement(ET.SubElement(test, "Testables"), "TestableReference", skipped="NO")
    reference(item, TEST)
    reference(ET.SubElement(test, "MacroExpansion"), APP)
    ET.ElementTree(root).write(base / f"{SCHEME}.xcscheme", encoding="utf-8", xml_declaration=True)


def project(work, sdl_prefix, archive_prefix):
    work.mkdir(parents=True, exist_ok=True)
    fixture_dir = work / "Fixtures"
    fixtures(fixture_dir)
    project_dir = work / "ModScreens.xcodeproj"
    project_dir.mkdir()
    app_info = {
        "CFBundleIdentifier": "$(PRODUCT_BUNDLE_IDENTIFIER)", "CFBundleName": "$(PRODUCT_NAME)",
        "CFBundleExecutable": "$(EXECUTABLE_NAME)", "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1", "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleDisplayName": "MOD Screen Test", "CFBundleDevelopmentRegion": "en",
        "LSRequiresIPhoneOS": True, "LSSupportsOpeningDocumentsInPlace": True, "UIFileSharingEnabled": True,
        "UILaunchScreen": {}, "UISupportedInterfaceOrientations": ["UIInterfaceOrientationLandscapeLeft",
                                                                   "UIInterfaceOrientationLandscapeRight"],
        "UIApplicationSceneManifest": {"UIApplicationSupportsMultipleScenes": False,
            "UISceneConfigurations": {"UIWindowSceneSessionRoleApplication": [
                {"UISceneConfigurationName": "Settings", "UISceneDelegateClassName": "SKFixtureScene"}]}}}
    (work / "HostInfo.plist").write_bytes(plistlib.dumps(app_info))

    objects = {}
    def obj(identifier, isa, **values):
        key = stable_id(identifier)
        assert key not in objects
        objects[key] = dict(isa=isa, **values)
        return key
    def configs(label, values):
        config = obj(label+"Debug", "XCBuildConfiguration", name="Debug", buildSettings=values)
        return obj(label+"Configs", "XCConfigurationList", buildConfigurations=[config],
                   defaultConfigurationIsVisible=0, defaultConfigurationName="Debug")

    refs = []
    def buildfile(path, filetype):
        ref = obj("ref:"+str(path), "PBXFileReference", lastKnownFileType=filetype,
                  path=str(path), sourceTree="<absolute>")
        refs.append(ref)
        return obj("build:"+str(path), "PBXBuildFile", fileRef=ref)
    def source_phase(name, sources):
        files = []
        types = {".m": "sourcecode.c.objc", ".c": "sourcecode.c.c", ".swift": "sourcecode.swift"}
        for source in sources:
            if not source.is_file():
                raise ValueError(f"Missing simulator input: {source}")
            files.append(buildfile(source, types[source.suffix]))
        return obj(name+"Sources", "PBXSourcesBuildPhase", files=files,
                   buildActionMask=2147483647, runOnlyForDeploymentPostprocessing=0)
    app_sources = [
        "mobile_ui.m", "mobile_core.c", "game_language.c", "diagnostic_upload.m",
        "performance_metrics.c", "present_pacing_core.c", "lock_metrics.c", "mod_manager.m", "mod_fomod.m",
        "mod_browser.m", "mod_plugins.c", "mod_archive.c", "mod_deploy.c",
        "mod_analyze.c", "mod_optimize.c", "mod_libarchive.c", "mod_translate.swift",
    ]
    dll_enabled = os.environ.get("SKYRIM_IOS_SKSE_DLL_PROBE") == "1"
    dll_sources = [DLL_PROBE / p for p in [
        "pe_image.c", "x86_call.c", "x86_heap.c", "x86_winthread.c", "x86_wintext.c", "x86_winfile.c", "x86_winsync.c",
        "x86_winenv.c", "papyrus_trial.c",
        "x86_skse_records.c", "dependency_plan.c", "plugin_trial.c",
        "probe_local_file.c", "probe_directory.c", "probe_asset_file.c", "mod_dll_probe.m"
    ]] if dll_enabled else []
    classic_enabled = os.environ.get("SKYRIM_IOS_CLASSIC_SKSE") == "1"
    if classic_enabled:
        app_sources.extend(("mod_requirements.c", "mod_features.c"))
    if classic_enabled or dll_enabled:
        dll_sources.append(DLL_PROBE / "classic_pex.c")
    if classic_enabled and dll_enabled:
        app_sources.append("mod_papyrus_inputs.c")
    cosave_sources = [COSAVE / "copy_pair.c"]
    if classic_enabled:
        cosave_sources.extend(COSAVE / p for p in ("inspect.c", "records.c"))
    app_source_phase = source_phase(APP, [
        HERE/"host.m", HERE/"guest_boundary.m", *(PORT/p for p in app_sources), *dll_sources, *cosave_sources
    ])
    test_source_phase = source_phase(TEST, [HERE/"screens.m"])
    if not (sdl_prefix / "include/SDL2/SDL.h").is_file():
        raise SystemExit("Simulator host requires real public SDL headers from the prepared dependency prefix")
    static_libs = [archive_prefix / "lib" / name for name in ("libarchive.a", "liblzma.a")]
    if not all(p.is_file() for p in static_libs):
        raise SystemExit("Simulator libarchive and liblzma must be built for iphonesimulator")
    for codec, h in (("bcdec", "bcdec.h"), ("stb", "stb_dxt.h")):
        if not (ROOT/"vendor"/codec/h).is_file():
            raise SystemExit("Run scripts/fetch-mod-codecs.sh first")

    resources = obj("Resources", "PBXResourcesBuildPhase", files=[buildfile(fixture_dir, "folder")],
                    buildActionMask=2147483647, runOnlyForDeploymentPostprocessing=0)
    app_product = obj("appProduct", "PBXFileReference", explicitFileType="wrapper.application",
                      path=APP+".app", sourceTree="BUILT_PRODUCTS_DIR")
    test_product = obj("testProduct", "PBXFileReference", explicitFileType="wrapper.cfbundle",
                       path=TEST+".xctest", sourceTree="BUILT_PRODUCTS_DIR")
    products = obj("Products", "PBXGroup", children=[app_product, test_product], name="Products", sourceTree="<group>")
    group = obj("Group", "PBXGroup", children=refs+[products], sourceTree="<group>")
    project_settings = configs("project", {
        "SDKROOT":"iphonesimulator", "SUPPORTED_PLATFORMS":"iphonesimulator", "IPHONEOS_DEPLOYMENT_TARGET":"18.0",
        "CLANG_ENABLE_MODULES":"YES", "CLANG_ENABLE_OBJC_ARC":"YES", "TARGETED_DEVICE_FAMILY":"1",
        "GCC_C_LANGUAGE_STANDARD":"gnu11", "ONLY_ACTIVE_ARCH":"YES", "CODE_SIGN_STYLE":"Automatic"})
    frameworks = ["Foundation","UIKit","QuartzCore","UniformTypeIdentifiers","WebKit","SwiftUI","Translation"]
    classic_defines = ["SKYRIM_IOS_CLASSIC_SKSE=1"] if classic_enabled else []
    dll_defines = ["SKYRIM_IOS_SKSE_DLL_PROBE=1"] if dll_enabled else []
    app_configs = configs(APP, {
        "PRODUCT_NAME":APP, "PRODUCT_BUNDLE_IDENTIFIER":APP_ID, "INFOPLIST_FILE":str(work/"HostInfo.plist"),
        "SWIFT_VERSION":"6.0", "SWIFT_STRICT_CONCURRENCY":"complete",
        "HEADER_SEARCH_PATHS":[str(PORT),str(ROOT/"ios/private-abi"),str(sdl_prefix/"include"),str(archive_prefix/"include"),
                               str(COSAVE), *([str(DLL_PROBE)] if dll_enabled or classic_enabled else [])],
        "SYSTEM_HEADER_SEARCH_PATHS":[str(ROOT/"vendor/bcdec"),str(ROOT/"vendor/stb")],
        "GCC_PREPROCESSOR_DEFINITIONS":["$(inherited)", "SKYRIM_HAVE_LIBARCHIVE=1", *classic_defines, *dll_defines],
        "OTHER_LDFLAGS":["$(inherited)",*map(str,static_libs),"-lz","-liconv",
                         *[arg for name in frameworks for arg in ("-framework", name)]]})
    test_configs = configs(TEST, {
        "PRODUCT_NAME":TEST, "PRODUCT_BUNDLE_IDENTIFIER":APP_ID+".uitests", "GENERATE_INFOPLIST_FILE":"YES",
        "GCC_PREPROCESSOR_DEFINITIONS":["$(inherited)", *classic_defines, *dll_defines],
        "TEST_TARGET_NAME":APP, "ENABLE_TESTING_SEARCH_PATHS":"YES", "OTHER_LDFLAGS":["$(inherited)","-framework","XCTest"],
        "LD_RUNPATH_SEARCH_PATHS":["$(inherited)","@executable_path/Frameworks","@loader_path/Frameworks"]})
    def target(name, config, phases, dependencies, product, kind):
        return obj(name, "PBXNativeTarget", buildConfigurationList=config, buildPhases=phases,
                   buildRules=[], dependencies=dependencies, name=name, productName=name,
                   productReference=product, productType=kind)
    aid = target(APP, app_configs, [app_source_phase,resources], [], app_product, "com.apple.product-type.application")
    proxy = obj("testProxy", "PBXContainerItemProxy", containerPortal=stable_id("Project"), proxyType=1,
                remoteGlobalIDString=aid, remoteInfo=APP)
    dependency = obj("appDependency", "PBXTargetDependency", target=aid, targetProxy=proxy)
    tid = target(TEST, test_configs, [test_source_phase], [dependency], test_product,
                 "com.apple.product-type.bundle.ui-testing")
    proj = obj("Project", "PBXProject", attributes={"LastUpgradeCheck":"1600",
             "TargetAttributes":{aid:{"CreatedOnToolsVersion":"16.0"},tid:{"CreatedOnToolsVersion":"16.0","TestTargetID":aid}}},
             buildConfigurationList=project_settings, compatibilityVersion="Xcode 14.0", developmentRegion="en",
             hasScannedForEncodings=0, knownRegions=["en","Base"], mainGroup=group, productRefGroup=products,
             projectDirPath="", projectRoot="", targets=[aid,tid])
    structure = {"archiveVersion":1,"classes":{},"objectVersion":56,"objects":objects,"rootObject":proj}
    (project_dir/"project.pbxproj").write_text("// !$*UTF8*$!\n"+plist_string(structure)+"\n", encoding="utf-8")
    scheme(project_dir)


def choose_device():
    def sim_list(kind):
        data = subprocess.check_output(["xcrun","simctl","list",kind,"--json"], text=True)
        return json.loads(data)[kind]
    runtimes = [r for r in sim_list("runtimes") if r.get("isAvailable") and
                r["identifier"].startswith("com.apple.CoreSimulator.SimRuntime.iOS-")]
    runtimes.sort(key=lambda r: tuple(map(int,re.findall(r"\d+",r["version"]))),reverse=True)
    existing = sim_list("devices")
    for runtime in runtimes:
        if int(runtime["version"].split(".")[0]) < 18:
            continue
        types = [d.get("deviceTypeIdentifier","") for d in existing.get(runtime["identifier"],[])
                 if d.get("isAvailable") and "iPhone" in d.get("deviceTypeIdentifier","")]
        if not types:
            types = [d["identifier"] for d in runtime.get("supportedDeviceTypes",[]) if "iPhone" in d["identifier"]]
        if types:
            kind = next((t for t in types if t.endswith("Pro-Max")), types[0])
            return subprocess.check_output(["xcrun","simctl","create",f"Skyrim-Mod-Screens-{uuid.uuid4().hex[:8]}",
                                            kind,runtime["identifier"]],text=True).strip()
    raise SystemExit("Simulator check requires an installed, available iOS 18+ iPhone runtime")


def verify_original(container, assets, summary):
    result = json.loads(summary.read_text())
    if result.get("result") != "Passed" or result.get("passedTests",0) < 1 or result.get("failedTests",1) != 0:
        raise ValueError("The real UI test did not run to a passing conclusion")
    base = container/"Library/Application Support/SkyrimIOS"
    def contents(root):
        return {p.relative_to(root).as_posix():p.read_bytes() for p in root.rglob("*") if p.is_file()}
    assert contents(container/"Documents/Skyrim") == contents(assets/"Original")
    assert (base/"Profile/Documents/Skyrim/plugins.txt").read_bytes() == (assets/"initial-plugins.txt").read_bytes()
    assert (base/"Profile/Documents/My Games/Skyrim/Saves/Authored Save.ess").read_bytes() == (assets/"Authored Save.ess").read_bytes()
    companion = assets / "Authored Save.skse"
    if companion.is_file():
        assert (base/"Profile/Documents/My Games/Skyrim/Saves/Authored Save.skse").read_bytes() == companion.read_bytes()
    return base


def verify(container, assets, summary):
    base = verify_original(container, assets, summary)
    state = json.loads((base/"mods-state.json").read_text())
    profiles = {p["name"]:p for p in state["profiles"]}
    active = profiles["Trial Two"]
    imported = profiles["Trial Two (imported)"]
    assert active["id"] == state["next"] and active["id"] != imported["id"]
    saves = base / "Profiles" / active["id"] / "Documents/My Games/Skyrim/Saves"
    expected_saves = {"Authored Save.ess"}
    if (assets / "Authored Save.skse").is_file():
        expected_saves.add("Authored Save.skse")
    assert {item.name for item in saves.iterdir()} == expected_saves
    for name in expected_saves:
        assert (saves / name).read_bytes() == (assets / name).read_bytes()
    imported_saves = base / "Profiles" / imported["id"] / "Documents/My Games/Skyrim/Saves"
    assert not imported_saves.exists() or not any(imported_saves.iterdir())
    assert imported["packages"] == active["packages"][:-1]
    assert len(active["packages"]) == 5 and all(p["on"] for p in active["packages"])
    default_enabled = {p["id"] for p in profiles["Default"]["packages"] if p["on"]}
    assert len(default_enabled) == 2
    default_plugins = {row["name"]: row["on"] for row in profiles["Default"]["plugins"]}
    assert default_plugins["Dragonborn.esm"] is False
    assert "Dawnguard.esm" not in default_plugins and "HearthFires.esm" not in default_plugins
    assert len({p["id"] for p in imported["packages"]} - default_enabled) == 2
    exported = json.loads((container/"Documents/UI Samples/Trial Two.skyrimios-modlist.json").read_text())
    assert exported["format"]=="skyrimios-modlist" and exported["name"] == "Trial Two" and len(exported["packages"]) == 4
    view_data = base / "Views" / active["id"] / "Data"
    view = view_data / "interface/screens"
    assert (view/"shared.txt").read_bytes()==b"authored folder\n"
    assert (view/"choice.txt").read_bytes()==b"red\n"
    assert (view/"profile.txt").read_bytes()==b"authored prerequisite disabled in target profile\n"
    assert (view/"folder-choice.txt").read_bytes()==b"expanded blue\n"
    assert (view/"folder-profile.txt").read_bytes()==b"expanded target profile\n"
    assert not (view/"unselected-folder-file.txt").exists()
    assert not (view/"missing.txt").exists()
    packages = [json.loads(path.read_text()) for path in (base/"Mods").glob("*/package.json")]
    names = {"AuthoredZip", "AuthoredFolder", "AuthoredInstaller", "AuthoredFolderInstaller", "AuthoredGenerated"}
    if os.environ.get("SKYRIM_IOS_SKSE_DLL_PROBE") == "1":
        names.add("AuthoredDLL")
        assert "DLL Trial" in profiles
        verify_dll_package(base, assets)
    assert {p["name"] for p in packages} == names
    assert len(packages) == len(names) and not list((base/"Mods").glob(".staging-*"))
    generated = next(p for p in packages if p["name"] == "AuthoredGenerated")
    assert active["packages"][-1] == {"id": generated["id"], "on": True}
    ids = [p["id"] for p in active["packages"][:-1] if p["on"]]
    active_plugins = {row["name"]: row["on"] for row in active["plugins"]}
    assert active_plugins["AuthoredGenerated.esp"] and not active_plugins["AuthoredPrerequisite.esp"]
    assert generated["generatedFor"] == {
        "profile": active["id"],
        "packagesSHA256": hashlib.sha256(",".join(ids).encode()).hexdigest(),
        "pluginsSHA256": hashlib.sha256(b"authoredprerequisite.esp").hexdigest(),
    }
    for relative in (Path("Data/meshes/authored/body.nif"), Path("Data/AuthoredGenerated.esp")):
        source = assets / "Imports/AuthoredGenerated" / relative
        assert (base / "Mods" / generated["id"] / relative).read_bytes() == source.read_bytes()
        assert not (base / "Views" / active["id"] / relative).exists()
    authored_zip = next(p for p in packages if p["name"] == "AuthoredZip")
    assert authored_zip["report"]["hkx64"] == 1
    with ZipFile(assets / "Imports/AuthoredZip.zip") as source:
        for name in ("se", "le"):
            relative = f"meshes/authored/{name}.hkx"
            assert (view_data / relative).read_bytes() == source.read("Data/" + relative)
    shared = authored_zip["id"]
    assert shared in default_enabled
    assert profiles["Archive Reuse"]["packages"] == [{"id": shared, "on": False}]
    assert profiles["Archive Reuse"]["plugins"] == [
        {"name": "Dragonborn.esm", "on": False},
        {"name": "AuthoredPrerequisite.esp", "on": True},
    ]
    assert next(p for p in packages if p["name"] == "AuthoredFolderInstaller")["installer"] is True
    print("Real simulator UI result, original game/profile, conflict winner, archive and folder installers, and export verified.")


def verify_dll_package(base, assets):
    packages = [p for p in (base / "Mods").glob("*/package.json")
                if json.loads(p.read_text())["name"] == "AuthoredDLL"]
    assert len(packages) == 1
    package = packages[0]
    metadata = json.loads(package.read_text())
    assert metadata["name"] == "AuthoredDLL" and metadata["report"]["skseDlls"] == 2
    with ZipFile(assets / "Imports/AuthoredDLL.zip") as fixture:
        assert len(fixture.namelist()) == 2
        for name in fixture.namelist():
            assert (package.parent / name).read_bytes() == fixture.read(name)
    print("Installed MOD DLL diagnosis and unchanged original game, profile and imported DLL bytes verified.")


def verify_dll(container, assets, summary):
    base = verify_original(container, assets, summary)
    assert len(list((base / "Mods").glob("*/package.json"))) == 1
    verify_dll_package(base, assets)


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest="action",required=True)
    p=sub.add_parser("project")
    for arg in ("work","sdl","archive"):
        p.add_argument(arg,type=lambda s:Path(s).resolve())
    sub.add_parser("device")
    for action in ("verify", "verify-dll"):
        v=sub.add_parser(action)
        for arg in ("container","assets","summary"):
            v.add_argument(arg,type=Path)
    opts=parser.parse_args()
    if opts.action=="project":
        project(opts.work,opts.sdl,opts.archive)
    elif opts.action=="device":
        print(choose_device())
    elif opts.action=="verify":
        verify(opts.container,opts.assets,opts.summary)
    else:
        verify_dll(opts.container,opts.assets,opts.summary)
