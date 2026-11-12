"""Synthetic tests for incomplete, guarded negative-table candidate recovery."""
import importlib.util
import copy
import io
import json
import multiprocessing
import mmap
import os
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
PATCHES = [ROOT / "patches/skydream" / name for name in (
    "0001-auxiliary-carry.patch", "0002-report-empty-jump-tables.patch",
    "0003-negative-index-candidate-roots.patch")]
HAS_DECODER = all(importlib.util.find_spec(name) is not None for name in ("capstone", "pefile"))
BASE = 0x400000
SITE = 0x401040
TARGETS = {0x401080, 0x4010A0, 0x4010C0, 0x4010E0}


class SourceFixture:
    @classmethod
    def setUpClass(cls):
        if not shutil.which("git"):
            raise unittest.SkipTest("Git required")
        result = subprocess.run(["git", "-c", "core.autocrlf=false", "-C", str(ROOT / "vendor/skydream"),
                                 "archive", PIN, "tools/discover.py", "tools/peimg.py", "tools/test_discover.py",
                                 "tools/lift.py", "runtime/x86.h"], capture_output=True)
        if result.returncode:
            raise unittest.SkipTest("Pinned source unavailable; no test downloads")
        cls.source_tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.source_tmp.cleanup)
        cls.base = Path(cls.source_tmp.name) / "base"
        cls.changed = Path(cls.source_tmp.name) / "changed"
        for directory, patches in ((cls.base, PATCHES[:2]), (cls.changed, PATCHES)):
            directory.mkdir()
            with tarfile.open(fileobj=io.BytesIO(result.stdout)) as archive:
                archive.extractall(directory, filter="data")
            subprocess.run(["git", "-c", "core.autocrlf=false", "init", "--quiet"], cwd=directory,
                           check=True, capture_output=True)
            for path in patches:
                for flags in (["--check"], []):
                    subprocess.run(["git", "-c", "core.autocrlf=false", "apply", *flags, "--", str(path)],
                                   cwd=directory, check=True, capture_output=True)


class NegativePatchApplicationTests(SourceFixture, unittest.TestCase):
    def test_patch_stack_applies_only_to_private_archive(self):
        self.assertNotIn(b"def negative_table_candidate_roots", (self.base / "tools/discover.py").read_bytes())
        self.assertIn(b"def negative_table_candidate_roots", (self.changed / "tools/discover.py").read_bytes())
        self.assertEqual((self.base / "tools/peimg.py").read_bytes(), (self.changed / "tools/peimg.py").read_bytes())


@unittest.skipUnless(HAS_DECODER, "Existing capstone/pefile required; no installation in tests")
class NegativeTableCandidateTests(SourceFixture, unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.exe = self.folder / "fixture.exe"
        runpy.run_path(str(self.base / "tools/test_discover.py"))["make_pe"](self.exe)
        self.data = bytearray(self.exe.read_bytes())
        self.data[0x200:0x400] = b"\xcc" * 0x200
        self.emitted = {}
        self.emit(0x20, b"\x83\xe9\x04\x72\x1b\xc3")  # sub ecx,4; jb dispatch; ret
        self.emit(0x40, b"\xff\x24\x8d" + struct.pack("<I", 0x402110))
        for offset in (0x80, 0xA0, 0xC0, 0xE0):
            self.emit(offset, b"\xb8" + struct.pack("<I", offset) + b"\xc3")
        struct.pack_into("<4I", self.data, 0x500, *sorted(TARGETS))
        struct.pack_into("<I", self.data, 0x510, 0)  # positive index zero is invalid
        struct.pack_into("<I", self.data, 0x98 + 16, 0x1020)

    def emit(self, offset, code):
        self.data[0x200 + offset:0x200 + offset + len(code)] = code
        self.emitted[offset] = code

    def namespace(self, root=None):
        self.exe.write_bytes(self.data)
        root = root or self.changed
        with patch.object(sys, "argv", ["discover.py", str(self.exe), str(self.folder / "unused.json")]), \
                patch.object(sys, "path", [str(root / "tools"), *sys.path]):
            namespace = runpy.run_path(str(root / "tools/discover.py"))
        return namespace["explore_function"].__globals__

    def candidates(self, start=0x401020, include=None, positive_targets=(), mutate=None):
        ns = self.namespace()
        seen = {}
        # Explicit synthetic graph for adversarial local incoming-edge checks.
        for offset, code in self.emitted.items():
            if include is not None and offset not in include:
                continue
            for insn in ns["md"].disasm(code, 0x401000 + offset):
                seen[insn.address] = insn.size
        ns["jump_tables"] = {SITE: list(positive_targets)}
        ns["jt_ranges"] = [(0x402110, 0x402114)]
        ns["indirect_jumps"] = []
        if mutate:
            mutate(ns)
        tables = {k: list(v) for k, v in ns["jump_tables"].items()}
        ranges = list(ns["jt_ranges"])
        found = ns["negative_table_candidate_roots"](start, seen)
        self.assertEqual(ns["jump_tables"], tables)
        self.assertEqual(ns["jt_ranges"], ranges)
        return found, ns

    def test_wrapped_negative_indexes_add_candidates_but_never_resolve_dispatch(self):
        roots, ns = self.candidates()
        self.assertEqual(roots, TARGETS)
        self.assertIn(SITE, ns["indirect_jumps"])
        # Even unrelated positive entries cannot make a negative dispatch complete.
        roots, ns = self.candidates(positive_targets=[0x401080])
        self.assertEqual(roots, TARGETS)
        self.assertIn(SITE, ns["indirect_jumps"])

    def test_actual_exploration_keeps_blocks_tables_and_ranges_only_augments_roots(self):
        before = self.namespace(self.base)["explore_job"](0x401020)
        after = self.namespace()["explore_job"](0x401020)
        self.assertEqual(before[1], after[1])
        self.assertEqual(before[3:5], after[3:5])
        self.assertEqual(set(after[2]), set(before[2]) | TARGETS)
        self.assertIn(SITE, before[5])
        self.assertIn(SITE, after[5])

    def test_flags_and_register_clobbers_partial_width_wrong_index_and_signed_immediates_rejected(self):
        cases = [b"\x83\xe9\x04\x31\xc0\x72\x19\xc3",  # flags clobber
                 b"\x83\xe9\x04\xb9\0\0\0\0\x72\x16\xc3",  # register clobber
                 b"\x83\xe9\x04\xe8\0\0\0\0\x72\x16\xc3",  # call between SUB/JB
                 b"\x66\x83\xe9\x04\x72\x1a\xc3",  # 16-bit SUB
                 b"\x83\xe9\xff\x72\x1b\xc3",  # sign-extended -1
                 b"\x83\xe9\x00\x72\x1b\xc3", b"\x83\xe9\x21\x72\x1b\xc3"]
        for code in cases:
            with self.subTest(code=code.hex()):
                self.emit(0x20, code)
                self.assertEqual(self.candidates()[0], set())
        self.emit(0x20, b"\x83\xe9\x04\x72\x1b\xc3")
        for jump in (b"\xff\x24\x85" + struct.pack("<I", 0x402110),
                     b"\x64\xff\x24\x8d" + struct.pack("<I", 0x402110)):
            self.emit(0x40, jump)
            self.assertEqual(self.candidates()[0], set())

    def test_alternative_dispatch_predecessor_and_sub_bypass_rejected(self):
        for target in (0x40, 0x23):
            with self.subTest(target=target):
                self.emit(0x60, b"\xe9" + struct.pack("<i", target - 0x65))
                roots, ns = self.candidates()
                self.assertEqual(roots, set())
                self.assertIn(SITE, ns["indirect_jumps"])

    def test_call_and_root_entries_rejected(self):
        for target in (0x40, 0x23):
            self.emit(0x60, b"\xe8" + struct.pack("<i", target - 0x65) + b"\xc3")
            self.assertEqual(self.candidates()[0], set())
        self.emitted.pop(0x60)
        for start in (SITE, 0x401023):
            self.assertEqual(self.candidates(start=start)[0], set())

    def test_conflicting_predecessor_and_iteration_order_do_not_establish_bound(self):
        self.emit(0x60, b"\x83\xe9\x02\x72\xdb\xc3")  # another guard -> dispatch
        self.assertEqual(self.candidates()[0], set())
        self.emitted = dict(reversed(list(self.emitted.items())))
        self.assertEqual(self.candidates()[0], set())

    def test_invalid_target_unmapped_or_non_file_backed_slots_and_underflow_rejected(self):
        for value in (0, 0x403100, 0x401081):  # unmapped / interior of known instruction
            struct.pack_into("<I", self.data, 0x500, value)
            self.assertEqual(self.candidates()[0], set())
        struct.pack_into("<I", self.data, 0x500, 0x401080)
        # Keep virtual mapping, but table bytes fall outside file-backed section size.
        rdata_header = 0x98 + 224 + 40
        struct.pack_into("<I", self.data, rdata_header + 16, 0x100)
        self.assertEqual(self.candidates()[0], set())
        struct.pack_into("<I", self.data, rdata_header + 16, 0x400)
        for displacement in (8, 0x500000):
            self.emit(0x40, b"\xff\x24\x8d" + struct.pack("<I", displacement))
            self.assertEqual(self.candidates()[0], set())

    def test_invalid_one_of_four_entries_does_not_emit_partial_candidates(self):
        struct.pack_into("<I", self.data, 0x50C, 0)
        roots, ns = self.candidates()
        self.assertEqual(roots, set())
        self.assertIn(SITE, ns["indirect_jumps"])

    def test_actual_raw_backing_and_unambiguous_mapping_required(self):
        for mode in ("raw_offset_outside_file", "truncated_backing", "overlapping_table",
                     "overlapping_target", "mapped_bytes_differ_from_file"):
            def mutate(ns):
                if mode == "raw_offset_outside_file":
                    ns["pe"].sections[1].PointerToRawData = len(self.data)
                elif mode == "truncated_backing":
                    ns["RAW_IMAGE"] = ns["RAW_IMAGE"][:0x508]
                elif mode.startswith("overlapping_"):
                    extra = copy.copy(ns["pe"].sections[1])
                    extra.VirtualAddress = 0x2108 if mode == "overlapping_table" else 0x1080
                    extra.Misc_VirtualSize = extra.SizeOfRawData = 8
                    ns["pe"].sections.append(extra)
                else:
                    # Still points to valid code, but is no longer file-backed data.
                    struct.pack_into("<I", ns["img"], 0x2100, 0x4010A0)
            with self.subTest(mode=mode):
                roots, ns = self.candidates(mutate=mutate)
                self.assertEqual(roots, set())
                self.assertIn(SITE, ns["indirect_jumps"])

    def test_raw_snapshot_outlives_pefile_mapping_and_rebased_inputs_decline(self):
        def close_backing(ns):
            closed = mmap.mmap(-1, 1)
            closed.close()
            ns["pe"].__data__ = closed
            self.assertEqual(ns["RAW_IMAGE"], bytes(self.data))
        self.assertEqual(self.candidates(mutate=close_backing)[0], TARGETS)
        def rebase(ns):
            ns["NEW_BASE"] = 0x800000
        self.assertEqual(self.candidates(mutate=rebase)[0], set())

    @unittest.skipUnless("fork" in multiprocessing.get_all_start_methods(), "Upstream parallel discovery requires fork")
    def test_serial_parallel_root_merge_preserves_tables_and_gap_coverage(self):
        self.exe.write_bytes(self.data)
        runs = {}
        for name, root in (("base", self.base), ("patched", self.changed)):
            for jobs in (1, 2):
                output = self.folder / f"{name}-{jobs}.json"
                result = subprocess.run([sys.executable, str(root / "tools/discover.py"), str(self.exe), str(output)],
                                        env={**os.environ, "DISCOVER_JOBS": str(jobs)}, capture_output=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                runs[name, jobs] = (json.loads(output.read_text()), json.loads(Path(str(output) + ".gaps").read_text()))
            self.assertEqual(runs[name, 1], runs[name, 2])
        base, old_gaps = runs["base", 1]
        changed, new_gaps = runs["patched", 1]
        self.assertEqual(base["jump_tables"], changed["jump_tables"])
        self.assertEqual(old_gaps, new_gaps)
        self.assertIn(hex(SITE), changed["indirect_jumps"])
        for target in TARGETS:
            self.assertIn(hex(target), changed["functions"])


if __name__ == "__main__":
    unittest.main()
