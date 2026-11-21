"""Stage review-pinned Classic fresh-save notifications without modifying the original units."""
from pathlib import Path
import sys

SAVE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SAVE_DIR.parent / "classic-package-decision"))
from stage import PRELUDE, check_unit, function_span


UNITS = {
    "exe_101.c": "8ec90cddea6c81f16123a5f9fbc7ae7f9059295712fac1383565ce2abdeb697b",
    "exe_103.c": "f6e3c6c49403e7a00b5da4f30f436123c943254ae39eae4288885f75313c4fd7",
    "exe_102.c": "51c73b53b9acd3a11be7e1994272d45dcac547c8aebe0dcd2dd8cfd6bea47e43",
    "exe_467.c": "e40f124913665af49895b591147fbaa3cc11cae5b5534aa5b6c0b89d33253769",
    "exe_198.c": "4038b313752d58d5e4273fe01f79f71f21a4b1d6b283bacbc3685bf91826d62b",
    "exe_261.c": "1f4ea50599764ad19c3bf5752598ab2036fb4b6fd8d7802774a288ab903331b5",
}
OPEN_FACTORY = b"    esp -= 4; M32(esp) = 0x681658u; SPILL(); f_677A40(c); RELOAD();\n"
OPEN_CHECK = b"    esp -= 4; M32(esp) = 0x68166Cu; SPILL(); f_677540(c); RELOAD();\n"
SLOT_CALL = b"    esp -= 4; M32(esp) = 0x6816B7u; SPILL(); f_679200(c); RELOAD();\n"
FINAL_CLOSE = b"    esp -= 4; M32(esp) = 0x6816CDu; SPILL(); f_680190(c); RELOAD();\n"
VM_CALL = b"    esp -= 4; M32(esp) = 0x8D3856u; SPILL(); f_8CD8F0(c); RELOAD();\n"
FLUSH_CALL = b"    esp -= 4; M32(esp) = 0xA579CAu; SPILL(); f_B29850(c); RELOAD();\n"
CLOSE_CALL = b"    esp -= 4; M32(esp) = 0xA579D3u; SPILL(); f_F56351(c); RELOAD();\n"
MOVE = b"    esp -= 4; M32(esp) = 0x68031Cu; SPILL(); f_F53F06(c); RELOAD();\n"

HEADER = b'#include "save_notifications.h"\n'
ROOT_ENTRY = b"\nvoid f_6814D0(CPU *c) {\n"
ROOT_EXIT = b"    esp += 16; SPILL(); return;\n"
ROOT_TICKET = b"    uint64_t skyrim_save_attempt = 0;\n"
BEGIN = b"    skyrim_save_attempt = skyrim_save_notify_begin(c, esp, M32(esp + 4u));\n"
LEAVE = (b"    esp += 16; SPILL();\n"
         b"    if (skyrim_save_attempt) skyrim_save_notify_event(c, skyrim_save_attempt, SkyrimSaveAttemptLeft, 0);\n"
         b"    return;\n")


def notify(site: str, value: str = "0") -> bytes:
    return (f"    if (skyrim_save_attempt) skyrim_save_notify_event(c, skyrim_save_attempt, "
            f"{site}, {value});\n").encode("ascii")


def around(anchor: bytes, before: bytes = b"", after: bytes = b"") -> bytes:
    prologue, call = anchor.removesuffix(b" RELOAD();\n").split(b"SPILL(); ")
    return prologue + b"SPILL();\n" + before + b"    " + call + b"\n" + after + b"    RELOAD();\n"


def nested(anchor: bytes, phase: str, after: bytes, before: bytes = b"") -> bytes:
    ticket = f"    const uint64_t skyrim_save_attempt = skyrim_save_notify_current(c, {phase});\n".encode("ascii")
    return b"    {\n" + around(anchor, ticket + before, after) + b"    }\n"


SAVE_SITES = {
    "exe_103.c": ("f_6814D0", (
        (ROOT_ENTRY, ROOT_ENTRY + ROOT_TICKET, 1),
        (OPEN_FACTORY, around(OPEN_FACTORY, before=BEGIN), 1),
        (OPEN_CHECK, around(OPEN_CHECK, after=notify("SkyrimSaveOpenFinished")), 1),
        (SLOT_CALL, around(SLOT_CALL, notify("SkyrimSaveSlotEntered"), notify("SkyrimSaveSlotLeft")), 1),
        (FINAL_CLOSE, around(FINAL_CLOSE, notify("SkyrimSaveCloseEntered"), notify("SkyrimSaveCloseLeft")), 1),
        (ROOT_EXIT, LEAVE, 3),
    )),
    "exe_198.c": ("f_8D3750", (
        (VM_CALL, nested(VM_CALL, "SkyrimSaveInSlot", notify("SkyrimSaveVmReturned", "(uint8_t)c->r[0] != 0")), 1),
    )),
    "exe_261.c": ("f_A579B0", (
        (FLUSH_CALL, nested(FLUSH_CALL, "SkyrimSaveInClose", notify("SkyrimSaveFlushReturned", "(uint8_t)c->r[0] != 0")), 1),
        (CLOSE_CALL, nested(CLOSE_CALL, "SkyrimSaveInClose", notify("SkyrimSaveCrtCloseReturned", "c->r[0] == 0")), 1),
    )),
    "exe_102.c": ("f_680190", (
        (MOVE, nested(MOVE, "SkyrimSaveInClose", notify("SkyrimSaveCrtMoveReturned", "c->r[0] == 0"),
                      before=notify("SkyrimSaveMoveEntered")), 1),
    )),
}


def stage_save_notifications(root: Path) -> dict[str, bytes]:
    staged = {}
    for name, (function, sites) in SAVE_SITES.items():
        unit = (root / "gen" / name).read_bytes()
        check_unit(unit, UNITS[name])
        start, end = function_span(unit, function)
        body = unit[start:end]
        for anchor, updated, count in sites:
            if body.count(anchor) != count:
                raise ValueError(f"{name}: expected {count} reviewed {function} notifications")
            body = body.replace(anchor, updated)
        staged[name] = PRELUDE + HEADER + unit[len(PRELUDE):start] + body + unit[end:]
    return staged
