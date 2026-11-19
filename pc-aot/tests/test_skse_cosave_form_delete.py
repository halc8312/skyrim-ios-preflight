"""Route authoritative post-cleanup VM deletions to locally registered synthetic callbacks."""
import ctypes as C
import threading
import unittest
import test_skse_cosave_bridge as br
import test_skse_cosave_generation as gen
import test_skse_cosave_plugin_streams as ps

OK, ARG, BUSY, UNARMED, OCCUPIED, PENDING, WRONG_WORLD, GAP, INCOMPLETE, SOURCE = range(10)
P, Ptr, U64 = C.POINTER, gen.PTR, C.c_uint64
DELETE = C.CFUNCTYPE(None, Ptr, U64)


class Position(C.Structure):
    _fields_ = [("epoch", br.ID), ("sequence", U64)]


class Event(C.Structure):
    _fields_ = [("position", Position), ("handle", U64), ("phase", C.c_int)]


POSITION = C.CFUNCTYPE(C.c_int, Ptr, P(Position))
EVENT = C.CFUNCTYPE(C.c_int, Ptr, U64, P(Event))


class Source(C.Structure):
    _fields_ = [("state", Ptr), ("position", POSITION), ("event", EVENT)]


class DeletionTests(gen.GenerationHarness):
    def setUp(self):
        super().setUp()
        self.lib = C.CDLL(str(self.library))
        specs = {"open": ([P(ps.Plugin), C.c_size_t, P(Ptr)], C.c_int), "try_close": ([P(Ptr)], C.c_int),
                 "set_form_delete": ([Ptr, C.c_uint32, DELETE], C.c_int),
                 "begin_deletions": ([Ptr, P(Source)], C.c_int), "stop_deletions": ([Ptr], C.c_int),
                 "next_deletion": ([Ptr, P(C.c_size_t)], C.c_int)}
        for name, (args, result) in specs.items():
            f = getattr(self.lib, "skse_plugins_" + name)
            f.argtypes, f.restype = args, result
            setattr(self, name, f)
        self.registry = Ptr()
        # Intentionally register out of order, include a plugin with no UID.
        plugins = (ps.Plugin * 3)(ps.Plugin(handle=9, owns=1, uid=1, state=9), ps.Plugin(handle=2, state=2),
                                  ps.Plugin(handle=7, owns=1, uid=3, state=7))
        self.assertEqual(self.open(plugins, 3, C.byref(self.registry)), gen.OK)
        self.addCleanup(self.try_close, C.byref(self.registry))
        self.epoch = br.ID(*bytes(range(16)))
        self.high = 6
        self.available = True
        self.events, self.reads, self.seen, self.keep = {}, [], [], []

        @POSITION
        def position(_, dest):
            if not self.available:
                return 0
            dest[0] = Position(self.epoch, self.high)
            return 1

        @EVENT
        def event(_, sequence, dest):
            self.reads.append(sequence)
            if sequence not in self.events:
                return 0
            dest[0] = self.events[sequence]
            return 1
        self.source = Source(None, position, event)

    def arm(self):
        return self.begin_deletions(self.registry, C.byref(self.source))

    def next(self):
        count = C.c_size_t(9999)
        return self.next_deletion(self.registry, C.byref(count)), count.value

    def add(self, handle, tag=None, fn=None):
        @DELETE
        def receive(state, h):
            self.seen.append((state, tag, h))
            if fn:
                fn()
        self.keep.append(receive)
        return self.set_form_delete(self.registry, handle, receive)

    def observed(self, sequence, handle, phase=2):
        self.high = max(self.high, sequence)
        self.events[sequence] = Event(Position(self.epoch, sequence), handle, phase)

    def test_observed_sleep_cleanup_and_real_post_core_event_in_handle_order(self):
        word = 0xfedcba98ff324567
        for handle in (7, 9, 2):
            self.assertEqual(self.add(handle, handle), gen.OK)
        self.assertEqual(self.next(), (UNARMED, 0))
        self.assertEqual(self.arm(), OK)
        self.assertEqual(self.arm(), OCCUPIED)
        self.assertEqual(self.next(), (PENDING, 0))
        self.observed(7, word, 0)
        self.assertEqual(self.next(), (INCOMPLETE, 0))
        self.events[7].phase = 1
        self.assertEqual(self.next(), (INCOMPLETE, 0))
        self.assertEqual(self.seen, [])
        self.events[7].phase = 2
        self.assertEqual(self.next(), (OK, 3))
        self.assertEqual(self.seen, [(2, 2, word), (7, 7, word), (9, 9, word)])
        self.assertEqual(self.next(), (PENDING, 0))
        self.assertEqual(self.reads, [7, 7, 7])
        self.assertEqual(self.add(2, "new"), gen.OK)
        self.assertEqual(self.set_form_delete(self.registry, 7, DELETE()), gen.OK)
        self.observed(8, 0)
        self.assertEqual(self.next(), (OK, 2))
        self.assertEqual(self.seen[-2:], [(2, "new", 0), (9, 9, 0)])
        self.observed(9, 0)
        self.assertEqual(self.next(), (OK, 2))
        self.assertEqual(self.seen[-4:], [(2, "new", 0), (9, 9, 0)] * 2)

    def test_source_refusal_gaps_and_later_epochs_require_separate_baselines(self):
        self.assertEqual(self.add(2), gen.OK)
        self.available = False
        self.assertEqual(self.arm(), SOURCE)
        self.available = True
        self.assertEqual(self.arm(), OK)
        self.observed(9, 9)
        self.assertEqual(self.next(), (SOURCE, 0))
        self.events[7] = self.events[9]
        self.assertEqual(self.next(), (GAP, 0))
        self.observed(8, 8)
        self.observed(7, 7)
        self.assertEqual([self.next() for _ in range(3)], [(OK, 1)] * 3)
        self.assertEqual([x[2] for x in self.seen], [7, 8, 9])
        self.high = 1
        self.assertEqual(self.next(), (GAP, 0))
        self.epoch = br.ID(*bytes(range(1, 17)))
        self.assertEqual(self.next(), (WRONG_WORLD, 0))
        self.assertEqual(self.stop_deletions(self.registry), OK)
        self.assertEqual(self.arm(), OK)
        self.observed(2, 200)
        self.events[2].position.epoch = br.ID(*bytes(range(16)))
        self.assertEqual(self.next(), (WRONG_WORLD, 0))
        self.events[2].position.epoch = self.epoch
        self.assertEqual(self.next(), (OK, 1))

    def test_no_callbacks_unsupported_source_and_counter_limits(self):
        self.assertEqual(self.add(0), gen.ARG)
        self.assertEqual(self.add(55), gen.ARG)
        self.assertEqual(self.set_form_delete(self.registry, 7, DELETE()), gen.OK)
        epoch = self.epoch
        self.epoch = br.ID()
        self.assertEqual(self.arm(), SOURCE)
        self.epoch = epoch
        self.high = (1 << 64) - 2
        self.assertEqual(self.arm(), OK)
        self.observed((1 << 64) - 1, 0)
        self.assertEqual(self.next(), (OK, 0))
        self.assertEqual(self.next(), (PENDING, 0))
        self.high = 0
        self.assertEqual(self.next(), (GAP, 0))
        self.available = False
        self.assertEqual(self.next(), (SOURCE, 0))
        self.assertEqual(self.stop_deletions(self.registry), OK)
        self.assertEqual(self.next(), (UNARMED, 0))

    def test_duplicate_registration_during_callback_and_concurrent_dispatch_are_busy(self):
        arrived, resume = threading.Event(), threading.Event()
        inner = []
        def callback():
            self.lib.skse_plugins_close.argtypes = [Ptr]
            self.lib.skse_plugins_close.restype = None
            self.lib.skse_plugins_close(self.registry)
            inner.append((self.next(), self.stop_deletions(self.registry), self.arm(), self.try_close(C.byref(self.registry))))
            arrived.set()
            resume.wait(3)
        self.assertEqual(self.add(7, "x", callback), gen.OK)
        self.assertEqual(self.arm(), OK)
        self.observed(7, 700)
        result = []
        th = threading.Thread(target=lambda: result.append(self.next()))
        th.start()
        try:
            self.assertTrue(arrived.wait(3))
            self.assertEqual(self.set_form_delete(self.registry, 7, DELETE()), gen.BUSY)
            self.assertEqual(self.next(), (BUSY, 0))
            self.assertEqual(self.try_close(C.byref(self.registry)), gen.BUSY)
            self.assertTrue(self.registry)
        finally:
            resume.set()
            th.join(5)
        self.assertFalse(th.is_alive())
        self.assertEqual(inner, [((BUSY, 0), BUSY, BUSY, gen.BUSY)])
        self.assertEqual((result, self.seen), ([(OK, 1)], [(7, "x", 700)]))
        self.assertEqual(self.try_close(C.byref(self.registry)), gen.OK)
        self.assertFalse(self.registry)


if __name__ == "__main__":
    unittest.main()
