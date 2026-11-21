"""Stage authenticated native load observations in the full original Classic units."""

import json
from pathlib import Path
import sys

from stage_save_notifications import SAVE_DIR, around, function_span, stage_save_and_load_notifications

sys.path.insert(0, str(SAVE_DIR.parent / "classic-fresh-load"))
from test_fresh_load import BOOL_RESULT, SLOT_INVOKE, UNIT, VM_UNIT, at_site, validate_generation
from test_slot_read import READ_CALL, SLOT_UNIT, stage_slot_read
from mod_hooks_guest import stage_vm_lifecycle


HEADER = b'#include "native_load.h"\n'
VM_LOAD = b"    esp -= 4; M32(esp) = 0x8D6CA3u; SPILL(); f_8D69C0(c); RELOAD();\n"
VM_REVERT = b"    esp -= 4; M32(esp) = 0x67D0DBu; SPILL(); f_8D6990(c); RELOAD();\n"
NATIVE_SLOT = (b"    const SkyrimLifecycleCall slot = ticket ? skyrim_lifecycle_begin(SkyrimLoadSlot)"
               b" : (SkyrimLifecycleCall){0};\n"
               b"    classic_native_load_slot_bind(ticket, c, esi);\n"
               + SLOT_INVOKE
               + b"    if (slot.ticket) skyrim_lifecycle_end(slot, " + BOOL_RESULT + b");\n")
NATIVE_READ = b"""{
        const uint32_t stream = c->r[1];
        const uint32_t frame = classic_native_load_read_begin(c, stream);
        f_677990(c);
        classic_native_load_read_observed(c, stream, frame, c->r[0]);
    }"""
APP_READ = b"""{
        const uint32_t stream = c->r[1];
        const uint64_t frame = skyrim_load_notify(c, SkyrimFreshReadBegin, 0, stream, 0);
        f_677990(c);
        if (frame) skyrim_load_notify(c, SkyrimFreshReadLeave, frame, stream, c->r[0]);
    }"""


def vm_call(anchor: bytes, app: bool = False) -> bytes:
    if app:
        return b"    {\n" + around(
            anchor,
            b"    const uint32_t entry = c->r[4];\n"
            b"    const uint64_t vm = skyrim_load_notify(c, SkyrimFreshVmBegin, 0, entry, 0);\n",
            b"    if (vm) skyrim_load_notify(c, SkyrimFreshVmLeave, vm, entry, 0);\n",
        ) + b"    }\n"
    return b"    {\n" + around(
        anchor,
        b"    const uint32_t entry = c->r[4];\n"
        b"    const SkyrimLifecycleCall vm = classic_native_load_vm_begin(c, entry);\n",
        b"    classic_native_load_vm_leave(c, vm, entry);\n",
    ) + b"    }\n"


def edit_function(unit: bytes, baseline: bytes, name: str, edit) -> bytes:
    start, end = function_span(unit, name)
    old_start, old_end = function_span(baseline, name)
    if unit[start:end] != baseline[old_start:old_end]:
        raise ValueError(f"Another stage already changed {name}")
    return unit[:start] + edit(unit[start:end]) + unit[end:]


def native_selected(body: bytes, app: bool) -> bytes:
    if app:
        for original, staged in (
            (b"classic_fresh_enter(c)", b"skyrim_load_notify(c, SkyrimFreshBegin, 0, 0, 0)"),
            (b"classic_fresh_cached(ticket, c)", b"skyrim_load_notify(c, SkyrimFreshCached, ticket, 0, 0)"),
            (b"classic_fresh_open_enter(ticket, c, esi, esp + 0x14u)",
             b"skyrim_load_notify(c, SkyrimFreshOpenEnter, ticket, esi, esp + 0x14u)"),
            (b"classic_fresh_open_leave(ticket, c, (uint8_t)c->r[0] != 0)",
             b"skyrim_load_notify(c, SkyrimFreshOpenLeave, ticket, (uint8_t)c->r[0] != 0, 0)"),
            (b"classic_fresh_slot_enter(ticket, c, esi)", b"skyrim_load_notify(c, SkyrimFreshSlotEnter, ticket, esi, 0)"),
            (b"classic_fresh_slot_leave(ticket, c, (uint8_t)c->r[0] != 0)",
             b"skyrim_load_notify(c, SkyrimFreshSlotLeave, ticket, (uint8_t)c->r[0] != 0, 0)"),
            (b"classic_fresh_leave(ticket, c, (uint8_t)eax != 0)",
             b"skyrim_load_notify(c, SkyrimFreshFinish, ticket, (uint8_t)eax != 0, 0)"),
        ):
            body = at_site(body, original, staged)
        return body
    for hook in (b"enter", b"open_enter", b"open_leave"):
        body = at_site(body, b"classic_fresh_" + hook + b"(", b"classic_native_load_" + hook + b"(")
    return at_site(body, SLOT_INVOKE, NATIVE_SLOT)


def stage_native_load(root: Path, *, app: bool = False) -> dict[str, bytes]:
    """Compose save/epoch notifications without replacing any lifted body."""
    state = json.loads((root / "generation.json").read_bytes())
    validate_generation(state)
    if (state.get("mode") != "original-guest-bodies" or state.get("fixed_native_replacements") is not False or
            state.get("generation_finished") is not True or state.get("inputs_unchanged") is not True):
        raise ValueError("Requires a completed, unchanged original-body generation")

    originals = {name: (root / "gen" / name).read_bytes() for name in (SLOT_UNIT, VM_UNIT)}
    staged = stage_save_and_load_notifications(root)
    vm = stage_vm_lifecycle(root)
    if staged.keys() & vm.keys():
        raise ValueError("Save and VM notification stages overlap")
    staged.update(vm)
    # The selected function has exactly the initial fresh-load observations at this point.
    start, end = function_span(staged[UNIT], "f_6821C0")
    staged[UNIT] = staged[UNIT][:start] + native_selected(staged[UNIT][start:end], app) + staged[UNIT][end:]
    if app:
        staged[UNIT] = staged[UNIT].removeprefix(b'#include "fresh_load.h"\n')

    _, read, _ = stage_slot_read(originals[SLOT_UNIT])
    staged[SLOT_UNIT] = edit_function(staged[SLOT_UNIT], originals[SLOT_UNIT], "f_67B720",
        lambda body: at_site(body, read, at_site(read, READ_CALL, APP_READ if app else NATIVE_READ)))
    for unit, function, anchor in ((SLOT_UNIT, "f_67CF20", VM_REVERT), (VM_UNIT, "f_8D6C80", VM_LOAD)):
        staged[unit] = edit_function(staged[unit], originals[unit], function,
                                     lambda body: at_site(body, anchor, vm_call(anchor, app)))
    for unit in (UNIT, SLOT_UNIT, VM_UNIT):
        staged[unit] = (b'#include "PcGuestModHooksABI.h"\n' if app else HEADER) + staged[unit]
    return staged
