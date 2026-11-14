#!/usr/bin/env python3
"""Borrow shader payload/metadata arguments; retain the owning member copies.

MoltenVK 4588705, Apache-2.0. Only internal C++ signatures change; the codec,
serialized data, source bytes, shader compilation and Metal APIs are untouched.
"""
import argparse
import hashlib
from pathlib import Path

PINS = {
    "MVKShaderModule.h": "54d5d158d567ea4ac73f1f1e05c35840436096c2422d42cc5f225f419a48b514",
    "MVKShaderModule.mm": "f455a0f4f364fad8071f237e54b810ba47af2f8c3a7da4acb31e10619e8722e1",
}
OLD = b"const MVKCompressor<std::string> compressedMSL"
NEW = b"const MVKCompressor<std::string>& compressedMSL"
METADATA_OLD = {
    "MVKShaderModule.h": b"const mvk::SPIRVToMSLConversionResultInfo scRslts",
    "MVKShaderModule.mm": b"const SPIRVToMSLConversionResultInfo scRslts",
}


def metadata_new(name):
    return METADATA_OLD[name].replace(b" scRslts", b"& scRslts")


def patched(name, raw):
    if name not in PINS or hashlib.sha256(raw).hexdigest() != PINS[name] or raw.count(OLD) != 2:
        raise ValueError("MoltenVK shader source differs from the pinned revision")
    if raw.count(METADATA_OLD[name]) != 1:
        raise ValueError("MoltenVK function metadata signature changed")
    return raw.replace(OLD, NEW).replace(METADATA_OLD[name], metadata_new(name))


def verify_patched(name, raw):
    if (raw.count(NEW) != 2 or raw.count(metadata_new(name)) != 1 or
            patched(name, raw.replace(NEW, OLD).replace(metadata_new(name), METADATA_OLD[name])) != raw):
        raise ValueError("MoltenVK compressed argument patch has unrelated changes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="isolated GPUObjects build directory")
    args = parser.parse_args()
    edits = {}
    for name, expected in PINS.items():
        raw = (args.source / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() == expected:
            edits[name] = patched(name, raw)
        else:
            verify_patched(name, raw)
    for name, raw in edits.items():
        (args.source / name).write_bytes(raw)
    print("Pinned MoltenVK shader payload/metadata argument copy reductions present")
