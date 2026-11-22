"""Extract the original Classic VM class-table searches into a private fixture."""

from hashlib import sha256

from stage_effect_magnitude import PRELUDE, function


UNIT = "exe_338.c"
UNIT_SHA256 = "093abc91cefaf68bf004dcc885617f99f5535e1d6ddb28c1edbe5cb734a7d7fc"


def private_fixture(unit):
    if sha256(unit).hexdigest() != UNIT_SHA256:
        raise ValueError("VM class table unit differs from the reviewed Classic lift")
    return PRELUDE + b"".join(function(unit, address) for address in (0xC363B0, 0xC36540, 0xC365A0))
