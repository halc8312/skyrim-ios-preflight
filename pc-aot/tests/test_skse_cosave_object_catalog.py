"""Read authored Classic UI-form, spawner and delayed-queue images through the generation store."""
import ctypes as C
import struct
import unittest
from contextlib import contextmanager

import test_skse_cosave_callbacks as cb
import test_skse_cosave_generation as gen
import test_skse_cosave_history as hist
import test_skse_cosave_objects as obj
import test_skse_cosave_ui_objects as ui

U32, U64 = C.c_uint32, C.c_uint64
FUNQ, WLST, END = (int.from_bytes(x, "big") for x in (b"FUNQ", b"WLST", b"____"))
SPAWN = b"SpawnerTaskFunctor"
UIFORM = b"UIInvokeFormDelegate"


class Form(C.Structure):
    _fields_ = [(n, U32) for n in ("index", "stack", "kind")] + [(n, U64) for n in ("saved", "mapped")] + [
        ("resolution", C.c_int), ("menu", cb.Name), ("target", cb.Name)]


class Task(C.Structure):
    _fields_ = [("location", C.c_int)] + [(n, U32) for n in ("index", "owner", "latent", "entries", "has_delay")] + [
        ("delay", U64)]


class Entry(C.Structure):
    _fields_ = [(n, U32) for n in ("saved", "mapped", "target")] + [("resolution", C.c_int),
               ("position", U32 * 3), ("rotation", U32 * 3), ("count", U32),
               ("persist", C.c_uint8), ("disabled", C.c_uint8)]


ON_FORM = C.CFUNCTYPE(None, gen.PTR, C.POINTER(Form))
ON_TASK = C.CFUNCTYPE(None, gen.PTR, C.POINTER(Task))
ON_ENTRY = C.CFUNCTYPE(None, gen.PTR, C.POINTER(Task), U32, C.POINTER(Entry))


class Hooks(C.Structure):
    _fields_ = list(zip(("mod", "argument", "ui", "ui_arg", "form", "task", "entry"),
                        (obj.ON_OBJECT, obj.ON_ARGUMENT, ui.ON_UI, ui.ON_ARG, ON_FORM, ON_TASK, ON_ENTRY)))


class Summary(C.Structure):
    _fields_ = [(n, U32) for n in ("slots", "filled", "events", "callbacks", "forms", "spawns",
                                    "mod_args", "ui_args", "spawn_entries", "strings", "bad_events", "bad_ui", "bad_spawn")]


class Delays(C.Structure):
    _fields_ = [(n, U32) for n in ("queued", "incoming", "waiting", "ready", "entries", "unresolved")]


class Observations:
    def __init__(self):
        self.forms, self.tasks, self.entries = [], [], []
        @ON_FORM
        def form(_, ptr):
            f = ptr.contents
            self.forms.append((f.index, f.stack, f.kind, f.saved, f.mapped, f.resolution,
                               C.string_at(f.menu.data, f.menu.size), C.string_at(f.target.data, f.target.size)))
        @ON_TASK
        def task(_, ptr):
            t = ptr.contents
            self.tasks.append((t.location, t.index, t.owner, t.latent, t.entries, t.has_delay, t.delay))
        @ON_ENTRY
        def entry(_, t, idx, ptr):
            v = ptr.contents
            self.entries.append((t.contents.location, t.contents.index, idx, v.saved, v.mapped, v.resolution,
                                 v.target, list(v.position), list(v.rotation), v.count, v.persist, v.disabled))
        self.hooks = Hooks(form=form, task=task, entry=entry)


class CatalogBinding(obj.ObjectBinding):
    def __init__(self, library):
        super().__init__(library)
        self.catalog = self.lib.skse_core_storage_objects
        self.catalog.argtypes, self.catalog.restype = [gen.PTR, C.POINTER(Summary), C.POINTER(Hooks), gen.PTR], C.c_int
        self.delayed = self.lib.skse_core_delayed_tasks
        self.delayed.argtypes = [gen.PTR, C.POINTER(Delays), ON_TASK, ON_ENTRY, gen.PTR]
        self.delayed.restype = C.c_int

    @contextmanager
    def snapshot(self, store, generation, current):
        loaded = (hist.Loaded * len(current))(*(hist.Loaded(n, len(n), idx, kind) for n, idx, kind in current))
        view, source, mapping = gen.PTR(), C.c_int(), C.c_int()
        status = self.open(store, C.byref(generation), loaded, len(loaded), C.byref(view), C.byref(source), C.byref(mapping))
        if status:
            raise AssertionError((status, source.value, mapping.value))
        try:
            yield view
        finally:
            self.close(view)

    def read(self, view, delayed=False, observe=True):
        seen = Observations()
        if delayed:
            out = Delays()
            rc = self.delayed(view, C.byref(out), seen.hooks.task if observe else ON_TASK(),
                              seen.hooks.entry if observe else ON_ENTRY(), None)
        else:
            out = Summary()
            rc = self.catalog(view, C.byref(out), C.byref(seen.hooks) if observe else None, None)
        return rc, [getattr(out, field) for field, _ in out._fields_], seen


def prefix(cls):
    return struct.pack("<I", len(cls)) + cls


def entry(form, ref=0xdeadbeef, persist=1, disabled=0):
    return struct.pack("<9IBB2s", form, ref, 0x80000000, 0x7fc01234, 7, 3, 4, 5, 0x80000002,
                       persist, disabled, b"\xcc\xa5")


def spawn_body(stack, rows, *, cls=SPAWN, count=None):
    return prefix(cls) + struct.pack("<II", stack, len(rows) if count is None else count) + b"".join(rows)


def form_body(handle):
    return prefix(UIFORM) + cb.string(b"InventoryMenu") + cb.string(b"some.path") + struct.pack("<IQ", 0x42, handle)


def slot(body, number, owner, version=1):
    return obj.OBJE, version, body + struct.pack("<II", owner, number)


def queues(groups):
    """Serialize the count into the last currently open record, as the Classic stream does."""
    queued, incoming, waiting, ready = groups
    rows = [(obj.DFMG, 1, bytearray()), (FUNQ, 1, bytearray(struct.pack("<I", len(queued))))]
    for body, _ in queued:
        rows.append((obj.OBJE, 1, bytearray(body)))
    rows.append((WLST, 1, bytearray()))
    for index, group in enumerate((incoming, waiting, ready)):
        rows[-1][2].extend(struct.pack("<I", len(group)))
        for body, ticks in group:
            value = bytearray(body)
            if index < 2:
                value.extend(struct.pack("<q", ticks))
            rows.append((obj.OBJE, 1, value))
    return [(kind, ver, bytes(data)) for kind, ver, data in rows] + [(END, 1, b"")]


class CatalogTests(gen.GenerationHarness):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.reader = CatalogBinding(cls.library)

    def setUp(self):
        super().setUp()
        self.mods = (hist.MODS, 0, hist.history(b"Skyrim.esm", b"Owner.esp", b"Gone.esp"))
        self.current = [(b"Skyrim.esm", 0, 0), (b"OWNER.ESP", 5, 0)]

    def read(self, number, rows, *, delayed=False, observe=True, prepend=True):
        g = self.g(number)
        image = gen.image(gen.section(0, ([self.mods] if prepend else []) + rows), gen.section(gen.FOREIGN, []))
        self.publish(g, image)
        with self.reader.snapshot(self.store, g, self.current) as view:
            return self.reader.read(view, delayed, observe)

    def test_four_classes_relocation_raw_engine_reference_and_float_bits(self):
        saved, missing, dynamic = 0x7512345601000042, 0x3344556602000021, 0x12345678ffabcdcf
        rows = [obj.container(8, 6), obj.record(1, 7, b"e", [obj.scalar(5, 0x02000123)]),
                ui.ui(0, 31, b"M", b"f", [ui.ui_arg(4, 0xbad1234)]),
                slot(form_body(saved), 2, 9), slot(form_body(missing), 4, 13), slot(form_body(dynamic), 6, 15),
                slot(spawn_body(65, [entry(0x01001234), entry(0x02001234), entry(0xff112233)]), 7, 5)]
        rc, sums, seen = self.read(1, rows)
        self.assertEqual((rc, sums), (obj.OK, [8, 6, 1, 1, 3, 1, 1, 1, 3, 1, 1, 1, 1]))
        self.assertEqual([r[:6] for r in seen.forms], [(2, 9, 0x42, saved, 0x7512345605000042, hist.RESOLVED),
             (4, 13, 0x42, missing, 0, hist.MISSING), (6, 15, 0x42, dynamic, dynamic, hist.DYNAMIC)])
        self.assertEqual([r[6:] for r in seen.forms], [(b"InventoryMenu", b"some.path")] * 3)
        self.assertEqual(seen.tasks, [(0, 7, 5, 65, 3, 0, 0)])
        self.assertEqual([r[:7] for r in seen.entries], [(0, 7, 0, 0x01001234, 0x05001234, hist.RESOLVED, 0xdeadbeef),
            (0, 7, 1, 0x02001234, 0, hist.MISSING, 0xdeadbeef), (0, 7, 2, 0xff112233, 0xff112233, hist.DYNAMIC, 0xdeadbeef)])
        self.assertEqual([r[7:] for r in seen.entries], [([0x80000000, 0x7fc01234, 7], [3, 4, 5], 0x80000002, 1, 0)] * 3)
        self.assertEqual(self.read(2, rows, observe=False)[:2], (rc, sums))

    def test_invalid_storage_is_all_or_nothing(self):
        good = slot(spawn_body(71, [entry(0x01000001)]), 0, 6)
        invalid = [(slot(form_body(1)[:-1], 1, 1), obj.INVALID), (slot(form_body(1) + b"X", 1, 1), obj.INVALID),
                   (slot(spawn_body(1, [entry(0, persist=2)]), 1, 2), obj.INVALID),
                   (slot(spawn_body(1, [entry(0, disabled=3)]), 1, 2), obj.INVALID),
                   (slot(spawn_body(1, [], count=100001), 1, 2), obj.LIMIT),
                   (slot(spawn_body(1, [], count=1), 1, 2), obj.INVALID),
                   (slot(spawn_body(1, [], cls=b"UnknownFactory"), 1, 2), obj.UNKNOWN),
                   (slot(spawn_body(1, []), 1, 2, 2), obj.VERSION), (slot(form_body(1), 0, 7), obj.DUPLICATE)]
        for i, (bad, expected) in enumerate(invalid, 8):
            with self.subTest(case=i):
                rc, counts, seen = self.read(i, [obj.container(2, 2), good, bad])
                self.assertEqual((rc, sum(counts), seen.tasks, seen.entries, seen.forms), (expected, 0, [], [], []))

    def test_all_empty_and_nonempty_wait_group_boundaries(self):
        body = spawn_body(87, [entry(0x02000123)])
        for mask in range(16):
            with self.subTest(mask=mask):
                groups = [[(body, -100 - index), (body, 7)] if mask & (1 << index) else [] for index in range(4)]
                rows = queues(groups)
                rc, counts, seen = self.read(32 + mask, rows + [(obj.OBJE, 9, b"other-manager")], delayed=True)
                n = sum(map(len, groups))
                self.assertEqual((rc, counts), (obj.OK, list(map(len, groups)) + [n, n]))
                expected = [(i + 1, j, 0, 87, 1, int(i in (1, 2)), (ticks % (1 << 64) if i in (1, 2) else 0))
                            for i, group in enumerate(groups) for j, (_, ticks) in enumerate(group)]
                self.assertEqual(seen.tasks, expected)
                self.assertTrue(all(x[2:7] == (0, 0x02000123, 0, hist.MISSING, 0xdeadbeef) for x in seen.entries))
                self.assertEqual(len(seen.entries), n)

    def test_missing_truncated_nonclassic_or_excessive_delays_never_emit_early_rows(self):
        one = spawn_body(4, [entry(0x01000123)])
        plain = queues([[(one, 0)], [], [], []])
        wait = queues([[], [(one, -17)], [(one, 96)], [(one, 0)]])
        odd = [(plain[:-1], obj.INVALID), ([], obj.MISSING), (plain + plain, obj.DUPLICATE),
               ([(obj.DFMG, 1, b"X")] + plain[1:], obj.INVALID), ([plain[0], (FUNQ, 2, b"")] + plain[2:], obj.VERSION),
               ([plain[0], (FUNQ, 1, struct.pack("<I", 65537))] + plain[2:], obj.LIMIT),
               (plain[:1] + [(WLST, 1, b"")], obj.INVALID), (plain[:2] + [(obj.OBMG, 1, one)] + plain[3:], obj.INVALID),
               (plain[:2] + [(obj.OBJE, 1, spawn_body(4, [], cls=b"Other"))] + plain[3:], obj.UNKNOWN),
               (plain[:2] + [(obj.OBJE, 1, one + b"X")] + plain[3:], obj.INVALID),
               (wait[:3] + [(obj.OBJE, 1, one + b"x")] + wait[4:], obj.INVALID),
               (wait[:-1] + [(END, 1, b"extra")], obj.INVALID), (wait[:-1] + [(END, 2, b"")], obj.VERSION),
               (plain[:-2] + [(WLST, 1, struct.pack("<III", 0, 0, 1)), (END, 1, b"")], obj.INVALID)]
        for i, (rows, expected) in enumerate(odd, 65):
            with self.subTest(case=i):
                rc, counts, seen = self.read(i, rows, delayed=True)
                self.assertEqual((rc, sum(counts), seen.tasks, seen.entries), (expected, 0, [], []))
        self.assertEqual(self.read(90, [plain[0], self.mods, *plain[1:]], prepend=False, delayed=True)[0], obj.ORDER)


if __name__ == "__main__":
    unittest.main()
