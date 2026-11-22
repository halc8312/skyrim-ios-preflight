"""Extract the fixed player third-person update for a passive hosted witness."""

from hashlib import sha256

from stage_effect_magnitude import PRELUDE, function
from stage_scene_refresh import DEPENDENCIES as REFRESH_DEPENDENCIES, SCENE_UNITS


UNITS = (
    ("exe_132.c", "ad21bbdf90ee77aa0747db86732ae10210c736778013e4966e56f4ac15b7b0df", (0x735080,)),
    ("exe_34.c", "8a6afcb2625c22fbd9490fb4716cff3d6a9772075dd723b15a183e592d348bca", (0x4D52C0,)),
    ("exe_275.c", SCENE_UNITS[0][0], (0xAAF320, 0xAAD9A0)),
    ("exe_277.c", "8f3e127b6b84e1551c802d4278f7faed2c750233b143a270d98f64aa518918e4", (0xABDF60,)),
    *((name, digest, addresses) for name, (digest, addresses) in zip(REFRESH_DEPENDENCIES, SCENE_UNITS[1:])),
)
UNIT = UNITS[0][0]
DEPENDENCIES = tuple(name for name, _, _ in UNITS[1:])


def private_fixture(*units, runtime=False, transform=False):
    if len(units) != len(UNITS) or any(
        sha256(body).hexdigest() != digest for body, (_, digest, _) in zip(units, UNITS)
    ):
        raise ValueError("Scene witness units differ from the reviewed Classic lift")
    player = function(units[0], 0x734F50)
    start, end = b"L_734FC1:\n", b"L_734FF1:\n"
    if player.count(start) != 1 or player.count(end) != 1:
        raise ValueError("Expected the fixed player third-person update block")
    block = player[player.index(start):player.index(end) + len(end)]
    flat = function(units[3], 0xABDEC0)
    entry = b"L_ABDF23:\n"
    if flat.count(entry) != 1:
        raise ValueError("Expected the fixed existing-node transform branch")
    observed = flat.replace(entry, entry + b"    fixture_existing_flat_bone(c, ebp, esi, eax);\n")
    if runtime:
        from stage_scene_app import DIGESTS, HEADER, apply, apply_transform
        observed_units = []
        for unit, (name, _, _) in zip(units, UNITS):
            staged = apply(name, unit, unit) if name in DIGESTS else unit
            if transform and name == "exe_276.c":
                staged = apply_transform(unit, staged)
            observed_units.append(staged)
        units = observed_units
    return (HEADER if runtime else b"") + PRELUDE + b"""
void fixture_existing_flat_bone(const CPU *, uint32_t, uint32_t, uint32_t);

void fixture_update_third_person(CPU *c) {
    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3];
    uint32_t esp = c->r[4], ebp = c->r[5], esi = ecx, edi = c->r[7];
    Flags f = c->flags;
""" + block + b"    SPILL();\n}\n" + observed + b"".join(
        function(unit, address)
        for unit, (_, _, addresses) in zip(units, UNITS)
        for address in addresses
    )
