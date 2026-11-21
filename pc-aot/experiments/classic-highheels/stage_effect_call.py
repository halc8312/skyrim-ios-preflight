"""Extract the original Classic native invoke body for the caller fixture."""

from hashlib import sha256

from stage_effect_magnitude import PRELUDE, function


UNIT = "exe_341.c"
UNIT_SHA256 = "e5c36dcbc65835ea7f06ac512f407d1177e0d3cb1766fc1b8fceaf3cf262d2e0"


DEPENDENCIES = ("exe_339.c",)
ARGUMENT_SHA256 = "d62bb38e7b58b6db2f9999cd6dcac6fa5cbf524f0375b250a20e101b1e7bca94"


def private_fixture(unit, arguments):
    if (sha256(unit).hexdigest() != UNIT_SHA256 or
            sha256(arguments).hexdigest() != ARGUMENT_SHA256):
        raise ValueError("Native invoke units differ from the reviewed Classic lift")
    return PRELUDE + function(unit, 0xC46CB0) + b"".join(
        function(arguments, address) for address in (0xC3A620, 0xC3A830, 0xC3AC40)
    )
