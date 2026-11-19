"""Classic UID 0 ModEvent callback registrations, independent of VM execution."""
import ctypes as C
import struct
import unittest
import test_skse_cosave_generation as gen
import test_skse_cosave_history as hist
import test_skse_cosave_menu as menus

MCBR = int.from_bytes(b"MCBR", "big")
CALLBACKS = 1


class Name(C.Structure):
    _fields_ = [("data", gen.PTR), ("size", C.c_size_t)]


class Key(C.Structure):
    _fields_ = [("kind", C.c_int), ("name", Name), ("number", C.c_uint32)]


class Format(C.Structure):
    _fields_ = [("tag", C.c_uint32), ("kind", C.c_int), ("named", C.c_int), ("recognized", C.c_int)]


OBSERVER = C.CFUNCTYPE(None, gen.PTR, Key, Name, C.c_uint64, C.c_int, C.POINTER(C.c_uint64))


def string(data):
    return struct.pack("<H", len(data)) + data


def group(event, subscriptions):
    body = [string(event), struct.pack("<I", len(subscriptions))]
    body.extend(struct.pack("<Q", handle) + string(name) for handle, name in subscriptions)
    return menus.REGS, 1, b"".join(body)


class CallbackBinding:
    def __init__(self, path):
        self.lib = C.CDLL(str(path))
        self.read = self.lib.skse_registration_history_load
        self.read.argtypes = [gen.PTR, C.POINTER(gen.Generation), C.POINTER(hist.Loaded), C.c_size_t,
                              C.c_int, C.POINTER(menus.Summary), OBSERVER, gen.PTR,
                              C.POINTER(C.c_int), C.POINTER(C.c_int)]
        self.read.restype = C.c_int
        self.format = self.lib.skse_registration_format
        self.format.argtypes, self.format.restype = [C.c_int, C.POINTER(Format)], C.c_int

    def inspect(self, store, generation, catalog, family=CALLBACKS, observe=True):
        current = (hist.Loaded * len(catalog))(*(hist.Loaded(n, len(n), idx, kind) for n, idx, kind in catalog))
        summary, src, state, rows = menus.Summary(), C.c_int(), C.c_int(), []

        @OBSERVER
        def collect(_, event, callback, saved, resolution, mapped):
            key = C.string_at(event.name.data, event.name.size) if event.kind == 1 else event.number if event.kind == 2 else None
            rows.append((key, C.string_at(callback.data, callback.size), saved, resolution, mapped[0] if mapped else None))

        result = self.read(store, C.byref(generation), current, len(current), family, C.byref(summary),
                           collect if observe else OBSERVER(), None, C.byref(src), C.byref(state))
        return result, summary, rows, src.value, state.value


class CallbackTests(gen.GenerationHarness):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.callbacks = CallbackBinding(cls.library)

    def setUp(self):
        super().setUp()
        self.mods = hist.MODS, 0, hist.history(b"Skyrim.esm", b"Old.esp", b"Removed.esp")
        self.catalog = [(b"Skyrim.esm", 0, 0), (b"OLD.ESP", 6, 0)]
        self.start, self.end = (MCBR, 1, b""), (menus.REGE, 1, b"")

    def stored(self, number, owner_records):
        target = self.g(number)
        self.publish(target, gen.image(gen.section(0, owner_records)))
        return target

    def test_callbacks_on_same_event_have_independent_names_and_handles(self):
        live = 0xaaaaeeee01003210
        absent = 0xbbbbdddd02005555
        dynamic = 0xeeeeffffff007890
        max_name = b"S" * 256
        event = self.stored(70, [self.mods, (menus.MENR, 1, b""), menus.group(b"MapMenu", [live]),
                                 self.end, self.start,
                                 group(b"AuthoredEvent", [(live, b"OnToggle"), (absent, b"OnMissing"),
                                                          (dynamic, max_name)]),
                                 group(b"AnotherEvent", [(live, b"OnReading")]), self.end])
        rc, total, rows, src, state = self.callbacks.inspect(self.store, event, self.catalog)
        self.assertEqual((rc, total.groups, total.handles, src, state), (menus.OK, 2, 4, gen.OK, hist.MAP_OK))
        self.assertEqual(rows, [(b"AuthoredEvent", b"OnToggle", live, hist.RESOLVED, 0xaaaaeeee06003210),
                                (b"AuthoredEvent", b"OnMissing", absent, hist.MISSING, None),
                                (b"AuthoredEvent", max_name, dynamic, hist.DYNAMIC, dynamic),
                                (b"AnotherEvent", b"OnReading", live, hist.RESOLVED, 0xaaaaeeee06003210)])
        rc, s, bare, _, _ = self.callbacks.inspect(self.store, event, self.catalog, family=0)
        self.assertEqual((rc, s.handles, bare), (menus.OK, 1, [(b"MapMenu", b"", live, hist.RESOLVED, 0xaaaaeeee06003210)]))
        rc, stats, no_calls, _, _ = self.callbacks.inspect(self.store, event, self.catalog, observe=False)
        self.assertEqual((rc, bytes(stats), no_calls), (menus.OK, bytes(total), []))

    def test_malformed_tail_never_delivers_even_valid_initial_subscriptions(self):
        valid = group(b"event", [(1, b"First")])
        prefix = string(b"event") + struct.pack("<I", 2) + struct.pack("<Q", 11) + string(b"Valid")
        tails = [b"", b"\x01" * 7, bytes(8), bytes(8) + b"\x01", bytes(8) + string(b""),
                 bytes(8) + string(b"A\0B"), bytes(8) + string(b"X" * 257),
                 bytes(8) + struct.pack("<H", 8) + b"abc",
                 bytes(8) + string(b"Valid") + b"extra"]
        for n, tail in enumerate(tails, 1):
            with self.subTest(n=n):
                event = self.stored(n, [self.mods, self.start, valid, (menus.REGS, 1, prefix + tail), self.end])
                rc, stats, rows, _, _ = self.callbacks.inspect(self.store, event, self.catalog)
                self.assertEqual((rc, stats.groups, stats.handles, rows), (menus.INVALID, 0, 0, []))

    def test_family_boundary_order_version_and_zero_rows(self):
        invalid = (([self.mods, (menus.MENR, 1, b""), self.end], menus.MISSING),
                   ([self.start, self.end, self.mods], menus.ORDER),
                   ([self.mods, (MCBR, 3, b""), self.end], menus.UNSUPPORTED),
                   ([self.mods, self.start, (menus.MENR, 1, b""), self.end], menus.INVALID),
                   ([self.mods, self.start, self.end, self.start, self.end], menus.DUPLICATE),
                   ([self.mods, self.start, group(b"x", []), (menus.REGE, 2, b"")], menus.UNSUPPORTED))
        for n, (records, wanted) in enumerate(invalid, 30):
            rc, s, rows, _, _ = self.callbacks.inspect(self.store, self.stored(n, records), self.catalog)
            self.assertEqual((rc, s.handles, rows), (wanted, 0, []))
        valid = self.stored(50, [self.mods, self.start, self.end])
        rc, s, rows, _, _ = self.callbacks.inspect(self.store, valid, self.catalog)
        self.assertEqual((rc, s.handles, rows), (menus.OK, 0, []))
        rc, s, rows, _, _ = self.callbacks.inspect(self.store, valid, self.catalog, family=150)
        self.assertEqual((rc, s.groups, rows), (menus.ARG, 0, []))

    def test_invalid_core_wrong_generation_and_foreign_uid_are_separate(self):
        g = self.g(1)
        self.publish(g, gen.image(gen.section(1234, [self.mods, self.start, group(b"ev", [(1, b"F")]), self.end])))
        rc, _, rows, source, _ = self.callbacks.inspect(self.store, g, self.catalog)
        self.assertEqual((rc, rows, source), (menus.SOURCE, [], gen.NO_OWNER))
        bad = self.stored(2, [(hist.MODS, 2, b""), self.start, self.end])
        rc, _, rows, source, mode = self.callbacks.inspect(self.store, bad, self.catalog)
        self.assertEqual((rc, rows, source, mode), (menus.MAP_ERROR, [], gen.OK, hist.UNSUPPORTED))
        other = self.root / "different.ess"
        other.write_bytes(b"other bytes")
        with other.open("rb") as fd:
            wrong = gen.Generation(bad.key, fd.fileno())
            rc, _, rows, source, _ = self.callbacks.inspect(self.store, wrong, self.catalog)
            self.assertEqual((rc, rows, source), (menus.SOURCE, [], gen.WRONG))

    def test_callback_payload_and_group_limits(self):
        raw = string(b"E") + struct.pack("<I", 65537) + (struct.pack("<Q", 1) + string(b"A")) * 65537
        for n, records in ((61, [(menus.REGS, 1, raw)]), (62, [group(b"E", [])] * 4097)):
            event = self.stored(n, [self.mods, self.start, *records, self.end])
            rc, s, rows, _, _ = self.callbacks.inspect(self.store, event, self.catalog)
            self.assertEqual((rc, s.handles, rows), (menus.LIMIT, 0, []))


if __name__ == "__main__":
    unittest.main()
