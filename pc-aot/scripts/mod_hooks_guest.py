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

ROOT = Path(__file__).resolve().parents[1]
EXPERIMENTS = ROOT / "experiments"
SEAMS = EXPERIMENTS / "fuz-voice"

# PcGuestModHooksABI.h bits.
GROUPS = {
    "fuz_menu": 1, "fuz_world": 2, "vm_lifecycle": 4, "chargen_precache": 8,
    "camera_observe": 16, "chargen_slider": 32, "save_lifecycle": 64,
}
# Groups without a reviewed stage in this tree, with the reason shown to the caller.
UNSTAGED = {
    "vm_lifecycle": "the VM generation stage (plan X4) is not written yet",
    "camera_observe": "experiments/classic-camera has no reviewed stage of the camera call",
    "chargen_slider": "the slider call and its arguments are not confirmed from the pinned lift",
    "save_lifecycle": "the save lifecycle stage (plan P3-5) is not written yet",
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
# Same pattern as classic_skse_guest.DEFINITION, kept local so the check does
# not depend on the order in which the two preparations run.
CLASSIC_DEFINITION = re.compile(rb"^void f_(8F9720|A60200|842140|873D70)(_guest)?\(CPU \*c\) \{", re.M)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def header_hashes() -> dict[str, str]:
    return {name: digest(path.read_bytes()) for name, path in HEADERS.items()}


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
        if name in UNSTAGED:
            raise ValueError(f"MOD hook group {name} cannot be built: {UNSTAGED[name]}")
        mask |= GROUPS[name]
    if mask & GROUPS["fuz_world"] and not mask & GROUPS["fuz_menu"]:
        raise ValueError("fuz_world reuses the fuz_menu response stage; list both")
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


STAGES = {
    "fuz_menu": stage_fuz_menu,
    "fuz_world": stage_fuz_world,
    "chargen_precache": stage_chargen_precache,
}


def compose(root: Path, mask: int) -> dict[str, bytes]:
    """Run each selected stage; a unit staged by two groups must come out identical."""
    staged: dict[str, bytes] = {}
    owner: dict[str, str] = {}
    for name in group_names(mask):
        if name not in STAGES:
            raise ValueError(f"MOD hook group {name} has no reviewed stage")
        for unit, data in STAGES[name](root).items():
            if unit in staged and staged[unit] != data:
                raise ValueError(f"{unit}: the {owner[unit]} and {name} stages produce different results")
            staged.setdefault(unit, data)
            owner.setdefault(unit, name)
    return staged


def prepare_sources(sources: list[Path], destination: Path, groups: int, state: dict,
                    root: Path) -> tuple[list[Path], dict[str, str]]:
    """Return the source list with staged units replaced, and their SHA-256 by name.

    Call after classic_skse_guest.prepare_sources. A unit the Classic step
    renamed, or one that holds a Classic entry definition, is refused: the two
    preparations must not touch the same unit.
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
        if source.resolve().parent != generated:
            raise ValueError(f"{unit}: already changed by the Classic preparation")
        if CLASSIC_DEFINITION.search(source.read_bytes()) or CLASSIC_DEFINITION.search(staged[unit]):
            raise ValueError(f"{unit}: holds a Classic entry point and a MOD hook stage")
    destination.mkdir(parents=True, exist_ok=True)
    for unit, data in staged.items():
        (destination / unit).write_bytes(data)
    result = [destination / s.name if s.name in staged else s for s in sources]
    return result, {unit: digest(data) for unit, data in sorted(staged.items())}
