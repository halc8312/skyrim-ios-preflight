#!/usr/bin/env python3
"""Offline compile the three presentation fragment variants used on an Apple GPU."""
import argparse
from pathlib import Path
import subprocess
import tempfile

SHADERS = ("dxvk_present_frag", "dxvk_present_frag_blit", "dxvk_present_frag_ms")


def tool(directory, name):
    for file in (name, name + ".exe"):
        candidate = directory / file
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError(f"{directory} does not contain {name}")


def verify(compiled, cross, metal_tools, output):
    compiled, output = compiled.resolve(), output.resolve()
    if output.exists() or output.is_relative_to(compiled):
        raise ValueError("Choose a new directory separate from the generated shader files")
    inputs = [compiled / (shader + ".spv") for shader in SHADERS]
    if not all(p.is_file() for p in inputs):
        raise ValueError("A required SPIR-V variant is missing")
    compiler, linker, nm = (tool(metal_tools, command) for command in ("metal", "metallib", "metal-nm"))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        stage = Path(temp) / "apple"
        stage.mkdir()
        air = []
        for name, binary in zip(SHADERS, inputs):
            msl, obj = name + ".metal", name + ".air"
            # Relative output names also work with Apple's Windows tools on a mounted Windows drive.
            subprocess.run([cross, binary, "--msl", "--msl-ios", "--msl-version", "30200",
                            "--rename-entry-point", "main", name, "frag", "--output", msl], cwd=stage, check=True)
            subprocess.run([compiler, "-c", "-target", "air64-apple-ios18.0", "-std=metal3.2", "-fno-fast-math",
                            msl, "-o", obj], cwd=stage, check=True)
            air.append(obj)
        library = "display-grade.metallib"
        subprocess.run([linker, *air, "-o", library], cwd=stage, check=True)
        listing = subprocess.check_output([nm, library], cwd=stage, text=True)
        exported = {ln.split()[-1] for ln in listing.splitlines() if " T " in ln}
        if exported != set(SHADERS) or not (stage / library).read_bytes().startswith(b"MTLB"):
            raise ValueError(f"Unexpected Metal exports {sorted(exported)}")
        if output.exists():
            raise FileExistsError(output)
        stage.rename(output)
        return (output / library).stat().st_size


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiled", type=Path, required=True)
    parser.add_argument("--cross", type=Path, required=True)
    parser.add_argument("--metal-tools", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        size = verify(args.compiled, args.cross.resolve(), args.metal_tools, args.output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + "\n")
    print(f"Verified three iOS 18 fragment entry points in {size} bytes")
