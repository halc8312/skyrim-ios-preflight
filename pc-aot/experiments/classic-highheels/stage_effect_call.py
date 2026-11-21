"""Extract the original Classic native invoke body for the caller fixture."""

from hashlib import sha256

from stage_effect_magnitude import PRELUDE, function


UNIT = "exe_341.c"
UNIT_SHA256 = "e5c36dcbc65835ea7f06ac512f407d1177e0d3cb1766fc1b8fceaf3cf262d2e0"


def private_fixture(unit):
    if sha256(unit).hexdigest() != UNIT_SHA256:
        raise ValueError("Native invoke unit differs from the reviewed Classic lift")
    return PRELUDE + function(unit, 0xC46CB0)
