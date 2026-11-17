#!/usr/bin/env python3
"""Two mutually exclusive host trials applied only to a separate production stage."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = ROOT / "vendor/skydream/third_party/dxvk"
LOCK = "src/util/sync/sync_spinlock.h"
CS = "src/dxvk/dxvk_cs.cpp"
QOS = "src/dxvk/host_cs_qos.h"
WAIT = "src/util/sync/host_lock_wait.h"
MODES = ("unfair-lock", "cs-qos")
PINNED = {
    LOCK: "db4d1c12cfccdf565eb3234f50afb5a9dbe48a0c46df1d2a447687bb061751e7",
    CS: "cea284ce41eeaac5ac4fde0e2eb6f1ffcea06623da61c859e6627d655d6c94af",
}
GUARDS = ("src/dxvk/dxvk_graphics.h", "src/dxvk/dxvk_graphics.cpp")
spec = importlib.util.spec_from_file_location("host_wait_production_stage", ROOT / "ios/dependencies/prepare_dxvk.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def original(name: str) -> str:
    path = SOURCE / name
    if path.is_symlink() or not path.is_file():
        raise ValueError("Missing or linked pinned host-trial source")
    text = path.read_text(encoding="utf-8")
    expected = PINNED[name] if name in PINNED else adapter.EXPECTED[name]
    if hashlib.sha256(text.encode()).hexdigest() != expected:
        raise ValueError("Pinned host-trial source fingerprint changed: " + name)
    return text


def baseline(name: str) -> str:
    text = original(name)
    return adapter.adapt(name, text) if name in adapter.EXPECTED else text


def once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise ValueError("Host-trial anchor changed or is ambiguous")
    return text.replace(old, new, 1)


def candidate(mode: str) -> str:
    if mode == "unfair-lock":
        text = baseline(LOCK)
        text = once(text, "#include <atomic>", "#include <atomic>\n#if defined(__APPLE__)\n#include <os/lock.h>\n#include \"host_lock_wait.h\"\n#endif")
        begin = text.index("  class Spinlock {")
        end = text.index("\n  };", begin) + len("\n  };")
        old_class = text[begin:end]
        trial = (HERE / "apple_spinlock.in").read_text(encoding="utf-8").rstrip()
        return once(text, old_class, "#if defined(__APPLE__)\n" + trial + "\n#else\n" + old_class + "\n#endif")
    if mode == "cs-qos":
        text = baseline(CS)
        text = once(text, '    env::setThreadName("dxvk-cs");',
                    '    env::setThreadName("dxvk-cs");\n    skyrim::host_wait::csQosStartup();')
        return '#include "host_cs_qos.h"\n' + text
    raise ValueError("Choose exactly one host-trial mode")


def patched(mode: str, text: str) -> str:
    if mode not in MODES:
        raise ValueError("Choose exactly one host-trial mode")
    old = baseline(LOCK if mode == "unfair-lock" else CS)
    new = candidate(mode)
    if text not in (old, new):
        raise ValueError("Host trial requires the exact production-stage baseline")
    return new


def safe_file(stage: Path, name: str, *, optional=False) -> Path:
    path = stage / name
    if any(part.is_symlink() for part in (path, *path.parents) if part != stage and stage in part.parents):
        raise ValueError("Linked staged host-trial path")
    if stage not in path.resolve().parents or (not optional and not path.is_file()):
        raise ValueError("Missing or escaped staged host-trial path")
    if path.exists() and not path.is_file():
        raise ValueError("Host-trial target is not a regular file")
    return path


def apply(stage: Path, mode: str) -> None:
    if mode not in MODES:
        raise ValueError("Choose exactly one host-trial mode")
    stage = stage.resolve(strict=True)
    # Keep the allowlisted private prefix lexical: a local/ symlink must never
    # turn a resolved production/vendor target into an allowed destination.
    private = ROOT / "local"
    if stage == ROOT or stage in ROOT.parents or (ROOT in stage.parents and stage != private and private not in stage.parents):
        raise ValueError("Use a separate staged tree, never production/vendor source")
    target_name = LOCK if mode == "unfair-lock" else CS
    other_name = CS if mode == "unfair-lock" else LOCK
    target = safe_file(stage, target_name)
    other = safe_file(stage, other_name)
    if other.read_text(encoding="utf-8") != baseline(other_name):
        raise ValueError("Host trials are mutually exclusive; use a fresh production stage")
    # Also reject composition with the earlier graphics-flight experiment.
    for name in GUARDS:
        if safe_file(stage, name).read_text(encoding="utf-8") != baseline(name):
            raise ValueError("Host trial requires an uncombined production stage")
    output = {target: patched(mode, target.read_text(encoding="utf-8"))}
    support = safe_file(stage, QOS, optional=True)
    wait_support = safe_file(stage, WAIT, optional=True)
    if mode == "cs-qos":
        if wait_support.exists():
            raise ValueError("Lock observer present: host trials must remain independent")
        expected = (HERE / "host_cs_qos.h").read_text(encoding="utf-8")
        if support.exists() and support.read_text(encoding="utf-8") != expected:
            raise ValueError("Unrecognized staged QoS helper")
        output[support] = expected
    else:
        if support.exists():
            raise ValueError("QoS helper present: host trials must remain independent")
        expected = (HERE / "host_lock_wait.h").read_text(encoding="utf-8")
        if wait_support.exists() and wait_support.read_text(encoding="utf-8") != expected:
            raise ValueError("Unrecognized staged lock observer")
        output[wait_support] = expected
    # Every input and destination is validated before the first write.
    for path, text in output.items():
        path.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=MODES)
    parser.add_argument("stage", type=Path)
    args = parser.parse_args()
    apply(args.stage, args.mode)
    print("Applied isolated " + args.mode + " host trial; production and iPhone verification unchanged")
