#!/usr/bin/env python3
"""Verify the authored tokens, or compile the probe's DXVK dump for iOS 18."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
SHADERS = {
    "two-tone": ("two_tone.hlsl", "postprocess.cpp", "shaderCode"),
    "depth-view": ("depth_view.hlsl", "depth_input.cpp", "depthViewCode"),
    "underwater-fog": ("underwater_fog.hlsl", "depth_input.cpp", "underwaterFogCode"),
}


def embedded_tokens(program="two-tone"):
    _, filename, symbol = SHADERS[program]
    source = (HERE / filename).read_text(encoding="utf-8")
    match = re.search(rf"constexpr DWORD {symbol}\[\] = \{{([^{{}}]+)\}};", source)
    if not match:
        raise ValueError("The embedded pixel shader was not found")
    return tuple(int(v.strip(), 0) for v in match[1].split(","))


def without_comments(data):
    if not data or len(data) % 4:
        raise ValueError("FXC did not return a DWORD-aligned shader")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0xffff0200:
        raise ValueError("Expected pixel shader model 2.0")
    cursor, result = 1, [words[0]]
    while cursor < len(words):
        token = words[cursor]
        if token == 0xffff:
            if cursor != len(words) - 1:
                raise ValueError("Unexpected data following the shader end")
            return (*result, token)
        comment = token & 0xffff == 0xfffe
        operands = (token >> 16) & 0x7fff if comment else (token >> 24) & 0xf
        end = cursor + 1 + operands
        if end > len(words):
            raise ValueError("Truncated shader instruction")
        if not comment:
            result.extend(words[cursor:end])
        cursor = end
    raise ValueError("Missing shader end")


def check_hlsl(fxc, program="two-tone"):
    with tempfile.TemporaryDirectory() as temp:
        output = Path(temp) / "shader.dxso"
        subprocess.run([fxc, "/nologo", "/T", "ps_2_0", "/E", "main", "/O3", "/Fo", output,
                        HERE / SHADERS[program][0]], check=True)
        tokens = without_comments(output.read_bytes())
    if tokens != embedded_tokens(program):
        raise ValueError("FXC's executable instructions differ from the embedded shader")
    print(f"Matched all {len(tokens)} executable pixel-shader DWORDs")


def metal_tools(directory, name):
    return directory / (name + (".exe" if os.name == "nt" else ""))


def check_metal(args):
    words = embedded_tokens(args.program)
    shader = struct.pack(f"<{len(words)}I", *words)
    fragment = args.dump / ("FS_" + hashlib.sha1(shader).hexdigest() + ".spv")
    vertices = list(args.dump.glob("FF_VS_*.spv"))
    if not fragment.is_file() or len(vertices) != 1:
        raise ValueError("Supply a fresh dump from this probe with its pixel and single fixed-function vertex shader")
    args.output.mkdir(parents=True, exist_ok=False)
    air_files = []
    expected = set()
    for name, stage, spv in (("fragment", "frag", fragment), ("vertex", "vert", vertices[0])):
        entry = "skyrim_postfx_" + name
        expected.add(entry)
        source, air = args.output / (name + ".metal"), args.output / (name + ".air")
        subprocess.run([args.cross, spv, "--msl", "--msl-ios", "--msl-version", "30200",
                        "--rename-entry-point", "main", entry, stage, "--output", source], check=True)
        subprocess.run([metal_tools(args.tools, "metal"), "-c", "-target", "air64-apple-ios18.0",
                        "-std=metal3.2", "-fno-fast-math", source, "-o", air], check=True)
        air_files.append(air)
    library = args.output / f"{args.program}-ios18.metallib"
    subprocess.run([metal_tools(args.tools, "metallib"), *air_files, "-o", library], check=True)
    listing = subprocess.check_output([metal_tools(args.tools, "metal-nm"), library], text=True)
    symbols = {line.split()[-1] for line in listing.splitlines() if " T " in line}
    if symbols != expected or not library.read_bytes().startswith(b"MTLB"):
        raise ValueError("The iOS metallib must contain exactly the probe's two stage entry points")
    print(f"iOS 18 metallib verified with vertex and fragment entries ({library.stat().st_size} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    hlsl = modes.add_parser("hlsl", help="Compare the embedded shader with Microsoft FXC output")
    hlsl.add_argument("--fxc", type=Path, required=True)
    hlsl.add_argument("--program", choices=SHADERS, default="two-tone")
    metal = modes.add_parser("metal", help="Use project-patched SPIRV-Cross and the Apple Metal tools")
    metal.add_argument("--dump", type=Path, required=True)
    metal.add_argument("--cross", type=Path, required=True)
    metal.add_argument("--tools", type=Path, required=True)
    metal.add_argument("--program", choices=SHADERS, default="two-tone")
    metal.add_argument("--output", type=Path, required=True, help="A new directory for MSL, AIR and the metallib")
    args = parser.parse_args()
    try:
        if args.mode == "hlsl":
            check_hlsl(args.fxc, args.program)
        else:
            check_metal(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
