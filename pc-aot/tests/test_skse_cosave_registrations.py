"""Inspect all Classic 1.7.3 core registration families in one load snapshot."""
import ctypes as C
import os
import struct
import unittest
import test_skse_cosave_callbacks as cb
import test_skse_cosave_generation as gen
import test_skse_cosave_history as hist
import test_skse_cosave_menu as menus

FAMILIES = (b"MENR", b"MCBR", b"KEYR", b"CTLR", b"AACT", b"CHRR", b"CAMR", b"NINU", b"NiNU")
KIND = (1, 1, 2, 1, 2, 0, 0, 0, 0)


def code(family):
    return int.from_bytes(FAMILIES[family], "big")


def handles(*values):
    return struct.pack("<I", len(values)) + b"".join(struct.pack("<Q", v) for v in values)


def keyed(number, *values):
    return menus.REGS, 1, struct.pack("<I", number) + handles(*values)


class RegistrationTests(gen.GenerationHarness):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.binding = cb.CallbackBinding(cls.library)
        lib = cls.binding.lib
        lib.skse_registration_view_open.argtypes = [gen.PTR, C.POINTER(gen.Generation), C.POINTER(hist.Loaded),
                                                   C.c_size_t, C.POINTER(gen.PTR), C.POINTER(C.c_int), C.POINTER(C.c_int)]
        lib.skse_registration_view_open.restype = C.c_int
        lib.skse_registration_view_read.argtypes = [gen.PTR, C.c_int, C.POINTER(menus.Summary), cb.OBSERVER, gen.PTR]
        lib.skse_registration_view_read.restype = C.c_int
        lib.skse_registration_view_close.argtypes = [gen.PTR]
        lib.skse_registration_view_close.restype = None

    def setUp(self):
        super().setUp()
        self.mods = hist.MODS, 0, hist.history(b"Skyrim.esm", b"A.esp", b"Missing.esp")
        self.catalog = [(b"Skyrim.esm", 0, 0), (b"a.esp", 11, 0)]
        self.old, self.target = 0xaaaa00550100001a, 0xaaaa00550b00001a
        self.absent, self.dynamic = 0xbbbb00550200001c, 0x12340000ff00001d
        self.end = (menus.REGE, 1, b"")

    def generate(self, number, records):
        generation = self.g(number)
        self.publish(generation, gen.image(gen.section(0, [self.mods, *records])))
        return generation

    def test_descriptor_reports_exact_writer_and_reader_spellings(self):
        for family, key_type in enumerate(KIND):
            with self.subTest(family=family):
                fmt = cb.Format()
                self.assertEqual(self.binding.format(family, C.byref(fmt)), 1)
                self.assertEqual((fmt.tag, fmt.kind, fmt.named, fmt.recognized),
                                 (code(family), key_type, int(family == 1), int(family != 7)))
        invalid = cb.Format(1, 1, 1, 1)
        self.assertEqual(self.binding.format(-1, C.byref(invalid)), 0)
        self.assertEqual(bytes(invalid), bytes(C.sizeof(invalid)))

    def test_numeric_keys_named_controls_and_keyless_sets_have_distinct_boundaries(self):
        records = [(code(2), 1, b""), keyed(42, self.old, self.absent), keyed(0xffffffff, self.dynamic), self.end,
                   (code(3), 1, b""), menus.group(b"Ready Weapon", [self.old]), self.end,
                   (code(4), 1, b""), keyed(0, self.absent), self.end,
                   (code(5), 1, handles(self.dynamic, self.absent)), (code(6), 1, handles()),
                   (code(7), 1, handles(self.old)), (code(8), 1, handles(self.dynamic))]
        generation = self.generate(66, records)
        expected = {2: [(42, self.old, self.target), (42, self.absent, None), (0xffffffff, self.dynamic, self.dynamic)],
                    3: [(b"Ready Weapon", self.old, self.target)], 4: [(0, self.absent, None)],
                    5: [(None, self.dynamic, self.dynamic), (None, self.absent, None)], 6: [],
                    7: [(None, self.old, self.target)], 8: [(None, self.dynamic, self.dynamic)]}
        for family, wanted in expected.items():
            with self.subTest(family=family):
                status, summary, rows, src, mapping = self.binding.inspect(self.store, generation, self.catalog, family=family)
                self.assertEqual((status, src, mapping, summary.groups, summary.handles),
                                 (menus.OK, gen.OK, hist.MAP_OK, 2 if family == 2 else 1, len(wanted)))
                self.assertEqual([(key, saved, mapped) for key, callback, saved, _, mapped in rows if callback == b""], wanted)
        only_written = self.generate(67, [(code(7), 1, handles(self.old))])
        rc, _, rows, _, _ = self.binding.inspect(self.store, only_written, self.catalog, family=8)
        self.assertEqual((rc, rows), (menus.MISSING, []))

    def test_too_short_extra_duplicate_and_future_records_emit_nothing(self):
        valid = keyed(4, self.old)
        bad = [(2, [(code(2), 1, b""), valid, (menus.REGS, 1, b"\x01\x02"), self.end], menus.INVALID),
               (2, [(code(2), 1, b""), valid, (menus.REGS, 1, struct.pack("<II", 2, 0xffffffff)), self.end], menus.INVALID),
               (2, [(code(2), 1, b""), valid, (menus.REGS, 1, keyed(3, self.old)[2] + b"a"), self.end], menus.INVALID),
               (3, [(code(3), 1, b""), (menus.REGS, 1, cb.string(b"") + handles(self.old)), self.end], menus.INVALID),
               (5, [(code(5), 2, handles(self.old))], menus.UNSUPPORTED),
               (5, [(code(5), 1, b"")], menus.INVALID),
               (6, [(code(6), 1, handles(self.old)[:-1])], menus.INVALID),
               (6, [(code(6), 1, handles(self.old) + b"x")], menus.INVALID),
               (6, [(code(6), 1, handles(self.old))] * 2, menus.DUPLICATE),
               (7, [(code(7), 1, struct.pack("<I", 65537) + bytes(8 * 65537))], menus.LIMIT)]
        for n, (family, owner, expected) in enumerate(bad, 1):
            with self.subTest(n=n):
                rc, totals, rows, _, _ = self.binding.inspect(self.store, self.generate(n, owner), self.catalog, family=family)
                self.assertEqual((rc, totals.handles, rows), (expected, 0, []))

    def test_immutable_view_reuses_only_the_accepted_generation_without_more_file_reads(self):
        g = self.generate(22, [(code(2), 1, b""), keyed(3, self.old), self.end,
                               (code(5), 1, handles(self.dynamic))])
        lib = self.binding.lib
        view, source, mapping = gen.PTR(), C.c_int(), C.c_int()
        mods = (hist.Loaded * 2)(*(hist.Loaded(n, len(n), i, k) for n, i, k in self.catalog))
        held = os.dup(self.fd)
        try:
            pinned = gen.Generation(g.key, held)
            self.assertEqual(lib.skse_registration_view_open(self.store, C.byref(pinned), mods, 2, C.byref(view),
                                                            C.byref(source), C.byref(mapping)), menus.OK)
        finally:
            os.close(held)
        self.addCleanup(lib.skse_registration_view_close, view)
        self.ess.unlink()
        for family, expected in ((2, menus.OK), (5, menus.OK), (6, menus.MISSING), (87, menus.ARG)):
            stats = menus.Summary()
            result = lib.skse_registration_view_read(view, family, C.byref(stats), cb.OBSERVER(), None)
            self.assertEqual((result, stats.handles), (expected, 1 if expected == menus.OK else 0))


if __name__ == "__main__":
    unittest.main()
