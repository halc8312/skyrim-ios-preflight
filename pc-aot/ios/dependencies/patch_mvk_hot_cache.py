#!/usr/bin/env python3
"""Shorten the pinned pipeline-cache map lock; keep cold work serialized.

No change to external synchronization, shader conversion, Metal compilation,
cache wire format, or number of concurrent cold compilers per cache.
"""
import argparse
import hashlib
from pathlib import Path

PINS = {
    "MVKPipeline.h": "b0b85d75a31ee654d5b65ff7499f179bab14b985eb72f4482d3cb1a7fc7689b2",
    "MVKPipeline.mm": "4d8d6c9bbe639d8fa78a71802d5357f91ef15568964dd8e0d570ce4ba05790d2",
}
# Exact output pins include cold-lock wait and conversion observations.
PATCHED_PINS = {'MVKPipeline.h': '6668c842747ae49abf678e418f198a499eaf5d92a50b4dd80c56cbd6a832b91f', 'MVKPipeline.mm': '3edf941c97f50a0d5d83a9164da6eb8b41c3a25230e1ae95172e43cef30378ac'}
TEMPLATE_SHA256 = "c84582187e95cf6eb8ea132dcac16c54a21830e19fbd834d87d17d2adc13f031"
TEMPLATE = Path(__file__).with_name("mvk_hot_cache.cpp.in")


def template():
    data = TEMPLATE.read_bytes().replace(b"\r\n", b"\n")
    if hashlib.sha256(data).hexdigest() != TEMPLATE_SHA256:
        raise ValueError("MoltenVK hot-cache template differs from the tested candidate")
    return data


def patched(name, raw):
    if name not in PINS or hashlib.sha256(raw).hexdigest() != PINS[name]:
        raise ValueError("MoltenVK pipeline source differs from the pinned revision")
    if name.endswith(".h"):
        anchor = b"\tstd::mutex _shaderCacheLock;"
        if raw.count(anchor) != 1: raise ValueError("cache mutex anchor changed")
        return raw.replace(anchor, anchor+b"\n\tstd::mutex _shaderCompileLock;",1)
    start = raw.index(b"MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(")
    end = raw.index(b"MVKShaderLibrary* MVKPipelineCache::getShaderLibraryImpl(",start)
    result = raw[:start]+template()+b"\n"+raw[end:]
    anchor = b"#include <sstream>"
    if result.count(anchor) != 1: raise ValueError("cache include anchor changed")
    return result.replace(anchor, anchor+b'\n#include <memory>\n#include "skyrim_performance_metrics.h"',1)


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source",type=Path,help="isolated GPUObjects build directory")
    args=parser.parse_args()
    template()
    edits={}
    for name in PINS:
        raw=(args.source/name).read_bytes()
        if hashlib.sha256(raw).hexdigest() == PATCHED_PINS[name]:continue
        data=patched(name,raw)
        if hashlib.sha256(data).hexdigest() != PATCHED_PINS[name]:
            raise ValueError("hot-cache patch output changed")
        edits[name]=data
    for name,data in edits.items():(args.source/name).write_bytes(data)
    print("Pinned MoltenVK hot-cache lock candidate present")
