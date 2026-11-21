"""Read UID 0 MODS histories and resolve Classic form IDs and VM handles."""
import ctypes as C
import random
import struct
import unittest
import test_skse_cosave_generation as gen

MODS = 0x4d4f4453
MAP_OK, MAP_ARG, SOURCE, NO_HISTORY, UNSUPPORTED, INVALID, DUP_HISTORY, DUP_NAME, BAD_CURRENT = range(9)
RESOLVED, DYNAMIC, NOT_READY, OUT_OF_RANGE, MISSING, INCOMPATIBLE, AMBIGUOUS, BAD_ARG = range(8)


class Loaded(C.Structure):
    _fields_ = [("name", C.c_char_p), ("length", C.c_size_t), ("index", C.c_uint8), ("kind", C.c_int)]


class ModMap(C.Structure):
    _fields_ = [("saved_count", C.c_uint8), ("ready", C.c_uint8),
                ("current_index", C.c_uint8 * 255), ("state", C.c_uint8 * 255)]


def history(*names):
    return bytes([len(names)]) + b"".join(struct.pack("<H", len(name)) + name for name in names)


class HistoryBinding:
    def __init__(self, path):
        self.lib = C.CDLL(str(path))
        self.load = self.lib.skse_mod_map_load
        self.load.argtypes = [gen.PTR, C.POINTER(gen.Generation), C.POINTER(Loaded), C.c_size_t,
                              C.POINTER(ModMap), C.POINTER(C.c_int)]
        self.load.restype = C.c_int
        self.clear = self.lib.skse_mod_map_clear
        self.clear.argtypes = [C.POINTER(ModMap)]
        self.clear.restype = None
        self.form = self.lib.skse_mod_resolve_form
        self.form.argtypes = [C.POINTER(ModMap), C.c_uint32, C.POINTER(C.c_uint32)]
        self.form.restype = C.c_int
        self.handle = self.lib.skse_mod_resolve_handle
        self.handle.argtypes = [C.POINTER(ModMap), C.c_uint64, C.POINTER(C.c_uint64)]
        self.handle.restype = C.c_int
        self.encode = self.lib.skse_mod_history_encode
        self.encode.argtypes = [C.POINTER(Loaded), C.c_size_t, C.POINTER(gen.PTR), C.POINTER(C.c_size_t)]
        self.encode.restype = C.c_int
        self.write = self.lib.skse_pair_write_mod_history
        self.write.argtypes = [gen.PTR, C.POINTER(Loaded), C.c_size_t]
        self.write.restype = C.c_int

    def build(self, store, g, entries, out):
        candidates = (Loaded * len(entries))(*(Loaded(name, len(name), index, kind) for name, index, kind in entries))
        status = C.c_int(-1)
        result = self.load(store, C.byref(g), candidates, len(candidates), C.byref(out), C.byref(status))
        return result, status.value


class HistoryTests(gen.GenerationHarness):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.h = HistoryBinding(cls.library)

    def setUp(self):
        super().setUp()
        self.table = ModMap()

    def publish_core(self, event, records):
        g = self.g(event)
        self.publish(g, gen.image(gen.section(0, records), gen.section(gen.UID, [(MODS, 0, history(b"foreign"))])))
        return g

    def resolve(self, code, expected, mapped=None):
        out32, out64 = C.c_uint32(0x90807060), C.c_uint64(0x1122334455667788)
        tag = 0xffcd567800000000
        self.assertEqual(self.h.form(C.byref(self.table), code, C.byref(out32)), expected)
        self.assertEqual(self.h.handle(C.byref(self.table), tag | code, C.byref(out64)), expected)
        if mapped is None:
            self.assertEqual((out32.value, out64.value), (0x90807060, 0x1122334455667788))
        else:
            self.assertEqual((out32.value, out64.value), (mapped, tag | mapped))

    def encoded(self, rows):
        loaded = (Loaded * len(rows))(*(Loaded(name, len(name), index, kind) for name, index, kind in rows))
        out, length = gen.PTR(1), C.c_size_t(99)
        result = self.h.encode(loaded, len(loaded), C.byref(out), C.byref(length))
        if result != gen.OK:
            self.assertFalse(out)
            self.assertEqual(length.value, 0)
            return result, None
        self.addCleanup(self.native.free, out)
        return result, C.string_at(out, length.value)

    def test_encode_round_trip_uses_current_indices_and_raw_names(self):
        count = 255
        names = [f"fixture-{i}.esp".encode("ascii") for i in range(count)]
        names[0], names[-2], names[-1] = b"Skyrim.esm", b"\x82\xa0.esp", b"X" * 255 + b".esp"
        rows = [(name, index, 0) for index, name in reversed(list(enumerate(names)))]
        result, saved = self.encoded(rows)
        self.assertEqual((result, saved), (gen.OK, history(*names)))
        g = self.publish_core(4, [(MODS, 0, saved)])
        reordered = [(name, count - 1 - index, kind) for name, index, kind in rows]
        self.assertEqual(self.h.build(self.store, g, reordered, self.table), (MAP_OK, gen.OK))
        self.resolve(0xfeabcdef, RESOLVED, 0x00abcdef)
        self.resolve(0xfdabcdef, RESOLVED, 0x01abcdef)

    def test_encode_rejects_incomplete_or_ambiguous_catalogs(self):
        cases = [
            [(b"gap.esp", 1, 0)],
            [(b"first.esp", 0, 0), (b"duplicate-index.esp", 0, 0)],
            [(b"same.esp", 0, 0), (b"SAME.ESP", 1, 0)],
            [(b"incompatible.esp", 0, 1)],
            [(b"broken\0.esp", 0, 0)],
            [(b"x" * 260, 0, 0)],
        ]
        for rows in cases:
            with self.subTest(rows=rows):
                self.assertEqual(self.encoded(rows), (gen.INVALID, None))

    def test_update_only_mods_in_existing_core_and_keep_plugin_sections(self):
        foreign = gen.section(gen.UID, [(MODS, 0, history(b"foreign"))])
        extra = (333, 9, b"opaque Core data")
        base = self.publish_core(1, [extra, (MODS, 0, history(b"old.esp"))])
        changed = self.g(2)
        rc, stage = self.native.create(self.store, changed.key, base)
        self.assertEqual(rc, gen.OK)
        self.addCleanup(self.native.discard, stage)
        loaded = (Loaded * 2)(Loaded(b"New.esp", 7, 1, 0), Loaded(b"Skyrim.esm", 10, 0, 0))
        self.assertEqual(self.h.write(stage, loaded, len(loaded)), gen.OK)
        self.assertEqual(self.native.commit(self.store, stage, C.byref(changed)), gen.OK)
        expected = gen.image(gen.section(0, [(MODS, 0, history(b"Skyrim.esm", b"New.esp")), extra]), foreign)
        self.assertEqual(self.native.data(self.store, changed), (gen.OK, expected))
        self.assertEqual(self.native.data(self.store, base), (gen.OK,
            gen.image(gen.section(0, [extra, (MODS, 0, history(b"old.esp"))]), foreign)))

    def test_reordered_missing_renamed_ambiguous_and_incompatible(self):
        names = [b"Skyrim.esm", b"Spark.esp", b"old-name.esp", b"OtherEdition.esp", b"Fold.esp", b"\x82\xa0.ESP"]
        g = self.publish_core(6, [(333, 9, b"opaque"), (MODS, 0, history(*names))])
        current = [(b"new-name.esp", 4, 0), (b"FOLD.ESP", 6, 0), (b"otheredition.esp", 255, 1),
                   (b"SKYRIM.ESM", 0, 0), (b"sParK.ESP", 254, 0), (b"fold.esp", 5, 0), (b"\x82\xa0.esp", 17, 0)]
        self.assertEqual(self.h.build(self.store, g, current, self.table), (MAP_OK, gen.OK))
        self.assertEqual((self.table.saved_count, self.table.ready), (6, 1))
        self.resolve(0, RESOLVED, 0)
        self.resolve(0x01234567, RESOLVED, 0xfe234567)
        self.resolve(0x02010203, MISSING)
        self.resolve(0x03010203, INCOMPATIBLE)
        self.resolve(0x04010203, AMBIGUOUS)
        self.resolve(0x05010203, RESOLVED, 0x11010203)
        self.resolve(0x06010203, OUT_OF_RANGE)
        self.resolve(0xff123456, DYNAMIC, 0xff123456)
        self.h.clear(C.byref(self.table))
        self.resolve(0x01234567, NOT_READY)
        self.resolve(0xffffffff, DYNAMIC, 0xffffffff)

    def test_classic_full_load_slot_fe_and_byte_length_limits(self):
        names = [b"index-" + str(i).encode() + b".esp" for i in range(255)]
        names[254] = b"A" * 255 + b".esm"
        current = [(name, 254 - i, 0) for i, name in enumerate(names)]
        g = self.publish_core(4, [(MODS, 0, history(*names))])
        self.assertEqual(self.h.build(self.store, g, current, self.table), (MAP_OK, gen.OK))
        self.assertEqual(self.table.saved_count, 255)
        self.resolve(0xfeabcdef, RESOLVED, 0x00abcdef)
        self.resolve(0x00000000, RESOLVED, 0xfe000000)
        self.resolve(0xffabcdef, DYNAMIC, 0xffabcdef)
        self.assertEqual(C.sizeof(self.table), 512)

    def test_invalid_or_incomplete_history_empties_previous_table(self):
        cases = [
            ([], NO_HISTORY),
            ([(5, 0, b"unknown")], NO_HISTORY),
            ([(MODS, 1, history(b"a.esp"))], UNSUPPORTED),
            ([(MODS, 0, b"")], INVALID),
            ([(MODS, 0, b"\x01\x03")], INVALID),
            ([(MODS, 0, b"\x01\xff\xffshort")], INVALID),
            ([(MODS, 0, history(b""))], INVALID),
            ([(MODS, 0, history(b"a\0b.esp"))], INVALID),
            ([(MODS, 0, history(b"x" * 260))], INVALID),
            ([(MODS, 0, history(b"x") + b"extra")], INVALID),
            ([(MODS, 0, history(b"same.esp", b"SAME.ESP"))], DUP_NAME),
            ([(MODS, 0, history(b"x")), (MODS, 0, history(b"y"))], DUP_HISTORY)]
        good = self.publish_core(2, [(MODS, 0, history(b"x"))])
        current = [(b"x", 10, 0)]
        for i, (records, expected) in enumerate(cases):
            with self.subTest(index=i):
                self.assertEqual(self.h.build(self.store, good, current, self.table), (MAP_OK, gen.OK))
                g = self.publish_core(24 + i, records)
                self.assertEqual(self.h.build(self.store, g, current, self.table), (expected, gen.OK))
                self.assertEqual(bytes(self.table), bytes(512))
                self.resolve(0x00000002, NOT_READY)

    def test_empty_mods_and_missing_core_or_generation(self):
        empty = self.publish_core(1, [(MODS, 0, history())])
        self.assertEqual(self.h.build(self.store, empty, [], self.table), (MAP_OK, gen.OK))
        self.resolve(0x00000007, OUT_OF_RANGE)
        self.resolve(0xff112233, DYNAMIC, 0xff112233)
        foreign = self.g(2)
        self.publish(foreign, gen.image(gen.section(gen.UID, [(MODS, 0, history(b"x"))])))
        self.assertEqual(self.h.build(self.store, foreign, [], self.table), (SOURCE, gen.NO_OWNER))
        self.assertEqual(self.table.ready, 0)
        self.assertEqual(self.h.build(self.store, self.g(4), [], self.table), (SOURCE, gen.MISSING))
        changed = self.root / "other.ess"
        changed.write_bytes(b"different completed generation")
        with changed.open("rb") as file:
            self.assertEqual(self.h.build(self.store, self.g(1, file.fileno()), [], self.table), (SOURCE, gen.WRONG))

    def test_current_indices_are_explicit_and_malformed_snapshots_fail(self):
        g = self.publish_core(1, [(MODS, 0, history(b"a", b"b"))])
        cases = [[(b"a", 255, 0)], [(b"", 1, 0)], [(b"x" * 260, 1, 0)], [(b"a\0b", 3, 0)],
                 [(b"a", 3, 0), (b"b", 3, 0)], [(b"a", 0, 4)], [(b"x", 245, 1)] * 256]
        for i, values in enumerate(cases):
            with self.subTest(i=i):
                self.assertEqual(self.h.build(self.store, g, values, self.table), (BAD_CURRENT, gen.ARG))
                self.assertEqual(self.table.ready, 0)
        self.assertEqual(self.h.build(self.store, g, [(b"b", 19, 0), (b"a", 4, 0)], self.table), (MAP_OK, gen.OK))
        self.resolve(0x00443322, RESOLVED, 0x04443322)
        self.resolve(0x01443322, RESOLVED, 0x13443322)
        self.assertEqual(self.h.form(None, 0, None), BAD_ARG)

    def test_non_ascii_bytes_exact_and_no_filename_or_unicode_alias_guess(self):
        g = self.publish_core(8, [(MODS, 0, history(b"\xc9p\xe9e.esp", b"f.esp", b"a/odd.esp"))])
        current = [(b"\xe9p\xe9e.esp", 1, 0), (b"f.esm", 2, 0), (b"ODD.ESP", 7, 0)]
        self.assertEqual(self.h.build(self.store, g, current, self.table), (MAP_OK, gen.OK))
        for i in range(3):
            self.resolve((i << 24) | 5, MISSING)
        self.assertEqual(self.h.build(self.store, g, [(b"\xc9P\xe9E.ESP", 3, 0)], self.table), (MAP_OK, gen.OK))
        self.resolve(4, RESOLVED, 0x03000004)

    def test_randomized_order_and_preservation_of_non_index_handle_bits(self):
        rng = random.Random(173)
        for event in range(1, 16):
            names = [("Owner%02d.esp" % n).encode() for n in range(rng.randrange(3, 80))]
            indices = rng.sample(range(255), len(names))
            current = [(name.upper(), idx, 0) for name, idx in zip(names, indices)]
            rng.shuffle(current)
            g = self.publish_core(event, [(MODS, 0, history(*names))])
            self.assertEqual(self.h.build(self.store, g, current, self.table), (MAP_OK, gen.OK))
            for i, mapped in enumerate(indices):
                low, high = rng.getrandbits(24), rng.getrandbits(32)
                saved = high << 32 | i << 24 | low
                out = C.c_uint64()
                self.assertEqual(self.h.handle(C.byref(self.table), saved, C.byref(out)), RESOLVED)
                self.assertEqual(out.value, high << 32 | mapped << 24 | low)


if __name__ == "__main__":
    unittest.main()
