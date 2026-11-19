"""Inspect stored Papyrus ModEvent objects without executing their factories or callbacks."""
import ctypes as C
import struct
import unittest
import test_skse_cosave_callbacks as cb
import test_skse_cosave_generation as gen
import test_skse_cosave_history as hist

OBMG, OBJE, DFMG = [int.from_bytes(x, "big") for x in (b"OBMG", b"OBJE", b"DFMG")]
OK, ARGUMENT, MISSING, VERSION, UNKNOWN, UNKNOWN_ARG, INVALID, DUPLICATE, LIMIT, ORDER = range(10)


class Summary(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("slots", "filled", "arguments", "strings", "unresolved")]


class Event(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("index", "stack", "arguments")] + [("name", cb.Name)]


class Argument(C.Structure):
    _fields_ = [("kind", C.c_int), ("bits", C.c_uint32), ("form", C.c_uint32), ("resolution", C.c_int)]


ON_OBJECT = C.CFUNCTYPE(None, gen.PTR, C.POINTER(Event))
ON_ARGUMENT = C.CFUNCTYPE(None, gen.PTR, C.POINTER(Event), C.c_uint32, C.POINTER(Argument))


class ObjectBinding:
    def __init__(self, library):
        self.lib = C.CDLL(str(library))
        self.open = self.lib.skse_core_snapshot_open
        self.open.argtypes = [gen.PTR, C.POINTER(gen.Generation), C.POINTER(hist.Loaded), C.c_size_t,
                              C.POINTER(gen.PTR), C.POINTER(C.c_int), C.POINTER(C.c_int)]
        self.open.restype = C.c_int
        self.close = self.lib.skse_core_snapshot_close
        self.close.argtypes, self.close.restype = [gen.PTR], None
        self.objects = self.lib.skse_mod_event_objects
        self.objects.argtypes = [gen.PTR, C.POINTER(Summary), ON_OBJECT, ON_ARGUMENT, gen.PTR]
        self.objects.restype = C.c_int

    def inspect(self, store, generation, current, observe=True):
        catalog = (hist.Loaded * len(current))(*(hist.Loaded(n, len(n), i, k) for n, i, k in current))
        view, source, mapping = gen.PTR(), C.c_int(), C.c_int()
        status = self.open(store, C.byref(generation), catalog, len(current), C.byref(view), C.byref(source), C.byref(mapping))
        if status != 0:
            raise AssertionError((status, source.value, mapping.value))
        summary, events, args = Summary(), [], []

        @ON_OBJECT
        def event(_, ptr):
            e = ptr.contents
            events.append((e.index, e.stack, C.string_at(e.name.data, e.name.size), e.arguments))

        @ON_ARGUMENT
        def argument(_, e, i, ptr):
            a = ptr.contents
            args.append((e.contents.index, i, a.kind, a.bits, a.form, a.resolution))

        try:
            result = self.objects(view, C.byref(summary), event if observe else ON_OBJECT(),
                                  argument if observe else ON_ARGUMENT(), None)
            return result, summary, events, args
        finally:
            self.close(view)


def container(slots, filled, version=1):
    return OBMG, version, struct.pack("<II", slots, filled)


def scalar(kind, bits=None):
    return struct.pack("<I", kind) + (b"" if bits is None else struct.pack("<B" if kind == 3 else "<I", bits))


def record(index, stack, event_name, args, *, version=1, class_name=b"PapyrusModEvent", count=None, extra=b""):
    body = struct.pack("<I", len(class_name)) + class_name + cb.string(event_name)
    body += struct.pack("<I", len(args) if count is None else count) + b"".join(args) + extra
    return OBJE, version, body + struct.pack("<II", stack, index)


class ObjectTests(gen.GenerationHarness):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.binding = ObjectBinding(cls.library)

    def setUp(self):
        super().setUp()
        self.mods = (hist.MODS, 0, hist.history(b"Skyrim.esm", b"Owner.esp", b"Missing.esp"))
        self.catalog = [(b"Skyrim.esm", 0, 0), (b"OWNER.ESP", 5, 0)]

    def inspect(self, n, records, prepend=True, observe=True):
        g = self.g(n)
        self.publish(g, gen.image(gen.section(0, ([self.mods] if prepend else []) + records)))
        return self.binding.inspect(self.store, g, self.catalog, observe=observe)

    def test_sparse_slots_and_complete_argument_types(self):
        args = [scalar(0), scalar(1, 0xfffffff9), scalar(2, 0x7fc12345), scalar(3, 1), scalar(4, 0xb3),
                scalar(5, 0x01034567), scalar(5, 0x02001234), scalar(5, 0xff006789)]
        original = [container(9, 2), record(6, 271, b"EventA", args), record(1, 0, b"EventB", []),
                    (DFMG, 1, b""), (OBJE, 1, b"unrelated delayed object")]
        result, summary, objects, values = self.inspect(10, original)
        self.assertEqual((result, summary.slots, summary.filled, summary.arguments, summary.strings, summary.unresolved),
                         (OK, 9, 2, 8, 1, 1))
        self.assertEqual(objects, [(6, 271, b"EventA", 8), (1, 0, b"EventB", 0)])
        self.assertEqual([args[1] for args in values], list(range(8)))
        self.assertEqual([args[3:] for args in values], [
            (0, 0, hist.NOT_READY), (0xfffffff9, 0, hist.NOT_READY), (0x7fc12345, 0, hist.NOT_READY),
            (1, 0, hist.NOT_READY), (0, 0, hist.NOT_READY), (0x01034567, 0x05034567, hist.RESOLVED),
            (0x02001234, 0, hist.MISSING), (0xff006789, 0xff006789, hist.DYNAMIC)])
        rc, stats, no_objects, no_args = self.inspect(11, original, observe=False)
        self.assertEqual((rc, bytes(stats), no_objects, no_args), (OK, bytes(summary), [], []))

    def test_opaque_pointer_words_cannot_change_the_observed_value(self):
        observed = []
        for i, arbitrary in enumerate((0, 1, 0xffffffff), 20):
            rc, stats, _, args = self.inspect(i, [container(1, 1), record(0, 88, b"Q", [scalar(4, arbitrary)])])
            self.assertEqual((rc, stats.strings), (OK, 1))
            observed.append(args)
        self.assertEqual(observed, [[(0, 0, 4, 0, 0, hist.NOT_READY)]] * 3)

    def test_incomplete_or_unrecognized_objects_never_publish_partial_results(self):
        good = record(0, 9, b"Ready", [])
        bad = [(container(3, 2), good, record(0, 1, b"D", []), DUPLICATE),
               (container(3, 2), good, record(3, 1, b"D", []), INVALID),
               (container(3, 2), good, record(1, 1, b"D", [], class_name=b"UnimplementedFactory"), UNKNOWN),
               (container(3, 2), good, record(1, 1, b"D", [], version=9), VERSION),
               (container(3, 2), good, record(1, 1, b"D", [scalar(9)]), UNKNOWN_ARG),
               (container(3, 2), good, record(1, 1, b"D", [scalar(3, 2)]), INVALID),
               (container(3, 2), good, record(1, 1, b"D", [scalar(1)], count=45), INVALID),
               (container(3, 2), good, record(1, 1, b"D", [scalar(5)]), INVALID),
               (container(3, 2), good, record(1, 1, b"D", [], extra=b"X"), INVALID),
               (container(3, 2), good, record(1, 1, b"", []), INVALID),
               (container(3, 2), good, (OBJE, 1, struct.pack("<I", 1025) + bytes(1025)), INVALID),
               (container(3, 2), good, (DFMG, 1, b""), INVALID),
               (container(3, 1), good, good, INVALID)]
        for n, case in enumerate(bad, 30):
            with self.subTest(case=n):
                *items, expected = case
                rc, summary, objects, values = self.inspect(n, items)
                self.assertEqual((rc, bytes(summary), objects, values), (expected, bytes(C.sizeof(summary)), [], []))

    def test_storage_headers_order_empty_sparse_and_limits(self):
        for n, (records, prepend, expected) in enumerate((
            ([], True, MISSING), ([container(1, 0), self.mods], False, ORDER),
            ([container(1, 0, 2)], True, VERSION), ([container(1, 2)], True, INVALID),
            ([(OBMG, 1, bytes(7))], True, INVALID), ([container(65537, 0)], True, LIMIT),
            ([container(2, 1)], True, INVALID), ([container(0, 0), container(0, 0)], True, DUPLICATE),
            ([container(65536, 0)], True, OK),
            ([container(1, 1), record(0, 7, b"Event", [scalar(0)] * 100001)], True, LIMIT)
        ), 60):
            rc, s, objects, args = self.inspect(n, records, prepend=prepend)
            self.assertEqual((rc, s.filled, objects, args), (expected, 0, [], []))


if __name__ == "__main__":
    unittest.main()
