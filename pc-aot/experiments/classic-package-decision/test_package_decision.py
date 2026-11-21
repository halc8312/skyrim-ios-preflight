"""Exercise one verified lifted site against an authored engine and subscriber."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

import stage


HERE = Path(__file__).resolve().parent
HEADERS = {
    "x86.h": "dcc654965668c5cbe3f8aaa74cd1f8fd0bb53312e8de1747e6938ffc6cd03793",
    "lift_prelude.h": "59cd9f85d2a5905c0c04dd1b5c71de0443e51dad06d19c09fc69259e6637a746",
    "rt_table.h": "eba5a14664b66a8eccfa901f289d80e6155dac0df5e72a84f24a5f641656a612",
}


def check_rejections(original: bytes, site: stage.Site) -> None:
    for changed in (
        original.replace(site.anchor, b"", 1),
        original + site.anchor,
        original.replace(site.anchor, b"", 1) + site.anchor,
        original + site.definition,
        original + b"\n",
    ):
        try:
            stage.transform(changed, site)
        except ValueError:
            continue
        raise AssertionError("A changed package site was accepted")


def lifted_fixture(unit: bytes, site: stage.Site, generation: Path) -> bytes:
    start, end = stage.function_span(unit, site.function)
    function = unit[start:end]
    if site == stage.PACKAGE_DECISION:
        callees = [b"f_40E4F0"]
    elif site == stage.PACKAGE_CHANGE:
        callees = [b"f_A50D50", b"f_A51280", b"f_8B03E0", b"f_8B04D0", b"f_8B05C0"]
        locals_end = function.index(b"\nL_8CA9B0:\n")
        body = function.index(b"\nL_8CAAE4:\n")
        finish = function.index(b"\nL_8CAB72:\n") + len(b"\nL_8CAB72:\n")
        # Enter the exact dispatch block with its registers and stack already set.
        function = function[:locals_end] + function[body:finish] + b"    SPILL();\n}\n"
    else:
        callees = [b"f_64F710", b"f_6F01B0", b"f_6F0210", b"f_6F1690", b"f_A50A30", b"f_A51C20"]
        strings = (generation / "gen" / "exe_260.c").read_bytes()
        stage.check_unit(strings, "9d9ae9b0e211456c682c9bcc78f4ae7d5a95a6f9f4f9940157cab79e21e03c73")
        start, end = stage.function_span(strings, "f_A51280")
        function += strings[start:end]
    prototypes = b"".join(b"void " + name + b"(CPU *);\n" for name in callees)
    return b'#include "lift_prelude.h"\n' + site.include + prototypes + function


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sites = {
        "decision": stage.PACKAGE_DECISION,
        "change": stage.PACKAGE_CHANGE,
        "idle": stage.IDLE_EVENT,
    }
    parser.add_argument("--generation", type=Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--site", choices=sites, default="decision")
    args = parser.parse_args()
    site = sites[args.site]
    source = args.generation / "gen" / site.unit
    original = source.read_bytes()
    check_rejections(original, site)

    private = stage.ROOT / "private"
    private.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=site.fixture + "-", dir=private) as scratch:
        out = Path(scratch)
        updated = stage.stage(args.generation, out, site).read_bytes()
        if updated.replace(site.updated, site.anchor, 1).replace(site.include, b"", 1) != original:
            raise AssertionError("The staged file changed outside the package decision")
        if source.read_bytes() != original:
            raise AssertionError("The original generated file changed")
        headers = out / "vendor"
        headers.mkdir()
        for name, digest in HEADERS.items():
            header = (args.generation / "source" / "runtime" / name).read_bytes()
            if hashlib.sha256(header).hexdigest() != digest:
                raise ValueError("Generated runtime header differs from the reviewed input: " + name)
            (headers / name).write_bytes(header)

        # Keep the lifted code in temporary private storage with authored engine services.
        lifted = out / "lifted.c"
        lifted.write_bytes(lifted_fixture(updated, site, args.generation))
        binary = out / site.fixture
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g"]
        if args.sanitizers:
            flags.extend(["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                          "-fno-omit-frame-pointer"])
        subprocess.run([args.cc, *flags, "-isystem", headers, "-I", HERE, lifted,
                        HERE / "form_view.c", HERE / (site.fixture + ".c"),
                        HERE / ("test_" + site.fixture + ".c"),
                        "-o", binary], check=True, timeout=60)
        subprocess.run([binary], check=True, timeout=20)


if __name__ == "__main__":
    main()
