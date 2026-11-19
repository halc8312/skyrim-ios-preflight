"""Inspect pending Classic UICallbacks without invoking a menu or using saved string pointers."""
import ctypes as C
import struct
import unittest
import test_skse_cosave_callbacks as cb
import test_skse_cosave_generation as gen
import test_skse_cosave_history as hist
import test_skse_cosave_objects as obj


class Summary(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("slots", "filled", "callbacks", "mod_events", "arguments", "strings")]


class Event(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("index", "stack", "count")] + [(n, cb.Name) for n in ("menu", "target")]


class Argument(C.Structure):
    _fields_ = [("kind", C.c_int), ("bits", C.c_uint64)]


ON_UI = C.CFUNCTYPE(None, gen.PTR, C.POINTER(Event))
ON_ARG = C.CFUNCTYPE(None, gen.PTR, C.POINTER(Event), C.c_uint32, C.POINTER(Argument))


class UIReader(obj.ObjectBinding):
    def __init__(self, library):
        super().__init__(library)
        self.ui = self.lib.skse_ui_callback_objects
        self.ui.argtypes = [gen.PTR, C.POINTER(Summary), ON_UI, ON_ARG, gen.PTR]
        self.ui.restype = C.c_int

    def inspect_ui(self, store, generation, observe=True):
        catalog = (hist.Loaded * 1)(hist.Loaded(b"Skyrim.esm", 10, 0, 0))
        view, source, mapping = gen.PTR(), C.c_int(), C.c_int()
        rc = self.open(store, C.byref(generation), catalog, 1, C.byref(view), C.byref(source), C.byref(mapping))
        if rc:
            raise AssertionError((rc, source.value, mapping.value))
        summary, events, args = Summary(), [], []
        @ON_UI
        def event(_, ptr):
            e = ptr.contents
            events.append((e.index, e.stack, C.string_at(e.menu.data, e.menu.size),
                           C.string_at(e.target.data, e.target.size), e.count))
        @ON_ARG
        def arg(_, e, i, values):
            args.append((e.contents.index, i, values.contents.kind, values.contents.bits))
        try:
            code = self.ui(view, C.byref(summary), event if observe else ON_UI(), arg if observe else ON_ARG(), None)
            return code, summary, events, args
        finally:
            self.close(view)


def ui_arg(kind, bits):
    return struct.pack("<I", kind) + struct.pack({2: "<B", 3: "<Q", 4: "<I"}[kind], bits)


def ui(index, stack, menu, target, arguments, *, version=1, cls=b"UIInvokeDelegate", count=None, tail=b""):
    data = struct.pack("<I", len(cls)) + cls + cb.string(menu) + cb.string(target)
    data += struct.pack("<I", len(arguments) if count is None else count) + b"".join(arguments) + tail
    return obj.OBJE, version, data + struct.pack("<II", stack, index)


class UIObjectTests(gen.GenerationHarness):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.reader = UIReader(cls.library)

    def store_objects(self, n, records):
        g = self.g(n)
        core = gen.section(0, [(hist.MODS, 0, hist.history(b"Skyrim.esm")), *records])
        self.publish(g, gen.image(core, gen.section(gen.FOREIGN, [(obj.OBJE, 1, b"outside core")])))
        return g

    def test_numeric_bits_and_two_kinds_of_string_are_distinct_in_mixed_storage(self):
        target = b"t" * 256
        pending = ui(3, 0xf1, b"AuthoredMenu", target, [ui_arg(2, 1), ui_arg(2, 0), ui_arg(3, 0x8000000000000000),
                     ui_arg(3, 0x7ff81234567890ab), ui_arg(4, 0xffeeddcc)])
        other = obj.record(0, 88, b"SomeEvent", [obj.scalar(4, 0xfedcba90)])
        g = self.store_objects(8, [obj.container(6, 3), other, pending, ui(1, 34, b"M", b"method", [])])
        rc, summary, events, args = self.reader.inspect_ui(self.store, g)
        self.assertEqual((rc, summary.slots, summary.filled, summary.callbacks, summary.mod_events,
                          summary.arguments, summary.strings), (obj.OK, 6, 3, 2, 1, 5, 1))
        self.assertEqual(events, [(3, 0xf1, b"AuthoredMenu", target, 5), (1, 34, b"M", b"method", 0)])
        self.assertEqual([a[2:] for a in args], [(2, 1), (2, 0), (3, 0x8000000000000000),
                                               (3, 0x7ff81234567890ab), (4, 0)])
        self.assertEqual([(row[0], row[1]) for row in args], [(3, i) for i in range(5)])
        rc, stats, events, args = self.reader.inspect_ui(self.store, g, observe=False)
        self.assertEqual((rc, bytes(stats), events, args), (obj.OK, bytes(summary), [], []))
        self.assertEqual(self.reader.inspect(self.store, g, [(b"Skyrim.esm", 0, 0)])[0], obj.UNKNOWN)

    def test_invalid_ui_or_unrecognized_factory_blocks_even_earlier_valid_observations(self):
        first = ui(0, 22, b"Menu", b"path", [ui_arg(2, 1)])
        bad = [(ui(1, 2, b"", b"path", []), obj.INVALID), (ui(1, 2, b"M", b"x" * 257, []), obj.INVALID),
               (ui(1, 2, b"A\0B", b"t", []), obj.INVALID), (ui(1, 2, b"M", b"t", [], version=2), obj.VERSION),
               (ui(1, 2, b"M", b"t", [ui_arg(2, 2)]), obj.INVALID),
               (ui(1, 2, b"M", b"t", [struct.pack("<I", 5)]), obj.UNKNOWN_ARG),
               (ui(1, 2, b"M", b"t", [struct.pack("<II", 3, 0x1234)]), obj.INVALID),
               (ui(1, 2, b"M", b"t", [], count=8), obj.INVALID), (ui(1, 2, b"M", b"t", [], tail=b"extra"), obj.INVALID),
               (ui(1, 2, b"M", b"t", [], cls=b"UIInvokeFormDelegate"), obj.UNKNOWN),
               (obj.record(0, 2, b"event", []), obj.DUPLICATE)]
        for i, (last, expected) in enumerate(bad, 21):
            with self.subTest(case=i):
                g = self.store_objects(i, [obj.container(2, 2), first, last])
                rc, summary, events, args = self.reader.inspect_ui(self.store, g)
                self.assertEqual((rc, bytes(summary), events, args), (expected, bytes(C.sizeof(Summary)), [], []))

    def test_argument_limit_is_shared_with_mod_events_and_raw_pointer_is_always_opaque(self):
        data = [obj.container(2, 2), obj.record(0, 1, b"A", [obj.scalar(0)] * 99995),
                ui(1, 7, b"M", b"t", [ui_arg(2, 1)] * 6)]
        g = self.store_objects(91, data)
        self.assertEqual(self.reader.inspect_ui(self.store, g)[0], obj.LIMIT)
        for n, pointer in enumerate((0, 1, 0xffffffff), 92):
            g = self.store_objects(n, [obj.container(1, 1), ui(0, 8, b"M", b"f", [ui_arg(4, pointer)])])
            rc, stats, _, args = self.reader.inspect_ui(self.store, g)
            self.assertEqual((rc, stats.strings, args), (obj.OK, 1, [(0, 0, 4, 0)]))


if __name__ == "__main__":
    unittest.main()
