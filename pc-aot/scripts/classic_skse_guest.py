"""Prepare the pinned Skydream Classic UI/VM entry points in a private build copy."""
from pathlib import Path
import re

from prepare_title_guest import CANDIDATE_SHA, DISCOVERY_SHA, PIN

# These are the four entry points used by upstream skse_entry.c and skse_gfx.c.
# Its unrelated physics and engine overrides have no role in the Classic variant.
ENTRIES = (b"8F9720", b"A60200", b"842140", b"873D70")
DEFINITION = re.compile(rb"^void f_(" + b"|".join(ENTRIES) + rb")(_guest)?\(CPU \*c\) \{", re.M)


def validate_generation(state):
    if (state.get("source_commit") != PIN or state.get("candidate_sha256") != CANDIDATE_SHA
            or state.get("discovery_sha256") != DISCOVERY_SHA):
        raise ValueError("Classic experiment requires the reviewed 2011 title and translator inputs")


def prepare_sources(sources: list[Path], destination: Path) -> list[Path]:
    """Redirect definitions only. Direct calls and dispatch-table references keep their names."""
    found = set()
    replacements = {}
    table_checked = False
    for source in sources:
        data = source.read_bytes()
        if source.name == "exe_table.c":
            for entry in ENTRIES:
                if data.count(b"{0x" + entry + b"u, f_" + entry + b"}") != 1:
                    raise ValueError("Missing or duplicate Classic entry in generated function table")
            table_checked = True
            continue
        definitions = list(DEFINITION.finditer(data))
        if not definitions:
            continue
        for match in definitions:
            name = match.group(1)
            if name in found or match.group(2):
                raise ValueError("Expected one original definition per Classic entry")
            found.add(name)
        replacements[source] = DEFINITION.sub(rb"void f_\1_guest(CPU *c) {", data)
    if found != set(ENTRIES) or not table_checked:
        raise ValueError("Incomplete original Classic entry points or missing generated function table")
    destination.mkdir(parents=True, exist_ok=True)
    for source, data in replacements.items():
        (destination / source.name).write_bytes(data)
    return [destination / s.name if s in replacements else s for s in sources]
