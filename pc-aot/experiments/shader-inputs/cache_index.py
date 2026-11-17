#!/usr/bin/env python3
"""Read-only key index for the pinned little-endian DXVK v10 state cache.

This validates record framing, checksums, key stages and packed field lengths.
It does not validate rendering semantics or authorize replay/compilation. Record
multiplicity is a count of saved recipes, never a measure of play frequency.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
MAX_BYTES = 64 << 20
MAX_ENTRIES = 65536
MAX_ENTRY_BYTES = 1024
STAGES = ((1, "VS"), (2, "TCS"), (4, "TES"), (8, "GS"), (16, "FS"), (32, "CS"))


class Invalid(ValueError):
    def __init__(self, reason, offset=0):
        self.reason, self.offset = reason, offset
        super().__init__(f"{reason} at byte {offset}")


class Reader:
    def __init__(self, data, base=0):
        self.data, self.position, self.base = data, 0, base

    def take(self, count):
        if count < 0 or count > len(self.data) - self.position:
            raise Invalid("truncated", self.base + self.position)
        start = self.position
        self.position += count
        return self.data[start:self.position]

    def unpack(self, pattern):
        return struct.unpack(pattern, self.take(struct.calcsize(pattern)))

    def need(self, condition, reason):
        if not condition:
            raise Invalid(reason, self.base + self.position)


def parse_record(payload, stage_mask, base=0):
    data = Reader(payload, base)
    data.need(0 < stage_mask < 64, "stage_mask")
    data.need(not stage_mask & 32 or stage_mask == 32, "mixed_compute_graphics")
    keys = []
    for stage, name in STAGES:
        if stage_mask & stage:
            stored_stage, = data.unpack("<I")
            data.need(stored_stage == stage, "key_stage_mismatch")
            sha1 = data.take(20).hex()
            keys.append({"stage_vk": stage, "stage": name, "sha1": sha1,
                         "dxvk_key": name + "_" + sha1})
    attributes = bindings = 0
    if stage_mask == 32:
        data.take(48)  # DxvkBindingSet<384>, twelve uint32 words.
    else:
        samples, depth_format, depth_layout = data.unpack("<BBB")
        data.need(samples in (1, 2, 4, 8, 16, 32, 64), "sample_count")
        data.need(not depth_format or depth_layout in (1, 3, 4, 0x80, 0x81), "depth_layout")
        for _ in range(8):
            image_format, layout = data.unpack("<BB")
            data.need(not image_format or layout in (1, 2), "color_layout")
        data.take(48)  # binding mask
        data.take(2)   # input assembly
        attributes, bindings = data.unpack("<BB")
        data.need(attributes <= 32 and bindings <= 32, "input_layout_counts")
        data.take(20)  # rs(4), ms(4), ds(2), om(2), stencil front/back(4+4)
        data.take(8 + 32)  # attachment swizzles, then blend records
        data.take(attributes * 4 + bindings * 4)
    constants_mask, = data.unpack("<I")
    data.need(constants_mask < (1 << 12), "specialization_mask")
    constants = {}
    for index in range(12):
        if constants_mask & (1 << index):
            value, = data.unpack("<I")
            data.need(value != 0, "noncanonical_zero_constant")
            constants[index] = value
    data.need(data.position == len(payload), "trailing_record_bytes")
    return {"keys": keys, "kind": "compute" if stage_mask == 32 else "graphics",
            "attributes": attributes, "bindings": bindings,
            "specialization_count": len(constants)}


def parse_cache(raw, max_bytes=MAX_BYTES, max_entries=MAX_ENTRIES):
    data = Reader(raw)
    data.need(0 <= len(raw) <= max_bytes, "file_size_limit")
    magic, version, entry_size = data.unpack("<4sII")
    data.need(magic == b"DXVK", "magic")
    data.need(version == 10, "unsupported_version")
    data.need(entry_size == 0, "noncanonical_header")
    records = []
    while data.position < len(raw):
        data.need(len(records) < max_entries, "entry_count_limit")
        offset = data.position
        header, = data.unpack("<I")
        mask, size = header & 0xff, header >> 8
        data.need(0 < size <= MAX_ENTRY_BYTES, "entry_size")
        checksum = data.take(20)
        payload_offset = data.position
        payload = data.take(size)
        data.need(hashlib.sha1(payload).digest() == checksum, "checksum")
        record = parse_record(payload, mask, payload_offset)
        records.append({"ordinal": len(records), "offset": offset, "payload_bytes": size,
                        "recipe_sha256": hashlib.sha256(struct.pack("<I", header) + payload).hexdigest(),
                        **record})
    key_counts = Counter(key["dxvk_key"] for record in records for key in record["keys"])
    return {"schema": 1, "status": "indexed", "format_version": 10, "upstream": UPSTREAM,
            "abi": "little-endian pinned DXVK native layout; not a portable interchange format",
            "file_bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
            "record_count": len(records), "unique_recipes": len({r["recipe_sha256"] for r in records}),
            "key_recipe_counts": dict(sorted(key_counts.items())), "records": records,
            "render_semantics_verified": False, "replay_authorized": False,
            "usage_frequency_measured": False, "game_rendering_verified": False}


def inspect(path, output):
    output = Path(output).resolve()
    # Keep the lexical private root fixed: resolving it could silently approve
    # a private/analysis junction pointing into vendor or a tracked directory.
    allowed = ROOT / "private/analysis"
    if not output.is_relative_to(allowed) or output == allowed:
        raise ValueError("Output must be a fresh child directory under private/analysis")
    output.mkdir(parents=True, exist_ok=False)
    result = {"schema": 1, "status": "rejected", "render_semantics_verified": False,
              "replay_authorized": False, "usage_frequency_measured": False,
              "game_rendering_verified": False}
    try:
        # A bounded snapshot avoids mapping an unbounded or concurrently growing file.
        with Path(path).open("rb") as stream:
            raw = stream.read(MAX_BYTES + 1)
        result = parse_cache(raw)
    except Invalid as error:
        result.update(reason=error.reason, offset=error.offset)
    except OSError as error:
        result.update(reason=type(error).__name__)
    (output / "index.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = inspect(args.cache, args.output)
    except (OSError, ValueError) as error:
        parser.exit(2, str(error) + "\n")
    print(f"State cache {result['status']}; no replay or compilation performed.")
    return 0 if result["status"] == "indexed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
