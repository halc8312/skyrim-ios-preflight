"""Game-free regression for reporting empty inferred tables as unresolved."""
import hashlib
import importlib.util
import json
import multiprocessing
import os
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
PATCH = ROOT / "patches/skydream/0002-report-empty-jump-tables.patch"
HAS_DECODER = all(importlib.util.find_spec(name) is not None for name in ("capstone", "pefile"))


@unittest.skipUnless(shutil.which("git"), "Git required for pinned source patch regression")
class EmptyTableDiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = {}
        for name in ("discover.py", "peimg.py", "test_discover.py"):
            result = subprocess.run(["git", "-C", str(ROOT / "vendor/skydream"), "show", PIN + ":tools/" + name],
                                    capture_output=True)
            if result.returncode:
                raise unittest.SkipTest("Pinned Skydream source is unavailable; no source download in tests")
            cls.sources[name] = result.stdout

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)
        self.base = self.folder / "baseline"
        self.changed = self.folder / "patched"
        for root in (self.base, self.changed):
            (root / "tools").mkdir(parents=True)
            for name, data in self.sources.items():
                (root / "tools" / name).write_bytes(data)
        subprocess.run(["git", "-c", "core.autocrlf=false", "init", "--quiet"], cwd=self.changed,
                       check=True, capture_output=True)
        for flags in (["--check"], []):
            subprocess.run(["git", "-c", "core.autocrlf=false", "apply", *flags, "--", str(PATCH)],
                           cwd=self.changed, check=True, capture_output=True)
        self.exe = self.folder / "synthetic.exe"
        # Reuse the exact pinned upstream synthetic PE builder; no game bytes.
        fixture = runpy.run_path(str(self.base / "tools/test_discover.py"))
        fixture["make_pe"](self.exe)
        data = bytearray(self.exe.read_bytes())
        # Existing bounded jmp [eax*4+table] now has two non-code entries.
        struct.pack_into("<II", data, 0x500, 0, 0)
        self.exe.write_bytes(data)

    def test_patch_is_exactly_a_diagnostic_addition(self):
        before = self.sources["discover.py"]
        after = (self.changed / "tools/discover.py").read_bytes()
        addition = (b"                    if not targets:\n"
                    b"                        indirect_jumps.append(i.address)  # No inferred destination: still unresolved.\n")
        self.assertEqual(after.count(addition), 1)
        self.assertEqual(after.replace(addition, b"", 1), before)
        self.assertEqual((self.base / "tools/discover.py").read_bytes(), before)

    @unittest.skipUnless(HAS_DECODER, "Existing capstone and pefile required; tests never install them")
    def test_worker_preserves_graph_tables_and_ranges(self):
        results = []
        for root in (self.base, self.changed):
            with patch.object(sys, "argv", ["discover.py", str(self.exe), str(self.folder / "unused.json")]), \
                    patch.object(sys, "path", [str(root / "tools"), *sys.path]):
                namespace = runpy.run_path(str(root / "tools/discover.py"))
                results.append(namespace["explore_job"](0x401020))
        before, after = results
        self.assertEqual(before[:5], after[:5])  # blocks, calls, tables, ranges
        self.assertEqual(before[3][0x401025], [])
        self.assertEqual(before[4], [(0x402100, 0x402108)])
        self.assertNotIn(0x401025, before[5])
        self.assertIn(0x401025, after[5])
        self.assertEqual(set(after[5]), set(before[5]) | {0x401025})
        self.assertFalse((self.folder / "unused.json").exists())

    @unittest.skipUnless(HAS_DECODER and "fork" in multiprocessing.get_all_start_methods(),
                         "Existing decoder packages and POSIX fork required for upstream parallel discovery")
    def test_baseline_omits_site_patched_serial_and_parallel_report_it(self):
        before_hash = hashlib.sha256(self.exe.read_bytes()).hexdigest()
        runs = {}
        for label, root in (("baseline", self.base), ("patched", self.changed)):
            for jobs in (1, 2):
                output = self.folder / f"{label}-{jobs}.json"
                result = subprocess.run([sys.executable, str(root / "tools/discover.py"), str(self.exe), str(output)],
                                        env={**os.environ, "DISCOVER_JOBS": str(jobs)},
                                        capture_output=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                runs[label, jobs] = (json.loads(output.read_text()), json.loads(Path(str(output) + ".gaps").read_text()))
            self.assertEqual(runs[label, 1], runs[label, 2])
        for jobs in (1, 2):
            original, old_gaps = runs["baseline", jobs]
            changed, new_gaps = runs["patched", jobs]
            self.assertEqual(original["jump_tables"]["0x401025"], [])
            self.assertNotIn("0x401025", original["indirect_jumps"])
            self.assertIn("0x401025", changed["indirect_jumps"])
            self.assertEqual(set(changed["indirect_jumps"]), set(original["indirect_jumps"]) | {"0x401025"})
            self.assertEqual({k: v for k, v in original.items() if k != "indirect_jumps"},
                             {k: v for k, v in changed.items() if k != "indirect_jumps"})
            self.assertEqual(old_gaps, new_gaps)
        self.assertEqual(hashlib.sha256(self.exe.read_bytes()).hexdigest(), before_hash)


if __name__ == "__main__":
    unittest.main()
