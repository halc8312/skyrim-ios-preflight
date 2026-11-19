"""Check the bounded numeric observer and exact callsite matching on authored inputs."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1] / "experiments/classic-lifecycle"
spec = importlib.util.spec_from_file_location("classic_lifecycle_prepare", HERE / "prepare.py")
prep = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = prep
spec.loader.exec_module(prep)


class Sites(unittest.TestCase):
    site = prep.Site(2, 0x1100, 0x1105, 0x1200, True)  # invented image below

    def image(self):
        data = bytearray(1024)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 80)
        data[80:84] = b"PE\0\0"
        struct.pack_into("<HH", data, 84, 0x14c, 1)
        struct.pack_into("<H", data, 100, 96)
        struct.pack_into("<H", data, 104, 0x10b)
        struct.pack_into("<I", data, 132, 0x1000)
        struct.pack_into("<4I", data, 208, 512, 0x100, 512, 512)
        struct.pack_into("<I", data, 236, 0x20000000)
        data[517] = 0xe8
        struct.pack_into("<i", data, 518, self.site.target - (self.site.instruction + 5))
        return data

    def test_wrong_image_call_target_owner_or_permissions_are_rejected(self):
        graph = {"functions": {"1100": [[0x1100, 0x1110]], "1200": []}}
        data = self.image()
        prep.check_image(data, graph, [self.site])
        for offset in (0, 236 + 3, 517, 518):
            other = data.copy()
            other[offset] ^= 0x20 if offset == 239 else 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                prep.check_image(other, graph, [self.site])
        graph["functions"]["1100"] = [[0x1100, 0x1107]]
        with self.assertRaises(ValueError):
            prep.check_image(data, graph, [self.site])

    def test_only_the_matched_owner_and_return_site_are_instrumented(self):
        field = b"M32(esp) = 0x110Au; SPILL(); f_1200(c); RELOAD();"
        src = b"void f_1100(CPU *c) {\n" + field + b"\n}\nvoid f_1300(CPU *c) {\n" + field + b"\n}\n"
        changed, found = prep.instrument(src, self.site)
        self.assertTrue(found)
        self.assertEqual(changed.count(b"SKYRIM_LIFECYCLE_CALL"), 1)
        self.assertEqual(changed[changed.index(b"void f_1300"):], src[src.index(b"void f_1300"):])
        for bad in (src.replace(b"110Au", b"110Bu"), src.replace(field, field + b"\n" + field), src * 2):
            with self.assertRaises(ValueError):
                prep.instrument(bad, self.site)
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "exe_01.c").write_bytes(src)
            result = prep.build_overlay(folder, [self.site])
            self.assertEqual(list(result), ["exe_01.c"])
            self.assertEqual((folder / "exe_01.c").read_bytes(), src)
            (folder / "exe_02.c").write_bytes(src)
            with self.assertRaises(ValueError):
                prep.build_overlay(folder, [self.site])


@unittest.skipUnless(os.name == "posix" and shutil.which("cc"), "POSIX C compiler required")
class Observations(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.work = Path(tmp.name)
        for enabled in (False, True):
            extra = ["-DSKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE=1", str(HERE / "lifecycle.c")] if enabled else []
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-std=c11", "-pthread", "-O2", *extra,
                            str(HERE / "synthetic.c"), "-o", str(cls.work / str(enabled))], check=True)

    def run_scope(self, arg, enabled=True):
        output = subprocess.check_output([self.work / str(enabled), arg], text=True, timeout=10)
        self.assertRegex(output, r"\A[0-9 \n]+\Z")
        return [list(map(int, row.split())) for row in output.splitlines()]

    def test_inserted_call_reads_only_al_after_guest_return(self):
        toy = b'''#include "lifecycle.h"
#include <stdio.h>
typedef struct { unsigned r[8]; } CPU;
static unsigned stack_word;
#define M32(x) stack_word
#define SPILL() (c->r[0] = eax)
#define RELOAD() (eax = c->r[0])
static void f_1200(CPU *c) { c->r[0] = c->r[1]; }
void f_1100(CPU *c) {
    unsigned eax = c->r[0];
    M32(esp) = 0x110Au; SPILL(); f_1200(c); RELOAD();
    c->r[2] = eax;
}
int main(void) {
    CPU c = {{0,0x12340000,0}};
    f_1100(&c);
    if (c.r[2] != 0x12340000) return 1;
    c.r[1] = 0x12340001; f_1100(&c);
    if (c.r[2] != 0x12340001) return 1;
    SkyrimLifecycleRow rows[4];
    if (skyrim_lifecycle_snapshot(rows, 4, NULL) != 4) return 1;
    return rows[1].outcome != SkyrimReturnedFalse || rows[3].outcome != SkyrimReturnedTrue;
}
'''
        changed, found = prep.instrument(toy, Sites.site)
        self.assertTrue(found)
        compiled = self.work / "guest-return"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(HERE),
                        "-DSKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE=1", "-x", "c", "-", str(HERE / "lifecycle.c"),
                        "-o", str(compiled)], input=changed, check=True)
        subprocess.run([compiled], check=True, timeout=10)

    def test_success_failure_and_unmodified_profile_copies(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "original-profile"
            source.mkdir()
            (source / "ok.ess").write_bytes(b"1 authored case")
            (source / "failed.ess").write_bytes(b"0 authored case")
            (source / "ok.skse").write_bytes(b"synthetic companion untouched")
            copied = Path(tmp) / "copied-profile"
            shutil.copytree(source, copied)
            def contents(folder):
                return {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in folder.iterdir()}
            before_source, before_copy = contents(source), contents(copied)
            for file, success in (("ok.ess", 1), ("failed.ess", 0)):
                head, *rows = self.run_scope(copied / file)
                self.assertEqual(head, [success, 14, 0])
                self.assertEqual([r[0] for r in rows], [7,7,1,3,3,1,2,5,5,4,4,2,6,6])
                self.assertEqual([r[2] for r in rows if r[1] and r[0] in (2,3,4)], [success + 1] * 3)
                self.assertTrue(all(r[2] == 0 for r in rows if not r[1] or r[0] not in (2,3,4)))
                self.assertEqual([r[1] for r in rows if r[3] == rows[2][3]], [0, 1])
                self.assertEqual(rows[3][4], rows[2][3])
                self.assertEqual(rows[7][4], rows[6][3])
                self.assertEqual(rows[9][4], rows[6][3])
                self.assertEqual(self.run_scope(copied / file, enabled=False), [[success, 0, 0]])
            self.assertEqual(contents(source), before_source)
            self.assertEqual(contents(copied), before_copy)

    def test_parallel_calls_keep_their_own_parent(self):
        head, *rows = self.run_scope("threads")
        self.assertEqual(head, [1, 8, 0])
        roots = {r[3] for r in rows if r[0] == 1 and not r[1] and not r[4]}
        children = {r[3]: r[4] for r in rows if r[0] == 3 and not r[1]}
        self.assertEqual(len(roots), 2)
        self.assertEqual(set(children.values()), roots)
        self.assertEqual({r[3]: r[4] for r in rows if r[0] == 3 and r[1]}, children)

    def test_capacity_keeps_complete_prefix_and_counts_discarded_rows(self):
        head, *rows = self.run_scope("overflow")
        self.assertEqual(head, [1, 256, 344])
        self.assertEqual(len(rows), 256)
        self.assertEqual(rows[0], [7, 0, 0, 1, 0])
        self.assertEqual(rows[-1], [7, 1, 0, 255, 0])

    def test_nonlocal_exit_stays_unpaired_instead_of_reporting_success(self):
        head, *rows = self.run_scope("nonlocal")
        self.assertEqual(head, [1, 3, 0])
        self.assertEqual(rows, [[2, 0, 0, 1, 0], [6, 0, 0, 2, 1], [6, 1, 0, 2, 1]])


if __name__ == "__main__":
    unittest.main()
