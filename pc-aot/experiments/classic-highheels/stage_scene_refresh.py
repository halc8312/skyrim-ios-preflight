"""Extract the pinned Classic NiNode name lookup and direct world refresh."""

from hashlib import sha256

from stage_effect_magnitude import PRELUDE, function


UNIT = "exe_275.c"
DEPENDENCIES = ("exe_276.c", "exe_37.c", "exe_11.c", "exe_18.c")
SCENE_UNITS = (
    ("c980cb667ac15d7f1bbe6df18de61cb550dd8319719eac53e5f74da4cd33a716", (0xAAF450, 0xAAE1D0)),
    ("8aa6fb0986cb60f615cdecf9dd49545f400da8091c5449fbb51c6010caf0b116", (0xAB53E0, 0xAB5370)),
    ("d0b0da2eb3ae374fd70733a4118065365e5b2c32f2412d3c1b5cec177ad8e090", (0x4EC9C0,)),
    ("c861d31c200bce71a68a831fda43686433b26ce0b4d3efe438677cf2057d0d56", (0x447ED0,)),
    ("4d4047b63d03af6beaf39c2fb8feac4498e5350776b438a9a515e9cf4f95096b", (0x474CD0,)),
)


def private_fixture(*units):
    if len(units) != len(SCENE_UNITS) or any(
        sha256(unit).hexdigest() != digest for unit, (digest, _) in zip(units, SCENE_UNITS)
    ):
        raise ValueError("Scene refresh units differ from the reviewed Classic lift")
    return PRELUDE + b"".join(
        function(unit, address)
        for unit, (_, addresses) in zip(units, SCENE_UNITS)
        for address in addresses
    )
