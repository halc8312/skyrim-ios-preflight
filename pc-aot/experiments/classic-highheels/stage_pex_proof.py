"""Extract the reviewed Store and class loader for the hosted PEX witness."""

from hashlib import sha256

from stage_effect_magnitude import PRELUDE, function


UNITS = (
    ("exe_195.c", "95fffc4dc333658997333efb875ae41e8e17ef4e81b3163be5ca2af3eaee43aa", (0x8C0810, 0x8C0730, 0x8C0970, 0x8C0C80, 0x8C0650)),
    ("exe_341.c", "e5c36dcbc65835ea7f06ac512f407d1177e0d3cb1766fc1b8fceaf3cf262d2e0", (0xC45410,)),
    ("exe_258.c", "e3d8e1dfd334a548c529ee91f7236a3a5314c433624edb43199bd678ac541ee2", (0xA44B50,)),
    ("exe_261.c", "1f4ea50599764ad19c3bf5752598ab2036fb4b6fd8d7802774a288ab903331b5", (0xA580A0,)),
    ("exe_346.c", "5cc10640ab995d0479ef16a120eb3c9b51b45b474afd6340cef0e6ee9d439e15", ()),
    ("exe_338.c", "093abc91cefaf68bf004dcc885617f99f5535e1d6ddb28c1edbe5cb734a7d7fc", ()),
)
UNIT = UNITS[0][0]
DEPENDENCIES = tuple(name for name, _, _ in UNITS[1:])


def private_fixture(*units):
    if len(units) != len(UNITS) or any(
        sha256(body).hexdigest() != digest for body, (_, digest, _) in zip(units, UNITS)
    ):
        raise ValueError("PEX witness units differ from the reviewed Classic lift")
    # The test supplies the caller's linked type and accumulated success byte.
    # Keep the final state write itself original, including the low-bit mask.
    linker = function(units[4], 0xC68D30)
    for seam in (b"0xC68E78u", b"0xC690DAu; SPILL(); f_C34F20(c); RELOAD();"):
        if linker.count(seam) != 1:
            raise ValueError("Expected one reviewed class-link site")
    finalizer = function(units[5], 0xC34F20)
    if finalizer.count(b"L_C36140:\n") != 1 or finalizer.count(b"L_C36169:\n") != 1:
        raise ValueError("Expected a reviewed class-link state write")
    start = finalizer.index(b"L_C36140:\n") + len(b"L_C36140:\n")
    stop = finalizer.index(b"    (void)logic32((uint32_t)(edi & edi), &f);", start)
    state_write = finalizer[start:stop]
    return PRELUDE + b"".join(
        function(unit, address)
        for unit, (_, _, addresses) in zip(units, UNITS)
        for address in addresses
    ) + b"""
void fixture_link_state(uint32_t type, uint32_t linker_stack) {
    uint32_t eax, ecx, edx, ebx = type, esp = linker_stack, edi;
    Flags f = {0};
""" + state_write + b"    (void)edi;\n}\n"
