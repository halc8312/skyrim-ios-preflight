"""Apply the small public runtime declaration needed for late guest binding."""
from pathlib import Path
import argparse

def apply(source):
    path = source / "runtime/rt_table.h"
    old = b"extern const FnModule g_modules[];"
    new = b"extern FnModule g_modules[]; /* assigned once before any guest thread starts */"
    data = path.read_bytes()
    if new in data:
        return
    if data.count(old) != 1:
        raise ValueError("Unexpected module declaration")
    path.write_bytes(data.replace(old,new))

if __name__ == "__main__":
    p=argparse.ArgumentParser()
    p.add_argument("source",type=Path)
    a=p.parse_args()
    apply(a.source)
