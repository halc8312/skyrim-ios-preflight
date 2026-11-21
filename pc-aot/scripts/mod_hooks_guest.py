"""Compose the reviewed MOD hook stages into a private Classic guest build copy.

Each stage is an existing experiment script that checks the SHA-256 of the
original unit and the exact insertion anchors before it changes anything.
This module only selects stages, rejects conflicting results and writes the
staged units next to the Classic-prepared sources. Nothing here reads or
writes the original generation.
"""
from __future__ import annotations

import contextlib
import hashlib
import importlib
import json
from pathlib import Path
import re
import sys

from classic_skse_guest import DEFINITION as CLASSIC_DEFINITION

ROOT = Path(__file__).resolve().parents[1]
EXPERIMENTS = ROOT / "experiments"
SEAMS = EXPERIMENTS / "fuz-voice"
SAVE_SEAMS = EXPERIMENTS / "classic-runtime-save"

# PcGuestModHooksABI.h bits.
GROUPS = {
    "fuz_menu": 1, "fuz_world": 2, "vm_lifecycle": 4, "chargen_precache": 8,
    "camera_observe": 16, "chargen_slider": 32, "save_lifecycle": 64,
    "loading_menu_observe": 128, "inventory_fit": 256, "safety_load": 512,
    "camera_render_observe": 1024,
}
# The interface header and the reviewed seam headers its tables use; the IPA
# assembly checks the same five (scripts/assemble_title_ipa.py).
HEADERS = {
    "PcGuestModHooksABI.h": ROOT / "ios/private-abi/PcGuestModHooksABI.h",
    "lift_menu_seam.h": SEAMS / "lift_menu_seam.h",
    "lift_world_seam.h": SEAMS / "lift_world_seam.h",
    "lift_voice_seam.h": SEAMS / "lift_voice_seam.h",
    "voice_fallback.h": SEAMS / "voice_fallback.h",
}
# In this pinned unit the loading dispatch body is separate from the Classic
# entry definition. Only this reviewed pair of edits can share a unit.
LOADING_MENU_UNIT = "exe_262.c"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def header_hashes() -> dict[str, str]:
    return {name: digest(path.read_bytes()) for name, path in HEADERS.items()}


# VM_LIFECYCLE (plan section 4.4). Sites, unit and hash from
# experiments/classic-fresh-load/test_fresh_load.py. Each entry: unit, unit
# SHA-256, containing function, the exact lifted call line, and the line added
# before (b) or after (a) it.
VM_HOOK_DECLARATIONS = (b"#include <stdint.h>\n"
                        b"void skyrim_vm_host_before_revert(unsigned site);\n"
                        b"void skyrim_vm_host_after_load(int succeeded);\n"
                        b"struct CPU;\n"
                        b"void skyrim_vm_host_form_deleted(struct CPU *cpu, uint32_t vm, uint64_t handle);\n")
# The sleep-unregister call in the VM's form deletion (experiments/classic-control-events).
FORM_DELETE_CALL = b"    esp -= 4; M32(esp) = 0x8D657Fu; SPILL(); f_8D4C40(c); RELOAD();\n"
FORM_DELETE_STAGED = (b"    esp -= 4; M32(esp) = 0x8D657Fu; SPILL();\n"
                      b"    {\n"
                      b"    const uint32_t skyrim_deleted_vm = c->r[1];\n"
                      b"    const uint64_t skyrim_deleted_handle = M64(c->r[4] + 4);\n"
                      b"    f_8D4C40(c);\n"
                      b"    skyrim_vm_host_form_deleted(c, skyrim_deleted_vm, skyrim_deleted_handle);\n"
                      b"    }\n"
                      b"    RELOAD();\n")
VM_UNIT_SHA256 = "f550390f7023b2375da3746cfd6b99be32bfd4fecabff2ce5396b73e2e18bf3c"
VM_SITES = [
    ("exe_199.c", VM_UNIT_SHA256, "f_8D6990",  # 0x8D69A7 load revert
     b"    esp -= 4; M32(esp) = 0x8D69ACu; SPILL(); f_8D5DF0(c); RELOAD();\n",
     "b", b"    skyrim_vm_host_before_revert(1u);\n"),
    ("exe_199.c", VM_UNIT_SHA256, "f_8D69C0",  # 0x8D6B79 VM load
     b"    esp -= 4; M32(esp) = 0x8D6B7Eu; SPILL(); f_8D3DB0(c); RELOAD();\n",
     "a", b"    skyrim_vm_host_after_load((uint8_t)c->r[0] != 0);\n"),
    ("exe_199.c", VM_UNIT_SHA256, "f_8D6550",  # 0x8D657A sleep-unregister on form deletion
     FORM_DELETE_CALL, "r", FORM_DELETE_STAGED),
]
VM_SCRIPT_RELOAD_SITE = (
    "exe_199.c", VM_UNIT_SHA256, "f_8D6D00",  # 0x8D6E16 script reload revert
    b"    esp -= 4; M32(esp) = 0x8D6E1Bu; SPILL(); f_8D5DF0(c); RELOAD();\n",
    "b", b"    skyrim_vm_host_before_revert(2u);\n",
)


def parse_groups(text: str) -> int:
    names = [part.strip() for part in text.split(",")]
    if not names or any(not name for name in names):
        raise ValueError("--mod-hooks needs a comma-separated list of hook groups")
    if len(set(names)) != len(names):
        raise ValueError("A MOD hook group is listed twice")
    mask = 0
    for name in names:
        if name not in GROUPS:
            raise ValueError(f"Unknown MOD hook group {name!r}; known groups: {', '.join(GROUPS)}")
        mask |= GROUPS[name]
    if mask & GROUPS["fuz_world"] and not mask & GROUPS["fuz_menu"]:
        raise ValueError("fuz_world reuses the fuz_menu response stage; list both")
    if mask & GROUPS["safety_load"] and not mask & GROUPS["loading_menu_observe"]:
        raise ValueError("safety_load needs loading_menu_observe; list both")
    return mask


def group_names(mask: int) -> list[str]:
    return [name for name, bit in GROUPS.items() if mask & bit]


@contextlib.contextmanager
def experiment_path(directory: Path):
    """The stage scripts import their siblings by plain module name."""
    sys.path.insert(0, str(directory))
    try:
        yield
    finally:
        sys.path.remove(str(directory))


def load(directory: Path, module: str):
    with experiment_path(directory):
        return importlib.import_module(module)


def reviewed_generation() -> dict:
    return load(SEAMS, "stage_lift").GENERATION


def validate_generation(state: dict, root: Path) -> None:
    """generation.json must be the lift the stages were reviewed against."""
    expected = reviewed_generation()
    if not isinstance(state, dict) or any(state.get(key) != value for key, value in expected.items()):
        raise ValueError("MOD hook stages require the reviewed Classic generation (fuz-voice/stage_lift.py)")
    on_disk = json.loads((root / "generation.json").read_text(encoding="utf-8"))
    if on_disk != state:
        raise ValueError("generation.json changed while preparing the MOD hook stages")


def stage_fuz_menu(root: Path) -> dict[str, bytes]:
    stages, _fixture = load(SEAMS, "stage_menu_lift").prepare_menu(root)
    return stages


def stage_fuz_world(root: Path) -> dict[str, bytes]:
    stages, _fixture = load(SEAMS, "stage_world_lift").prepare_world(root)
    return stages


def stage_chargen_precache(root: Path) -> dict[str, bytes]:
    precache = load(EXPERIMENTS / "classic-chargen-precache", "test_chargen_precache")
    name = precache.MENU_UNIT
    return {name: precache.stage_open((root / "gen" / name).read_bytes())}


def stage_chargen_slider(root: Path) -> dict[str, bytes]:
    slider = load(EXPERIMENTS / "classic-racemenu-apply", "stage_slider")
    gen = root / "gen"
    return {slider.SLIDER_UNIT: slider.stage_slider((gen / slider.SLIDER_UNIT).read_bytes(),
                                                  (gen / slider.UPDATE_UNIT).read_bytes())}


def stage_camera_observe(root: Path) -> dict[str, bytes]:
    camera = load(EXPERIMENTS / "classic-camera", "stage_camera")
    name = camera.CAMERA_UNIT
    return {name: camera.stage_camera((root / "gen" / name).read_bytes())}


def stage_camera_render_observe(root: Path) -> dict[str, bytes]:
    camera = load(EXPERIMENTS / "classic-camera", "stage_camera_render")
    name = camera.RENDER_UNIT
    return {name: camera.stage_camera_render((root / "gen" / name).read_bytes())}


def stage_loading_menu_observe(root: Path) -> dict[str, bytes]:
    loading_menu = load(EXPERIMENTS / "classic-loading-menu", "stage_loading_menu")
    return {LOADING_MENU_UNIT: loading_menu.stage_loading_menu((root / "gen" / LOADING_MENU_UNIT).read_bytes())}


def stage_inventory_fit(root: Path) -> dict[str, bytes]:
    inventory = load(EXPERIMENTS / "classic-inventory-fit", "stage_inventory_fit")
    return {inventory.UNIT: inventory.stage_inventory_fit((root / "gen" / inventory.UNIT).read_bytes(),
                                                         (root / "gen" / inventory.INVENTORY_UNIT).read_bytes())}


def stage_safety_load(root: Path) -> dict[str, bytes]:
    safety = load(EXPERIMENTS / "classic-safety-load", "stage_allocator")
    return {safety.UNIT: safety.stage_allocator((root / "gen" / safety.UNIT).read_bytes())}


def stage_save_lifecycle(root: Path) -> dict[str, bytes]:
    save = load(SAVE_SEAMS, "stage_save_notifications")
    return save.stage_save_notifications(root)


def function_span(unit: bytes, function: str) -> tuple[int, int]:
    marker = f"\nvoid {function}(CPU *c) {{\n".encode("ascii")
    if unit.count(marker) != 1:
        raise ValueError(f"Expected one original definition of {function}")
    start = unit.index(marker) + 1
    end = unit.index(b"\n}\n", start) + 3
    return start, end


def stage_vm_lifecycle(root: Path) -> dict[str, bytes]:
    sites = [*VM_SITES, VM_SCRIPT_RELOAD_SITE]
    staged: dict[str, bytes] = {}
    for unit, sha256, function, anchor, where, line in sites:
        data = staged.get(unit)
        if data is None:
            original = (root / "gen" / unit).read_bytes()
            if digest(original) != sha256:
                raise ValueError(f"{unit}: different from the reviewed Classic VM lift")
            data = VM_HOOK_DECLARATIONS + original
        if data.count(anchor) != 1:
            raise ValueError(f"{unit}: expected one reviewed VM call {anchor.strip()!r}")
        start, end = function_span(data, function)
        if not start <= data.index(anchor) < end:
            raise ValueError(f"{unit}: the reviewed VM call is not in {function}")
        replacement = {"b": line + anchor, "a": anchor + line, "r": line}[where]
        staged[unit] = data.replace(anchor, replacement, 1)
    return staged


STAGES = {
    "vm_lifecycle": stage_vm_lifecycle,
    "fuz_menu": stage_fuz_menu,
    "fuz_world": stage_fuz_world,
    "chargen_precache": stage_chargen_precache,
    "chargen_slider": stage_chargen_slider,
    "camera_observe": stage_camera_observe,
    "camera_render_observe": stage_camera_render_observe,
    "loading_menu_observe": stage_loading_menu_observe,
    "inventory_fit": stage_inventory_fit,
    "safety_load": stage_safety_load,
    "save_lifecycle": stage_save_lifecycle,
}


def compose(root: Path, mask: int) -> dict[str, bytes]:
    """Compose reviewed edits. Other shared units must produce identical output."""
    staged: dict[str, bytes] = {}
    owner: dict[str, str] = {}
    for name in group_names(mask):
        if name not in STAGES:
            raise ValueError(f"MOD hook group {name} has no reviewed stage")
        for unit, data in STAGES[name](root).items():
            if unit in staged and staged[unit] != data:
                if unit == LOADING_MENU_UNIT and owner[unit] == "loading_menu_observe" and name == "inventory_fit":
                    inventory = load(EXPERIMENTS / "classic-inventory-fit", "stage_inventory_fit")
                    staged[unit] = inventory.combine_with_observation(staged[unit], data)
                    continue
                raise ValueError(f"{unit}: the {owner[unit]} and {name} stages produce different results")
            staged.setdefault(unit, data)
            owner.setdefault(unit, name)
    return staged


def prepare_sources(sources: list[Path], destination: Path, groups: int, state: dict,
                    root: Path) -> tuple[list[Path], dict[str, str]]:
    """Return the source list with staged units replaced, and their SHA-256 by name.

    Call after classic_skse_guest.prepare_sources. Reuse the reviewed loading
    unit's Classic entry rename after checking the exact bytes. Reject any
    other overlap between these preparations.
    """
    if not groups:
        raise ValueError("No MOD hook groups selected")
    validate_generation(state, root)
    generated = (root / "gen").resolve()
    staged = compose(root, groups)
    by_name: dict[str, Path] = {}
    for source in sources:
        if source.name in by_name:
            raise ValueError(f"{source.name} appears twice in the guest sources")
        by_name[source.name] = source
    for unit in staged:
        source = by_name.get(unit)
        if source is None:
            raise ValueError(f"{unit}: staged by a MOD hook group but missing from the guest sources")
        if unit == LOADING_MENU_UNIT and groups & (GROUPS["loading_menu_observe"] | GROUPS["inventory_fit"]):
            entry = b"void f_A60200(CPU *c) {"
            renamed = b"void f_A60200_guest(CPU *c) {"
            original = (generated / unit).read_bytes()
            if (CLASSIC_DEFINITION.findall(original) != [(b"A60200", b"")]
                    or CLASSIC_DEFINITION.findall(staged[unit]) != [(b"A60200", b"")]
                    or source.read_bytes() != original.replace(entry, renamed, 1)):
                raise ValueError(f"{unit}: expected exactly the reviewed Classic entry rename")
            staged[unit] = staged[unit].replace(entry, renamed, 1)
            continue
        if source.resolve().parent != generated:
            raise ValueError(f"{unit}: already changed by the Classic preparation")
        if CLASSIC_DEFINITION.search(source.read_bytes()) or CLASSIC_DEFINITION.search(staged[unit]):
            raise ValueError(f"{unit}: holds a Classic entry point and a MOD hook stage")
    destination.mkdir(parents=True, exist_ok=True)
    for unit, data in staged.items():
        (destination / unit).write_bytes(data)
    result = [destination / s.name if s.name in staged else s for s in sources]
    return result, {unit: digest(data) for unit, data in sorted(staged.items())}
