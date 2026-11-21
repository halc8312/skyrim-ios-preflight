"""Extract a private fixture at the reviewed Classic task-queue return."""

from hashlib import sha256


UNIT = "exe_105.c"
UNIT_SHA256 = "fd98819fa0b94c2ac57c5b1b6be5268efd5532b0998ac2144e9486b6bc022904"
ENTRY = b"\nvoid f_6910F0(CPU *c) {\n"
RETURN = b"    esp -= 4; M32(esp) = 0x69110Bu; SPILL(); f_6A0B30(c); RELOAD();\n"
PRELUDE = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
DECLARE = b"void highheels_after_task_queue(const CPU *cpu);\n"
OBSERVE = b"    highheels_after_task_queue(c);\n"


def private_fixture(unit):
    if sha256(unit).hexdigest() != UNIT_SHA256:
        raise ValueError("Task-queue unit differs from the reviewed Classic lift")
    if unit.count(ENTRY) != 1:
        raise ValueError("Expected one reviewed task-queue caller")
    start = unit.index(ENTRY)
    end = unit.index(b"\n}\n", start) + len(b"\n}\n")
    original = unit[start:end]
    if original.count(RETURN) != 1:
        raise ValueError("Expected one reviewed ProcessTaskQueue return")
    baseline = original.replace(b"f_6910F0(", b"unobserved_task_queue(", 1)
    observed = original.replace(RETURN, RETURN + OBSERVE, 1)
    return PRELUDE + DECLARE + baseline + observed
