"""Authored SPIR-V: two scalar combined-image types share one binding.

Small fragment program using a helper and a specialization-selected depth
comparison. Contains no game input. Used with the real SPIRV-Cross CLI and the
Apple Metal compiler, not a mock compiler.
"""
import argparse
from pathlib import Path
import struct


def fixture(*, reverse=False, alias=True):
    words = []
    ids = {}

    def ident(name):
        if name not in ids:
            ids[name] = len(ids) + 1
        return ids[name]

    def string(s):
        raw = s.encode() + b"\0"
        raw += b"\0" * (-len(raw) % 4)
        return list(struct.unpack("<" + "I" * (len(raw) // 4), raw))

    def op(code, *args):
        values = [ident(x) if isinstance(x, str) else x for x in args]
        words.extend([(len(values) + 1) << 16 | code, *values])

    op(17, 1)  # Shader
    op(14, 0, 1)  # Logical GLSL450
    op(15, 4, "main", *string("main"), "out")
    op(16, "main", 7)  # OriginUpperLeft
    for name in ("color", "shadow", "shade"):
        op(5, name, *string(name))
    for name, binding in (("color", 0), ("shadow", 0 if alias else 1)):
        op(71, name, 34, 0)  # DescriptorSet
        op(71, name, 33, binding)
    op(71, "out", 30, 0)  # Location
    op(71, "use_shadow", 1, 7)  # SpecId
    op(19, "void")
    op(20, "bool")
    op(22, "float", 32)
    op(23, "vec2", "float", 2)
    op(23, "vec4", "float", 4)
    op(33, "fn_main", "void")
    op(33, "fn_shade", "vec4")
    op(32, "ptr_out", 3, "vec4")
    op(43, "float", "half", 0x3F000000)
    op(44, "vec2", "uv", "half", "half")
    op(49, "bool", "use_shadow")  # OpSpecConstantFalse
    for name, depth in (("shadow", 1), ("color", 0)) if reverse else (("color", 0), ("shadow", 1)):
        op(25, name + "_image", "float", 1, depth, 0, 0, 1, 0)
        op(27, name + "_combined", name + "_image")
        op(32, name + "_ptr", 0, name + "_combined")
        op(59, name + "_ptr", name, 0)
    op(59, "ptr_out", "out", 3)
    op(54, "vec4", "shade", 0, "fn_shade")
    op(248, "shade_label")
    op(61, "color_combined", "color_loaded", "color")
    op(61, "shadow_combined", "shadow_loaded", "shadow")
    op(87, "vec4", "color_result", "color_loaded", "uv")
    op(89, "float", "shadow_result", "shadow_loaded", "uv", "half")
    op(80, "vec4", "shadow_vector", *(["shadow_result"] * 4))
    op(169, "vec4", "result", "use_shadow", "shadow_vector", "color_result")
    op(254, "result")
    op(56)
    op(54, "void", "main", 0, "fn_main")
    op(248, "main_label")
    op(57, "vec4", "call", "shade")
    op(62, "out", "call")
    op(253)
    op(56)
    all_words = [0x07230203, 0x00010000, 0, len(ids) + 1, 0, *words]
    return struct.pack("<" + "I" * len(all_words), *all_words)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, options in (("color-first", {}), ("depth-first", {"reverse": True}),
                          ("distinct-bindings", {"alias": False})):
        (args.output / (name + ".spv")).write_bytes(fixture(**options))
