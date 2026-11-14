#!/usr/bin/env python3
"""Backport MoltenVK's exact dynamic/discrete descriptor cache-key fix.

Upstream cbe497918d1d24772de4753eaa199be74fa62d07, Apache-2.0.
Only the pinned, isolated build checkout may be changed in place.
"""
import argparse
import hashlib
from pathlib import Path

REVISION = "458870543b9bcf6b0edd6f90aaa776707b310d96"
SOURCE = "MoltenVKShaderConverter/MoltenVKShaderConverter/SPIRVToMSLConverter.cpp"
SHA256 = "02645fd056c4446be7c5ceb2332ba5b0a79d875e883f658121b11e1774eb8cdc"
OLD = b'''\tfor (uint32_t dsIdx : discreteDescriptorSets) {
\t\tif ( !contains(other.discreteDescriptorSets, dsIdx)) { return false; }
\t}

    return true;
}'''
NEW = b'''\tfor (const auto& db : other.dynamicBufferDescriptors) {
\t\tif (db.stage == options.entryPointStage &&
\t\t\t!containsMatching(dynamicBufferDescriptors, db)) { return false; }
\t}

\tfor (uint32_t dsIdx : discreteDescriptorSets) {
\t\tif ( !contains(other.discreteDescriptorSets, dsIdx)) { return false; }
\t}

\tfor (uint32_t dsIdx : other.discreteDescriptorSets) {
\t\tif ( !contains(discreteDescriptorSets, dsIdx)) { return false; }
\t}

    return true;
}'''


def patched(raw):
    if hashlib.sha256(raw).hexdigest() != SHA256 or raw.count(OLD) != 1:
        raise ValueError("MoltenVK converter differs from the reviewed revision")
    return raw.replace(OLD, NEW, 1)


def original_from_patched(raw):
    if raw.count(NEW) != 1:
        raise ValueError("MoltenVK converter does not contain the reviewed backport")
    original = raw.replace(NEW, OLD, 1)
    if patched(original) != raw:
        raise ValueError("MoltenVK converter has unrelated changes")
    return original


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--in-place", action="store_true")
    args = parser.parse_args()
    raw = args.source.read_bytes()
    if args.in_place and args.output is None:
        if hashlib.sha256(raw).hexdigest() == SHA256:
            args.source.write_bytes(patched(raw))
        else:
            original_from_patched(raw)
        print("Pinned MoltenVK cache-key fix present")
    elif args.output and not args.in_place:
        with args.output.open("xb") as stream:
            stream.write(patched(raw))
    else:
        parser.error("select either a fresh output or --in-place")
