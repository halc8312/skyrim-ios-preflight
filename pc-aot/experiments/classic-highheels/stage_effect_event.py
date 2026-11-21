"""Stage the exact Classic effect-event dispatch and smart-pointer bodies."""

from hashlib import sha256

from stage_effect_magnitude import PRELUDE, function


UNITS = (
    ("exe_96.c", "342c1fa3a5e90e292e228f4429a2afed1462827ed65587d5285fa5ca27fd3645", (0x656860,)),
    ("exe_23.c", "bdfa810c2cbecb365996f98ea416fda31766e9b3c0920fbee121e7da2390fd91", (0x4952A0, 0x4951F0)),
    ("exe_26.c", "065423b645414d998af80bfe7f3be8080800f0d2a0012b1d8971e5f10e22a596", (0x4A81B0,)),
    ("exe_12.c", "c3043212335f627e2674517e7610619355eef51fd376297c4e2563413f1dbb34", (0x44BB80,)),
    ("exe_01.c", "4c14091a23c8d33e06e25baca610b0bc6a237b3c5f6c594c7182ae53e798a9b1", (0x40B110,)),
    ("exe_120.c", "345a80d236dbe627eeed02503d897820ef299f87d0fbd426f342b2c076581ee5", (0x6EBC10,)),
)
UNIT = UNITS[0][0]
DEPENDENCIES = tuple(name for name, _, _ in UNITS[1:])


def private_fixture(*units):
    if len(units) != len(UNITS) or any(
        sha256(body).hexdigest() != digest for body, (_, digest, _) in zip(units, UNITS)
    ):
        raise ValueError("Effect event units differ from the reviewed Classic lift")
    return PRELUDE + b"".join(
        function(body, address)
        for body, (_, _, addresses) in zip(units, UNITS)
        for address in addresses
    )
