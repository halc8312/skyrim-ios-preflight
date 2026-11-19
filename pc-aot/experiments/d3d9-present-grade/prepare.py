#!/usr/bin/env python3
"""Prepare an isolated copy of the pinned DXVK presentation class and four GPU shaders."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SHADERS = {
    "dxvk_present_frag": ("fsCodeCopy", "60ae8f70d9d0f765afcf298a2a0c675039c0c7e4ee0d5cc66052f1b47fdb250f"),
    "dxvk_present_frag_blit": ("fsCodeBlit", "1ac5f4d04978df3418aa01a5ebf18a0ed0e6021db74b5adbfdb36d83eb139580"),
    "dxvk_present_frag_ms": ("fsCodeResolve", "d1a7dcc107cea6314ee95de474db795cf5264ff398266f64356b6e301de6075d"),
    "dxvk_present_frag_ms_amd": ("fsCodeResolveAmd", "a904b9838d7db6c02e6197fc881bbc4860203c05f7da1487ca31e941a23aca5b"),
}
HEADER_SHA = "a9260448c77533ed0e4feef8588323f52310dd733acdc91ffc7fe8347637f9eb"
CPP_SHA = "6209390b259de9d926d3c8d64d7aee1ef38a55006c105acc34a05df76456865c"
UPSTREAM = "DxvkSwapchainBlitter"
TRIAL = "SkyrimGradeTrialBlitter"


def pinned(path, checksum):
    content = path.read_text(encoding="utf-8")
    if hashlib.sha256(content.encode("utf-8")).hexdigest() != checksum:
        raise ValueError(f"Unexpected version of {path.name}")
    return content


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one occurrence of {old!r}")
    return text.replace(old, new)


def present_shader(source, grade):
    source = once(source, "void main() {", grade + "\n\nvoid main() {")
    return once(source, "  if (s_gamma_bound) {", "  o_color.rgb = skyrimWarmDisplay(o_color.rgb);\n\n  if (s_gamma_bound) {")


def select_shader_modules(cpp):
    for shader, (variable, _) in SHADERS.items():
        original = f"#include <{shader}.h>"
        cpp = once(cpp, original, f'{original}\n#include "grade_{shader}.h"')
        cpp = once(cpp, f"const SpirvCodeBuffer {variable}({shader});",
                   f"const SpirvCodeBuffer {variable} = grade ? SpirvCodeBuffer(grade_{shader}) : SpirvCodeBuffer({shader});")
    return cpp


def staged_host(dxvk):
    cpp = pinned(dxvk / "src/dxvk/dxvk_swapchain_blitter.cpp", CPP_SHA)
    cpp = once(cpp, '#include "dxvk_swapchain_blitter.h"', '#include "dxvk_swapchain_blitter.h"\n#include <cstdlib>')
    setup = f"void {UPSTREAM}::createShaders() {{"
    cpp = once(cpp, setup, setup + '\n    const char* value = std::getenv("SKYRIM_TRIAL_PRESENT_GRADE");'
               "\n    const bool grade = value && value[0] == '1' && value[1] == 0;")
    return select_shader_modules(cpp)


def test_class(dxvk):
    base = dxvk / "src/dxvk"
    header = pinned(base / "dxvk_swapchain_blitter.h", HEADER_SHA)
    cpp = pinned(base / "dxvk_swapchain_blitter.cpp", CPP_SHA)
    # Share the pinned Gamma type. All original descriptor/layout/draw/resolve code stays the same.
    header = once(header, "  /**\n   * \\brief Gamma control point\n   */\n  struct DxvkGammaCp {\n    uint16_t r, g, b, a;\n  };", "")
    header = once(header, '#include "../dxvk/dxvk_device.h"', '#include "dxvk_swapchain_blitter.h"')
    header = header.replace(UPSTREAM, TRIAL)
    header = once(header, f"{TRIAL}(const Rc<DxvkDevice>& device);", f"{TRIAL}(const Rc<DxvkDevice>& device, bool grade);")
    header = once(header, "void createShaders();", "void createShaders(bool grade);")
    cpp = cpp.replace(UPSTREAM, TRIAL)
    cpp = once(cpp, '#include "dxvk_swapchain_blitter.h"', '#include "grade_trial_blitter.h"')
    cpp = once(cpp, f"{TRIAL}(const Rc<DxvkDevice>& device)", f"{TRIAL}(const Rc<DxvkDevice>& device, bool grade)")
    cpp = once(cpp, "this->createShaders();", "this->createShaders(grade);")
    cpp = once(cpp, "::createShaders() {", "::createShaders(bool grade) {")
    return header, select_shader_modules(cpp)


def generate(dxvk, output, compiler, validator):
    dxvk, output = dxvk.resolve(), output.resolve()
    if output.exists() or output.is_relative_to(dxvk) or output.is_relative_to(HERE):
        raise ValueError("Choose a new output directory outside both source trees")
    header, cpp = test_class(dxvk)
    grade = (HERE / "warm_grade.glsl").read_text(encoding="utf-8")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        stage = Path(temporary) / "compiled"
        stage.mkdir()
        for shader, (_, checksum) in SHADERS.items():
            raw = pinned(dxvk / "src/dxvk/shaders" / (shader + ".frag"), checksum)
            fragment = stage / (shader + ".frag")
            fragment.write_text(present_shader(raw, grade), encoding="utf-8")
            binary = stage / (shader + ".spv")
            subprocess.run([compiler, "-V", "--target-env", "vulkan1.0", "-S", "frag", "-o", binary, fragment], check=True)
            subprocess.run([validator, "--target-env", "vulkan1.0", binary], check=True)
            data = binary.read_bytes()
            if len(data) < 20 or len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
                raise ValueError("Unexpected SPIR-V module")
            tokens = [f"0x{word:08x}" for (word,) in struct.iter_unpack("<I", data)]
            lines = ["    " + ", ".join(tokens[i:i + 8]) for i in range(0, len(tokens), 8)]
            (stage / f"grade_{shader}.h").write_text(
                f"#pragma once\nstatic constexpr uint32_t grade_{shader}[] = {{\n" + ",\n".join(lines) + "\n};\n",
                encoding="ascii")
        (stage / "grade_trial_blitter.h").write_text(header, encoding="utf-8")
        (stage / "grade_trial_blitter.cpp").write_text(cpp, encoding="utf-8")
        (stage / "grade_host_blitter.cpp").write_text(staged_host(dxvk), encoding="utf-8")
        if output.exists():
            raise FileExistsError(output)
        stage.rename(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--glslang", default="glslangValidator")
    parser.add_argument("--spirv-val", default="spirv-val")
    args = parser.parse_args()
    try:
        generate(args.dxvk, args.output, args.glslang, args.spirv_val)
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        parser.exit(1, str(e) + "\n")
