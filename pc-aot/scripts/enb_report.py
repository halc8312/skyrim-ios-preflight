#!/usr/bin/env python3
"""Build and run the ENB preset report on the owner's preset folders (read-only).

    python3 scripts/enb_report.py /path/to/preset-folder [more folders...]

A preset folder holds enbseries.ini and an enbseries/ folder with the .fx files,
as in the package's Data root. Nothing is written except a temporary binary.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        print("A C compiler is required.", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "enb_report"
        subprocess.run([cc, "-std=gnu11", "-O1", "-I", str(PORT), str(ROOT / "experiments/enb-report/enb_report.c"),
                        *(str(PORT / n) for n in ("enb_ini.c", "enb_fx.c", "enb_preset.c", "mod_plugins.c")),
                        "-lm", "-o", str(binary)], check=True)
        return subprocess.run([str(binary), *argv]).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
