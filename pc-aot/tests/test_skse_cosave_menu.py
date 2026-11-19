"""Inspect stored Classic menu registrations without entering the game VM."""
import ctypes as C
import struct
import unittest
import test_skse_cosave_generation as gen
import test_skse_cosave_history as hist

MENR, REGS, REGE, KEYR = (int.from_bytes(s, "big") for s in (b"MENR", b"REGS", b"REGE", b"KEYR"))
OK, ARG, SOURCE, MAP_ERROR, MISSING, UNSUPPORTED, INVALID, DUPLICATE, ORDER, LIMIT = range(10)


class Summary(C.Structure):
    _fields_ = [("groups", C.c_uint32), ("handles", C.c_uint32), ("resolutions", C.c_uint32 * 8)]


OBSERVER = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p, C.c_size_t, C.c_uint64, C.c_int, C.POINTER(C.c_uint64))


def group(name, handles, *, payload=None, version=1):
    data = struct.pack("<H", len(name)) + name + struct.pack("<I", len(handles))
    data += b"".join(struct.pack("<Q", h) for h in handles)
    return REGS, version, data if payload is None else payload


class MenuBinding:
    def __init__(self, library):
        self.lib = C.CDLL(str(library))
        self.load = self.lib.skse_menu_history_load
        self.load.argtypes = [gen.PTR, C.POINTER(gen.Generation), C.POINTER(hist.Loaded), C.c_size_t,
                              C.POINTER(Summary), OBSERVER, gen.PTR, C.POINTER(C.c_int), C.POINTER(C.c_int)]
        self.load.restype = C.c_int

    def inspect(self, store, generation, current, collect=True):
        loaded = (hist.Loaded * len(current))(*(hist.Loaded(n, len(n), idx, kind) for n, idx, kind in current))
        rows = []
        @OBSERVER
        def observe(_, name, length, saved, result, resolved):
            rows.append((C.string_at(name, length), saved, result, resolved[0] if resolved else None))
        result, source = C.c_int(-1), C.c_int(-1)
        summary = Summary()
        C.memset(C.byref(summary), 0xdd, C.sizeof(summary))
        rc = self.load(store, C.byref(generation), loaded, len(loaded), C.byref(summary),
                       observe if collect else OBSERVER(), None, C.byref(source), C.byref(result))
        return rc, summary, rows, source.value, result.value


class MenuTests(gen.GenerationHarness):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.menu = MenuBinding(cls.library)

    def setUp(self):
        super().setUp()
        self.mods = (hist.MODS, 0, hist.history(b"Skyrim.esm", b"A.esp", b"B.esp", b"C.esp", b"D.esp"))
        self.current = [(b"Skyrim.esm", 0, 0), (b"A.esp", 9, 0), (b"C.esp", 255, 1),
                        (b"D.esp", 5, 0), (b"d.EsP", 8, 0)]
        self.start = (MENR, 1, b"")
        self.end = (REGE, 1, b"")

    def store_core(self, event, records):
        g = self.g(event)
        self.publish(g, gen.image(gen.section(0, records)))
        return g

    def test_real_group_boundaries_and_64_bit_handles(self):
        a, missing, incompatible, ambiguous, dynamic, outside = (
            0xaaaa001201234567, 0xbb00000102000012, 0xcc00000203000034,
            0xdd00000304000055, 0x12345678ff001122, 0xddbc3344fe001234)
        unrelated = [(KEYR, 1, b""), (REGS, 1, b"completely different key encoding"), self.end]
        records = [self.mods, *unrelated, self.start, group(b"InventoryMenu", [a, a, missing, incompatible]),
                   group(b"MapMenu", [ambiguous, dynamic, outside]), self.end, *unrelated]
        g = self.store_core(9, records)
        rc, s, rows, source, mapping = self.menu.inspect(self.store, g, self.current)
        self.assertEqual((rc, source, mapping, s.groups, s.handles), (OK, gen.OK, hist.MAP_OK, 2, 7))
        self.assertEqual(list(s.resolutions), [2, 1, 0, 1, 1, 1, 1, 0])
        self.assertEqual(rows, [
            (b"InventoryMenu", a, hist.RESOLVED, 0xaaaa001209234567),
            (b"InventoryMenu", a, hist.RESOLVED, 0xaaaa001209234567),
            (b"InventoryMenu", missing, hist.MISSING, None),
            (b"InventoryMenu", incompatible, hist.INCOMPATIBLE, None),
            (b"MapMenu", ambiguous, hist.AMBIGUOUS, None), (b"MapMenu", dynamic, hist.DYNAMIC, dynamic),
            (b"MapMenu", outside, hist.OUT_OF_RANGE, None)])
        rc, stats, none, _, _ = self.menu.inspect(self.store, g, self.current, collect=False)
        self.assertEqual((rc, bytes(stats), none), (OK, bytes(s), []))

    def test_empty_menu_group_and_name_boundaries(self):
        data = self.store_core(3, [self.mods, self.start, self.end])
        rc, stats, rows, _, _ = self.menu.inspect(self.store, data, self.current)
        self.assertEqual((rc, stats.groups, stats.handles, rows), (OK, 0, 0, []))
        data = self.store_core(4, [self.mods, self.start, group(b"N" * 256, []), self.end])
        rc, stats, rows, _, _ = self.menu.inspect(self.store, data, self.current)
        self.assertEqual((rc, stats.groups, stats.handles, rows), (OK, 1, 0, []))

    def test_incomplete_or_future_data_has_no_observer_side_effects(self):
        completed = [self.mods, self.start, group(b"Map", [3])]
        bad = [(completed, INVALID), (completed + [(KEYR, 1, b""), self.end], INVALID),
               (completed + [self.end, self.start, self.end], DUPLICATE),
               ([self.start, self.end, self.mods], ORDER), ([self.mods], MISSING),
               ([self.mods, (MENR, 2, b""), self.end], UNSUPPORTED),
               ([self.mods, (MENR, 1, b"unexpected"), self.end], INVALID),
               (completed + [(REGE, 2, b"")], UNSUPPORTED), (completed + [(REGE, 1, b"x")], INVALID),
               (completed + [group(b"name", [], version=2), self.end], UNSUPPORTED)]
        for payload in (b"\x01", b"\x01\0x", struct.pack("<H", 257) + bytes(263),
                        group(b"", [3])[2], group(b"ab\0cd", [3])[2],
                        group(b"x", [3])[2][:-1], group(b"x", [3])[2] + b"y",
                        struct.pack("<H", 1) + b"x" + struct.pack("<I", 0xffffffff)):
            bad.append((completed + [(REGS, 1, payload), self.end], INVALID))
        for event, (records, expected) in enumerate(bad, 10):
            with self.subTest(event=event):
                g = self.store_core(event, records)
                rc, summary, rows, src, mapping = self.menu.inspect(self.store, g, self.current)
                self.assertEqual((rc, list(rows), bytes(summary), src, mapping),
                                 (expected, [], bytes(C.sizeof(summary)), gen.OK, hist.MAP_OK))

    def test_damaged_generation_missing_core_and_incompatible_mod_list(self):
        rc, _, _, source, mapping = self.menu.inspect(self.store, self.g(1), self.current)
        self.assertEqual((rc, source, mapping), (SOURCE, gen.MISSING, hist.SOURCE))
        foreign = self.g(2)
        self.publish(foreign, gen.image(gen.section(12, [self.mods, self.start, self.end])))
        rc, _, _, source, mapping = self.menu.inspect(self.store, foreign, self.current)
        self.assertEqual((rc, source, mapping), (SOURCE, gen.NO_OWNER, hist.SOURCE))
        future = self.store_core(3, [(hist.MODS, 3, hist.history(b"X.esp")), self.start, self.end])
        rc, _, rows, source, mapping = self.menu.inspect(self.store, future, self.current)
        self.assertEqual((rc, rows, source, mapping), (MAP_ERROR, [], gen.OK, hist.UNSUPPORTED))

    def test_over_budget_groups_and_handles_never_reach_observer(self):
        cases = [[group(b"x", [])] * 4097, [group(b"x", [1] * 65537)]]
        for event, groups in enumerate(cases, 1):
            g = self.store_core(event, [self.mods, self.start, *groups, self.end])
            rc, summary, rows, _, _ = self.menu.inspect(self.store, g, self.current)
            self.assertEqual((rc, summary.groups, summary.handles, rows), (LIMIT, 0, 0, []))


if __name__ == "__main__":
    unittest.main()
