"""Stage the exact Classic effect producer, dispatch and smart-pointer bodies."""

from hashlib import sha256

from stage_effect_magnitude import PRELUDE, function


UNITS = (
    ("exe_96.c", "342c1fa3a5e90e292e228f4429a2afed1462827ed65587d5285fa5ca27fd3645",
     (0x656CB0, 0x656860, 0x6554F0, 0x654E20, 0x654E60, 0x655670)),
    ("exe_23.c", "bdfa810c2cbecb365996f98ea416fda31766e9b3c0920fbee121e7da2390fd91", (0x4952A0, 0x4951F0)),
    ("exe_26.c", "065423b645414d998af80bfe7f3be8080800f0d2a0012b1d8971e5f10e22a596", (0x4A81B0,)),
    ("exe_12.c", "c3043212335f627e2674517e7610619355eef51fd376297c4e2563413f1dbb34", (0x44BB80,)),
    ("exe_01.c", "4c14091a23c8d33e06e25baca610b0bc6a237b3c5f6c594c7182ae53e798a9b1", (0x40B110,)),
    ("exe_120.c", "345a80d236dbe627eeed02503d897820ef299f87d0fbd426f342b2c076581ee5", (0x6EBC10,)),
    ("exe_98.c", "2465e7efd646217c9359f32419d3a7b523898206a76070d3e4bc919575c683c7", (0x662870,)),
    ("exe_99.c", "0edbdd17dd01bc392335927a78501a0e138dd2339d43ce2a885d18477251b023",
     (0x6691F0, 0x669220, 0x669340)),
    ("exe_112.c", "9952bd547ba9329c2f76fd611e94d58196563098f4de3eabd73349636a462115", (0x6BD6C0,)),
    ("exe_08.c", "6602c4b5fda6b8b781af7e1619e459f9c2b2643275b643c028b1b8875cedf7b5",
     (0x438740,)),
)
UNIT = UNITS[0][0]
DEPENDENCIES = tuple(name for name, _, _ in UNITS[1:])
DECLARE = b"void highheels_observe_phase(const CPU *, uint32_t, uint32_t, uint32_t, uint32_t);\n"
PRODUCER = 0x656CB0
PUBLICATION_SITES = (0x656EC5, 0x65710B)


def observe_publications(body, observe):
    for site in PUBLICATION_SITES:
        call = f"    esp -= 4; M32(esp) = 0x{site:X}u; SPILL(); f_656860(c); RELOAD();\n".encode()
        if body.count(call) != 1:
            raise ValueError("Expected one retained effect-phase publication")
        body = body.replace(call, observe(site) + call, 1)
    return body


def stage_app(unit):
    if sha256(unit).hexdigest() != UNITS[0][1]:
        raise ValueError("Effect event unit differs from the reviewed Classic lift")
    original = function(unit, PRODUCER)
    observed = observe_publications(original, lambda site: (
        "    if (skyrim_highheel_effect_guest_selected)\n"
        f"        skyrim_highheel_effect_phase(c, esi, esp, 0x{site:X}u);\n"
    ).encode())
    return b'#include "PcGuestModHooksABI.h"\n' + unit.replace(original, observed, 1)


def private_fixture(*units):
    if len(units) != len(UNITS) or any(
        sha256(body).hexdigest() != digest for body, (_, digest, _) in zip(units, UNITS)
    ):
        raise ValueError("Effect event units differ from the reviewed Classic lift")
    observed = observe_publications(
        function(units[0], PRODUCER), lambda site: f"    highheels_observe_phase(c, esi, ecx, esp, 0x{site:X}u);\n".encode()
    ).replace(b"void f_656CB0(", b"void observed_effect_producer(", 1)
    return PRELUDE + DECLARE + observed + b"".join(
        function(body, address)
        for body, (_, _, addresses) in zip(units, UNITS)
        for address in addresses
    )
