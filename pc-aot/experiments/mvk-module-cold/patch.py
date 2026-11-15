#!/usr/bin/env python3
"""Compatibility entry point for the module-cold candidate, now default-OFF in the build."""
import argparse
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("module_cold_adapter", ROOT / "ios/dependencies/patch_mvk_module_cold.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
NAMES = adapter.NAMES
ERROR_METHOD = adapter.ERROR_METHOD
POLICY_METHOD = adapter.POLICY_METHOD
baseline = adapter.baseline
patched = adapter.patched
apply = adapter.apply

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--pristine", type=Path, required=True)
    args = parser.parse_args()
    apply(args.source, args.pristine)
    print("Pinned module-cold adapter present; runtime default is OFF")
