"""Verify Classic 1.7.3 lifecycle sites and optionally instrument private copies of AOT C shards."""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OFFICIAL = {
    "Hooks_SaveLoad.cpp": "75c87c71678eaa11032cb17ecc5b83e559b68c7d00effe5eb06c36ced6c16d9f",
    "Hooks_Papyrus.cpp": "d496c6fc2a23078ad165b2c88daf7a232372cebf7108721af04ab0130b0f18df",
    "GameData.h": "9f9748b719deae96371c99673e8cf6623bb413e8e8cae3a564f130c650042a40",
    "PapyrusVM.h": "b116e52572cc51cf5a161b82a96976102d3a7e1c7ae3942bb2dd9ce7852bf2cb",
}


@dataclass(frozen=True)
class Site:
    event: int
    owner: int
    instruction: int
    target: int
    returns_bool: bool


def require(condition, message):
    if not condition:
        raise ValueError(message)


def only(matches):
    require(len(matches) == 1, "expected exactly one match")
    return matches[0]


def classic_sites(folder):
    sources = {}
    for name, expected in OFFICIAL.items():
        raw = (folder / name).read_bytes()
        require(hashlib.sha256(raw).hexdigest() == expected, "original Classic 1.7.3 source required")
        sources[name] = raw.decode("ascii", errors="replace")
    results = []
    definitions = [
        ((1,), "SaveGame_Hook", "SaveGame_HookTarget", "GameData.h", False),
        ((2,), "LoadGame_Hook", "LoadGame_HookTarget", "GameData.h", True),
        ((3,), "SaveGlobalData_Hook", "SaveRegSleepEventHandles_Internal", "PapyrusVM.h", True),
        ((4,), "LoadGlobalData_Hook", "LoadRegSleepEventHandles_Internal", "PapyrusVM.h", True),
        ((5, 6), "RevertGlobalData_Hook", "RevertGlobalData_Internal", "PapyrusVM.h", False),
        ((7,), "RegisterPapyrusFunctions_Hook", None, None, False),
    ]
    for event_ids, hook, original, header, returns_bool in definitions:
        code = sources["Hooks_SaveLoad.cpp" if event_ids[0] <= 2 else "Hooks_Papyrus.cpp"]
        if original:
            declaration = r"DEFINE_MEMBER_FN\(" + original + r",\s*\w+,\s*(0x[\da-fA-F]+)"
            target = only(re.findall(declaration, sources[header]))
        else:
            target = only(re.findall(r"RegisterPapyrusFunctions = \(_RegisterPapyrusFunctions\)(0x[\da-fA-F]+)", code))
        calls = re.findall(r"WriteRelCall\((0x[\da-fA-F]+)\s*\+\s*(0x[\da-fA-F]+),[^;\n]*?\b" + hook + r"\b[^;\n]*;", code)
        require(len(calls) == len(event_ids), "missing or ambiguous Classic call declaration")
        for event_id, (base, displacement) in zip(event_ids, calls):
            owner = int(base, 16)
            results.append(Site(event_id, owner, owner + int(displacement, 16), int(target, 16), returns_bool))
    return results


def executable_ranges(image):
    require(image[:2] == b"MZ", "unsupported image")
    pe, = struct.unpack_from("<I", image, 60)
    require(image[pe:pe + 4] == b"PE\0\0", "unsupported image")
    machine, count = struct.unpack_from("<HH", image, pe + 4)
    optional_size, = struct.unpack_from("<H", image, pe + 20)
    optional = pe + 24
    require(machine == 0x14c and 0 < count <= 96 and optional_size >= 96 and
            struct.unpack_from("<H", image, optional)[0] == 0x10b, "i386 PE32 required")
    base, = struct.unpack_from("<I", image, optional + 28)
    ranges = []
    for i in range(count):
        header = optional + optional_size + i * 40
        virtual_size, relative, size, offset = struct.unpack_from("<4I", image, header + 8)
        flags, = struct.unpack_from("<I", image, header + 36)
        if flags & 0x20000000 and size:
            require(offset + size <= len(image), "incomplete executable section")
            size = min(virtual_size or size, size)
            ranges.append((base + relative, base + relative + size, offset))
    return ranges


def check_image(image, graph, sites):
    ranges = executable_ranges(image)
    functions = {int(name, 16): blocks for name, blocks in graph["functions"].items()}
    for site in sites:
        require(site.owner in functions and site.target in functions, "guest functions absent from discovery")
        require(any(start <= site.instruction and site.instruction + 5 <= end
                    for start, end in functions[site.owner]), "call outside the discovered owner")
        start, _, offset = only([r for r in ranges if r[0] <= site.instruction and site.instruction + 5 <= r[1]])
        only([r for r in ranges if r[0] <= site.target < r[1]])
        raw = offset + site.instruction - start
        require(image[raw] == 0xe8, "official site is not a direct call in this image")
        delta, = struct.unpack_from("<i", image, raw + 1)
        require(site.instruction + 5 + delta == site.target, "official and on-disk callees differ")


def instrument(data, site):
    owner = f"void f_{site.owner:X}(CPU *c) {{".encode()
    if owner not in data:
        return data, False
    require(data.count(owner) == 1, "duplicate generated function")
    start = data.index(owner)
    end = data.find(b"\n}", start)
    require(end >= 0, "incomplete generated function")
    body = data[start:end]
    invocation = f"f_{site.target:X}(c)".encode()
    original = f"M32(esp) = 0x{site.instruction + 5:X}u; SPILL(); ".encode() + invocation + b"; RELOAD();"
    require(body.count(original) == 1, "generated return site and callee do not match uniquely")
    # At this point SPILL has stored the guest registers. The call has
    # returned and RELOAD has not yet copied r[0] (EAX) into its local.
    outcome = b"((c->r[0] & 255u) ? SkyrimReturnedTrue : SkyrimReturnedFalse)" if site.returns_bool else b"SkyrimNoResult"
    traced = b"SKYRIM_LIFECYCLE_CALL(" + str(site.event).encode() + b", " + invocation + b", " + outcome + b")"
    replacement = original.replace(invocation, traced)
    return data[:start] + body.replace(original, replacement) + data[end:], True


def build_overlay(generated, sites):
    seen = set()
    updates = {}
    owner_marks = {s.owner: f"void f_{s.owner:X}(CPU *c) {{".encode() for s in sites}
    for shard in generated.glob("exe_*.c"):
        if not re.fullmatch(r"exe_\d+\.c", shard.name):
            continue
        data = shard.read_bytes()
        wanted = [s for s in sites if owner_marks[s.owner] in data]
        if not wanted:
            continue
        for site in wanted:
            require(site.event not in seen, "multiple shards contain the same owner")
            data, found = instrument(data, site)
            require(found, "missing generated owner")
            seen.add(site.event)
        updates[shard.name] = b'#include "lifecycle.h"\n#line 1\n' + data
    require(seen == {s.event for s in sites}, "incomplete generated lifecycle coverage")
    return updates


def verify(official, image, discovery, generation, generated):
    sites = classic_sites(official)
    metadata = json.loads(generation.read_bytes())
    candidate = image.read_bytes()
    graph = discovery.read_bytes()
    require(metadata.get("source_commit") == "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a" and
            metadata.get("generation_finished") is True and metadata.get("inputs_unchanged") is True,
            "fixed completed local generation required")
    require(hashlib.sha256(candidate).hexdigest() == metadata.get("candidate_sha256") and
            hashlib.sha256(graph).hexdigest() == metadata.get("discovery_sha256"), "local generation input mismatch")
    check_image(candidate, json.loads(graph), sites)
    return sites, build_overlay(generated, sites)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("official", "image", "discovery", "generation", "generated"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--private-output", type=Path, help="new directory under pc-aot/private; omit for read-only verification")
    args = parser.parse_args()
    try:
        sites, changes = verify(args.official, args.image, args.discovery, args.generation, args.generated)
        if args.private_output:
            dest = args.private_output.resolve()
            require(dest.parent == (ROOT / "private").resolve() and dest != args.generated.resolve() and not dest.exists(),
                    "private output must be a new experiment directory")
            dest.mkdir(exist_ok=False)
            for name, data in changes.items():
                with (dest / name).open("xb") as out:
                    out.write(data)
        print(len(sites), len(changes), int(args.private_output is not None))
    except (OSError, ValueError, KeyError, TypeError, struct.error):
        # No file paths, addresses, checksums or generated source in diagnostics.
        print("0 0 0", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
