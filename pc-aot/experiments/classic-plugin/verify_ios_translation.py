"""Recreate Classic Lab's authored serialization probe from the exact Windows DLL."""
import argparse
from pathlib import Path
import re
import tempfile

from run import HERE, authored_bytes, lift_authored
from run_serialization import DLL, HASH, serial_layout


def source_snapshot(windows_work, skydream):
    data = authored_bytes(windows_work / DLL, HASH)
    with tempfile.TemporaryDirectory() as temp:
        dll, graph, generated, _ = lift_authored(data, DLL, "serial", skydream, Path(temp) / "translation")
        serial_layout(dll, graph, generated)
        exports = graph["exports"]
        expected = {"SKSEPlugin_Query": "0x101000", "SKSEPlugin_Load": "0x101050"}
        if {name: int(val, 16) for name, val in exports.items()} != {name: int(val, 16) for name, val in expected.items()}:
            raise ValueError("entry points of the fixed fixture changed")
        names = {"f_101000": "skyrim_serial_i386_query", "f_101050": "skyrim_serial_i386_load",
                 "f_1010D0": "skyrim_serial_i386_revert", "f_1010E0": "skyrim_serial_i386_save",
                 "f_101120": "skyrim_serial_i386_restore"}
        entries = {int(name[2:], 16) for name in names}
        if {int(k, 16) for k in graph["functions"]} != entries:
            raise ValueError("the authored fixture no longer has its five expected functions")
        banner = ("/* Static translation of the project's own serialization_fixture.c Windows DLL.\n"
                  f" * DLL SHA-256 {HASH}.\n"
                  " * Reproduce with verify_ios_translation.py. */\n")
        def read(name):
            original = (generated / name).read_text(encoding="ascii")
            return re.sub(r"\bf_[0-9a-fA-F]+\b", lambda m: names[m.group()], original)
        return {"serial_fixture_aot.c": banner + read("serial_00.c") + "\n" + read("serial_table.c"),
                "funcs_serial.h": banner + read("funcs_serial.h"),
                "serial_layout.h": banner + read("serial_layout.h")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("windows_work", type=Path)
    parser.add_argument("--skydream", type=Path, required=True)
    parser.add_argument("--write-snapshot", action="store_true", help="replace only the three authored source files")
    args = parser.parse_args()
    directory = HERE / "ios"
    for name, expected in source_snapshot(args.windows_work.resolve(), args.skydream.resolve()).items():
        path = directory / name
        if args.write_snapshot:
            path.write_text(expected, encoding="ascii", newline="\n")
        elif path.read_text(encoding="ascii") != expected:
            parser.exit(1, f"{name} differs from the translated, pinned Windows DLL\n")
    print("Verified five statically translated functions from the pinned authored Windows DLL.")


if __name__ == "__main__":
    main()
