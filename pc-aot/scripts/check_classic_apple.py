"""Run Classic fixtures on macOS and check real Classic components for iPhoneOS."""
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
PORT = ROOT / "ios/runtime-port"
ABI = ROOT / "ios/private-abi"
CPU_PATCHES = ("0001-auxiliary-carry.patch", "0005-call-flags.patch", "0006-inline-ordered-stores.patch")


def output(*command):
    return subprocess.check_output(command, text=True).strip()


def run(*command, **options):
    subprocess.run(command, check=True, **options)


def sdl_pin(recipe):
    # The full renderer already pins this exact SDL source and revision.
    rows = re.findall(r"^checkout SDL (https://\S+) \S+ ([a-f0-9]{40})$", recipe, re.MULTILINE)
    if len(rows) != 1:
        raise ValueError("Expected one pinned SDL checkout in the renderer build")
    return rows[0]


def export_subtree(url, revision, subtree, destination, mirror):
    """Export named committed files only, ignoring edits and additions in any local mirror."""
    if not re.fullmatch(r"[a-f0-9]{40}", revision):
        raise ValueError("A full pinned Git revision is required")
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        if not (mirror / ".git").exists():
            mirror = work / "upstream.git"
            run("git", "init", "-q", "--bare", mirror)
            run("git", "-C", mirror, "fetch", "-q", "--depth=1", "--no-tags", url, revision)
        actual = output("git", "-C", mirror, "rev-parse", "--verify", revision + "^{commit}")
        if actual != revision:
            raise ValueError("Upstream revision mismatch")
        archive = work / "tree.tar"
        run("git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "-C", mirror,
            "archive", "--format=tar", "-o", archive, f"{actual}:{subtree}")
        destination.mkdir(parents=True)
        with tarfile.open(archive) as stream:
            stream.extractall(destination, filter="data")


def prepare_sources(work):
    sources = json.loads((ROOT / "source-lock.json").read_text())["sources"]
    pin = next(item for item in sources if item["name"] == "skydream")
    source, overlay = work / "source", work / "overlay"
    runtime = overlay / "runtime"
    export_subtree(pin["url"], pin["revision"], "runtime", source / "runtime", ROOT / "vendor/skydream")
    sdl_url, sdl_revision = sdl_pin((ROOT / "ios/dependencies/build-dependencies.sh").read_text())
    prefix = Path(os.environ.get("SKYRIM_IOS_DEPS_ROOT", ROOT / "local/ios-dependencies-build15"))
    export_subtree(sdl_url, sdl_revision, "include", work / "include/SDL2", prefix / "sources/SDL")
    print(f"Using public Skydream {pin['revision']} and SDL {sdl_revision}", flush=True)

    # Apply the same CPU header, late-binding and Classic adapters as build-title-host.sh.
    run(sys.executable, PORT / "prepare_runtime.py", "--source", source, "--output", runtime)
    run("git", "-C", overlay, "init", "-q")
    patches = {Path(item["path"]).name: item for item in pin["patches"]}
    for name in CPU_PATCHES:
        item = patches[name]
        patch = ROOT / item["path"]
        if hashlib.sha256(patch.read_bytes()).hexdigest() != item["sha256"]:
            raise ValueError(f"Changed runtime patch {name}")
        run("git", "-C", overlay, "apply", "--include=runtime/*", patch)
    run(sys.executable, ABI / "prepare_private_abi.py", overlay)
    run(sys.executable, PORT / "prepare_classic_skse.py", "--source", source, "--runtime", runtime)
    return source, runtime


def native_fixtures(source):
    os.environ.update(SKYRIM_SKYDREAM_SOURCE=str(source), SKYDREAM_SOURCE_DIR=str(source))
    tests = ROOT / "tests"
    suite = unittest.TestSuite()
    for pattern in ("test_classic_skse_*.py", "test_classic_cosave.py", "test_classic_skyui_trial.py"):
        group = unittest.defaultTestLoader.discover(str(tests), pattern=pattern)
        if not group.countTestCases():
            raise ValueError(f"No tests matched {pattern}")
        suite.addTests(group)
    suite.addTests(unittest.defaultTestLoader.loadTestsFromName(
        "test_game_language.GameLanguageTests.test_actual_startup_keeps_unconfigured_profile_and_atomically_updates_only_profile"))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful() or result.skipped:
        raise ValueError("Classic fixtures must all execute and pass")
    return result.testsRun


def iphone_components(work, runtime):
    sdk = output("xcrun", "--sdk", "iphoneos", "--show-sdk-path")
    clang = output("xcrun", "--sdk", "iphoneos", "--find", "clang")
    flags = [clang, "-target", "arm64-apple-ios18.0", "-isysroot", sdk, "-std=gnu11",
             "-O2", "-fno-strict-aliasing", "-fwrapv", "-fsigned-char",
             "-DGAME_SKYRIM", "-DDXVK_NATIVE", "-DSKYRIM_IOS=1", "-DSKYDREAM_MOBILE=1",
             "-DSKYRIM_IOS_GUEST_DIAGNOSTIC=1", "-DSDL_MAIN_HANDLED", "-D_FILE_OFFSET_BITS=64",
             "-DSKYRIM_IOS_CLASSIC_SKSE=1", "-DSKYDREAM_SKSE=1", "-include", PORT / "ios_build_contract.h",
             "-I", runtime, "-I", PORT, "-I", ABI, "-I", work / "include"]
    units = sorted(runtime.glob("skse_*.c"))
    units += [ABI / "pc_guest_host.c", ABI / "pc_guest_skse_host.c", ABI / "pc_guest_skse_library.c"]
    objects = []
    for unit in units:
        obj = work / (unit.stem + ".o")
        run(*flags, "-c", unit, "-o", obj)
        objects.append(obj)
    required = {"_skse_register_natives", "_f_8F9720", "_skse_gfx_install", "_skse_input_install",
                "_skse_system_run", "_skse_import_translations", "_skse_native_table", "_pc_guest_open",
                "_pc_guest_bind_classic_skse", "_skyrim_guest_bind_classic_v1"}
    definitions = set(output("xcrun", "nm", "-gjU", *objects).split())
    if missing := required - definitions:
        raise ValueError("Missing device object definitions: " + ", ".join(sorted(missing)))
    # Link self-contained production code with two independent entry points. The other
    # runtime objects use services supplied by the full host and privately assembled game.
    products = (
        ("ClassicIniFixture", [ROOT / "tests/native/test_classic_skse_ini.c", work / "skse_compat_values.o"],
         {"_skse_native_table", "_skse_native_eval"}),
        ("ClassicCosaveTool", [SCRIPTS / "classic_cosave_tool.c", PORT / "classic_cosave.c"],
         {"_skyrim_classic_cosave_visit", "_skyrim_classic_cosave_replace"}),
    )
    macho = runpy.run_path(str(SCRIPTS / "record-build.py"))["macho"]
    for name, inputs, required in products:
        product = work / name
        run(*flags, *inputs, "-Wl,-undefined,error,-no_adhoc_codesign", "-o", product)
        if macho(product.read_bytes())["signed_load_command"]:
            raise ValueError(f"Unexpected code signature in {name}")
        symbols = set(output("xcrun", "nm", "-gjU", product).splitlines())
        if not required.issubset(symbols):
            raise ValueError(f"Missing linked Classic code in {name}")
    return len(units), len(products)


def main():
    if sys.platform != "darwin":
        raise ValueError("Apple SDK checks require macOS and Xcode")
    run("xcodebuild", "-version")
    run(sys.executable, SCRIPTS / "check-public-tree.py")
    with tempfile.TemporaryDirectory(prefix="classic-apple-") as folder:
        work = Path(folder)
        source, runtime = prepare_sources(work)
        os.environ.pop("IPHONEOS_DEPLOYMENT_TARGET", None)
        os.environ["SDKROOT"] = output("xcrun", "--sdk", "macosx", "--show-sdk-path")
        os.environ["CC"] = output("xcrun", "--sdk", "macosx", "--find", "clang")
        count = native_fixtures(source)
        print(f"Executed {count} Classic macOS tests with authored data.", flush=True)
        del os.environ["SDKROOT"]
        units, links = iphone_components(work, runtime)
        print(f"Compiled {units} iPhoneOS ARM64 runtime and bridge objects. Verified {links} unsigned component links.")
        print("Full game host and physical iPhone execution are separate checks.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"Classic Apple checks failed: {error}")
