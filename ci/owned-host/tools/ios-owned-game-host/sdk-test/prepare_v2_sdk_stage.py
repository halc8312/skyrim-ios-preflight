"""Copy an already public SDK stage into a fresh isolated V2 test bundle.

No SDK invocation, signing, game input or mutation of the V1 stage. Framework
arguments must be the independently built/signed PUBLIC synthetic V2 fixture.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import stat

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("public_v2_stage_driver", HERE / "run_v2_sdk_tests.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)

def regular_tree(path):
    info = path.lstat()
    if stat.S_ISDIR(info.st_mode):
        for child in path.iterdir():
            regular_tree(child)
    elif not stat.S_ISREG(info.st_mode):
        raise ValueError("public stage may contain only regular files/directories")

def project_v2(text):
    changes = [
        ("        PRODUCT_BUNDLE_IDENTIFIER: com.halc8312.ownedgamehost\n",
         "        PRODUCT_BUNDLE_IDENTIFIER: " + driver.BUNDLE + "\n"),
        ("        PRODUCT_BUNDLE_IDENTIFIER: com.halc8312.ownedgamehost.tests\n",
         "        PRODUCT_BUNDLE_IDENTIFIER: " + driver.BUNDLE + ".tests\n"),
        ("    SWIFT_VERSION: \"5.0\"\n", "    SWIFT_VERSION: \"5.0\"\n"
         "    SWIFT_ACTIVE_COMPILATION_CONDITIONS: \"$(inherited) PUBLIC_V2_SDK_FIXTURE\"\n"),
        ("-Wl,-u,_OHAttachMetalLayerV1\"\n",
         "-Wl,-u,_OHAttachMetalLayerV1 -Wl,-u,_OHReadPublicV2ProofV1\"\n"),
    ]
    if "PUBLIC_V2_SDK_FIXTURE" in text:
        raise ValueError("stage already selects V2")
    for before, after in changes:
        if text.count(before) != 1:
            raise ValueError("public project anchor mismatch")
        text = text.replace(before, after)
    return text

def prepare(base, fixture, resources, output):
    base, fixture, resources = (path.resolve(strict=True) for path in (base, fixture, resources))
    output = output.resolve()
    for source in (base, fixture, resources):
        if output.is_relative_to(source) or source.is_relative_to(output):
            raise ValueError("V2 stage output must be disjoint")
    if output.exists():
        raise ValueError("V2 stage must be fresh")
    old = driver.sdk.strict_json((base / "Resources/OwnedGuestBinding.json").read_bytes())
    binding = (resources / "OwnedGuestBinding.json").read_bytes()
    new = driver.sdk.strict_json(binding)
    xex = (resources / "default.xex").read_bytes()
    if (type(old.get("abi_version")) is not int or old["abi_version"] != 0x10000
            or type(new.get("abi_version")) is not int or new["abi_version"] != 0x20000
            or old.get("xex_sha256") != new.get("xex_sha256")
            or not 24 <= len(xex) <= 1_000_000
            or hashlib.sha256(xex).hexdigest() != new.get("xex_sha256")):
        raise ValueError("separate explicit public V1/V2 fixture identities required")
    project = project_v2((base / "project.yml").read_text())
    selected = [base / name for name in ("app", "native", "include", "Libraries")]
    selected += [base / "Frameworks/WgpuNative.framework", base / "sdk-test/native", fixture, resources]
    for path in selected:
        regular_tree(path)
    if not (base / "include/PrivateGuestABIV2.h").is_file():
        raise ValueError("active public V2 ABI header missing")
    # Refuse all inputs before creating the new stage. Source trees are never edited.
    output.mkdir()
    for name in ("app", "native", "include", "Libraries"):
        shutil.copytree(base / name, output / name)
    (output / "Frameworks").mkdir()
    shutil.copytree(base / "Frameworks/WgpuNative.framework", output / "Frameworks/WgpuNative.framework")
    shutil.copytree(fixture, output / "Frameworks/PrivateGuest.framework")
    (output / "Resources/PublicFixture").mkdir(parents=True)
    (output / "Resources/OwnedGuestBinding.json").write_bytes(binding)
    (output / "Resources/PublicFixture/default.xex").write_bytes(xex)
    (output / "sdk-test/tests").mkdir(parents=True)
    shutil.copytree(base / "sdk-test/native", output / "sdk-test/native")
    shutil.copy2(HERE / "native/PublicV2SDKProbe.h", output / "sdk-test/native/PublicV2SDKProbe.h")
    shutil.copy2(HERE / "tests/OwnedHostV2FullChainTests.swift", output / "sdk-test/tests/OwnedHostV2FullChainTests.swift")
    with (output / "app/OwnedHost-Bridging-Header.h").open("a") as stream:
        stream.write('\n#include "../sdk-test/native/PublicV2SDKProbe.h"\n')
    (output / "project.yml").write_text(project, encoding="utf-8", newline="\n")
    driver.validate_stage(output)
    return output

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("base-stage", "fixture", "resources", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args(argv)
    prepare(args.base_stage, args.fixture, args.resources, args.output)
    print("PUBLIC_V2_FRESH_STAGE prepared=1 abi_version=131072 game_input=0")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
