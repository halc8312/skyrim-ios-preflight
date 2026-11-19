"""Classic stream semantics, real generation store and a separately loaded authored guest."""
import ctypes as C
import os
import unittest
import test_skse_cosave_bridge as br
import test_skse_cosave_generation as gen
import test_skse_cosave_history as history

Ptr, P, U32, U64 = gen.PTR, C.POINTER, C.c_uint32, C.c_uint64
CALL = C.CFUNCTYPE(C.c_int, Ptr, Ptr)
A, B, NEW, CLEARED, PASSIVE = (0x9a000000 + i for i in range(5))


class Plugin(C.Structure):
    _fields_ = [("handle", U32), ("owns", U32), ("uid", U32), ("state", Ptr),
                ("save", CALL), ("load", CALL), ("reset", gen.REVERT)]


class PluginAPI:
    def __init__(self, lib):
        types = {
            "plugins_open": ([P(Plugin), C.c_size_t, P(Ptr)], C.c_int),
            "plugins_close": ([Ptr], None),
            "plugins_reset_scratch": ([Ptr], C.c_int),
            "plugins_stage": ([Ptr, Ptr, P(C.c_size_t)], C.c_int),
            "plugins_load": ([Ptr, Ptr, P(gen.Generation), P(history.Loaded), C.c_size_t, P(C.c_int)], C.c_int),
            "record_open": ([Ptr, U32, U32], C.c_int),
            "record_append": ([Ptr, Ptr, U32], C.c_int),
            "record_write": ([Ptr, U32, U32, Ptr, U32], C.c_int),
            "record_next": ([Ptr, P(U32), P(U32), P(U32)], C.c_int),
            "record_read": ([Ptr, Ptr, U32], U32),
            "record_resolve_form": ([Ptr, U32, P(U32)], C.c_int),
            "record_resolve_handle": ([Ptr, U64, P(U64)], C.c_int),
            "pair_test_ess_hash_count": ([], C.c_uint),
            "bridge_begin_save": ([Ptr, P(br.Event), P(gen.Generation), P(Ptr), P(C.c_int)], C.c_int),
            "bridge_write_plugins": ([Ptr, Ptr, P(C.c_size_t)], C.c_int),
            "bridge_load_plugins": ([Ptr, P(br.Event), P(br.Trace), P(br.Evidence), Ptr, P(history.Loaded),
                                     C.c_size_t, P(C.c_int), P(C.c_int)], C.c_int),
        }
        for name, (args, result) in types.items():
            fn = getattr(lib, "skse_" + name)
            fn.argtypes, fn.restype = args, result
            setattr(self, name, fn)

    def create(self, rows):
        result = Ptr()
        table = (Plugin * len(rows))(*rows)
        status = self.plugins_open(table, len(rows), C.byref(result))
        return status, result

    def save(self, registry, stage):
        count = C.c_size_t(1000)
        status = self.plugins_stage(registry, stage, C.byref(count))
        return status, count.value

    def load(self, registry, store, generation, current=()):
        mods = (history.Loaded * len(current))(*(history.Loaded(name, len(name), index, kind)
                                                for name, index, kind in current))
        reason = C.c_int(-1)
        status = self.plugins_load(registry, store, C.byref(generation), mods, len(mods), C.byref(reason))
        return status, reason.value

    def next(self, r):
        t, v, n = U32(), U32(), U32()
        return (t.value, v.value, n.value) if self.record_next(r, C.byref(t), C.byref(v), C.byref(n)) else None

    def read(self, r, size):
        out = C.create_string_buffer(size)
        n = self.record_read(r, out, size)
        return out.raw[:n]


class PluginTests(br.BridgeHarness):
    def setUp(self):
        super().setUp()
        self.p = PluginAPI(self.api.lib)
        self.callbacks = []
        self.problems = []
        self.addCleanup(self.assertEqual, self.problems, [])

    def data_stage(self, k, data):
        rc, stage = self.native.stage(k, data)
        self.assertEqual(rc, gen.OK)
        self.addCleanup(self.native.discard, stage)
        return stage

    def own(self, handle, uid, save=None, load=None, reset=None):
        def callback(fn):
            if not fn:
                return CALL()

            @CALL
            def checked(_, stream):
                try:
                    return int(fn(stream))
                except BaseException as failure:
                    self.problems.append(str(failure))
                    return 0
            return checked

        def reset_checked(_):
            try:
                reset()
            except BaseException as failure:
                self.problems.append(str(failure))

        owned = Plugin(handle, uid is not None, uid or 0, None, callback(save), callback(load),
                       gen.REVERT(reset_checked) if reset else gen.REVERT())
        self.callbacks.append(owned)
        return owned

    def registry(self, *descriptors):
        rc, r = self.p.create(descriptors)
        self.assertEqual(rc, gen.OK)
        self.addCleanup(self.p.plugins_close, r)
        return r

    def verify_publish(self, g, data):
        stage = self.data_stage(g.key, data)
        self.assertEqual(self.native.commit(self.store, stage, C.byref(g)), gen.OK)

    def test_multiple_owners_stream_to_exact_file_without_repacking_opaque_sections(self):
        called = []
        def save_a(w):
            called.append(A)
            self.assertTrue(self.p.record_open(w, 7, 3))
            self.assertTrue(self.p.record_append(w, b"data\0", 5))
            self.assertTrue(self.p.record_append(w, b"\xff!", 2))
            self.assertTrue(self.p.record_write(w, 7, 4, None, 0))
            return 1
        def write(uid, data):
            def callback(w):
                called.append(uid)
                return self.p.record_write(w, 20, 0xfedcba98, data, len(data))
            return callback
        def empty(_):
            called.append(CLEARED)
            return 1
        def forbidden(_):
            self.fail("save without an assigned UID")
        blob = bytes(range(256)) * 257
        reg = self.registry(self.own(90, A, save_a), self.own(72, B, write(B, blob)),
                            self.own(102, None, forbidden), self.own(10, NEW, write(NEW, b"new")),
                            self.own(3, PASSIVE), self.own(240, CLEARED, empty))
        untouched = [gen.section(gen.FOREIGN, [(22, 88, b"unknown\0")]), gen.section(PASSIVE, [(0, 43, b"keep")]),
                     gen.section(0, [(0xfeed, 0xbad, b"unrecognized core")])]
        original = gen.image(untouched[0], gen.section(B, [(7, 1, b"B")]), untouched[1],
                             gen.section(CLEARED, [(10, 11, b"explicit clearing")]), untouched[2],
                             gen.section(A, [(6, 1, b"A")]), skse=0x10203040)
        g = self.g(50)
        stage = self.data_stage(g.key, original)
        self.assertEqual(self.p.save(reg, stage), (gen.OK, 4))
        self.assertEqual(called, [NEW, B, A, CLEARED])
        self.assertEqual(self.native.commit(self.store, stage, C.byref(g)), gen.OK)
        final = gen.image(untouched[0], gen.section(B, [(20, 0xfedcba98, blob)]), untouched[1], untouched[2],
                          gen.section(A, [(7, 3, b"data\0\xff!"), (7, 4, b"")]),
                          gen.section(NEW, [(20, 0xfedcba98, b"new")]), skse=0x10203040)
        self.assertEqual(self.native.data(self.store, g), (gen.OK, final))
        self.assertEqual(self.p.save(reg, stage), (gen.SEALED, 0))

    def test_wire_order_absent_order_partial_reads_remainder_skip_own_map_and_one_hash(self):
        order, events = [], []
        names = history.history(b"Base.esm", b"Moved.esp", b"Gone.esp")
        data = gen.image(gen.section(B, [(22, 5, b"123456"), (33, 0, b"xyz")]),
                         gen.section(0, [(history.MODS, 0, names)]), gen.section(A, [(71, 9, b"")]),
                         gen.section(gen.FOREIGN, [(99, 1, b"cannot leak to the absent handlers")]))
        g = self.g(60)
        self.verify_publish(g, data)
        def receive_b(r):
            order.append(B)
            self.assertEqual(self.p.next(r), (22, 5, 6))
            self.assertEqual(self.p.read(r, 2), b"12")
            self.assertEqual(self.p.next(r), (33, 0, 3))
            self.assertEqual(self.p.read(r, 2), b"xy")
            self.assertEqual(self.p.read(r, 20), b"z")
            self.assertEqual(self.p.read(r, 1), b"")
            self.assertIsNone(self.p.next(r))
            out, handle = U32(66), U64(77)
            self.assertEqual(self.p.record_resolve_form(r, 0x01010203, C.byref(out)), history.RESOLVED)
            self.assertEqual(out.value, 0xfe010203)
            self.assertEqual(self.p.record_resolve_form(r, 0x02010203, C.byref(out)), history.MISSING)
            self.assertEqual(out.value, 0xfe010203)
            self.assertEqual(self.p.record_resolve_handle(r, 0xcafebabe01020304, C.byref(handle)), history.RESOLVED)
            self.assertEqual(handle.value, 0xcafebabefe020304)
            return 1
        def receive_a(r):
            order.append(A)
            self.assertEqual(self.p.next(r), (71, 9, 0))
            self.assertEqual(self.p.read(r, 5), b"")
            return 1
        def missing(n):
            def f(r):
                order.append(n)
                self.assertIsNone(self.p.next(r))
                self.assertIsNone(self.p.next(r))
                return 1
            return f
        reg = self.registry(self.own(40, A, load=receive_a, reset=lambda: events.append(40)),
                            self.own(90, B, load=receive_b, reset=lambda: events.append(90)),
                            self.own(7, NEW, load=missing(7), reset=lambda: events.append(7)),
                            self.own(20, None, load=missing(20), reset=lambda: events.append(20)))
        hashes = self.p.pair_test_ess_hash_count()
        self.assertEqual(self.p.load(reg, self.store, g, [(b"base.esm", 0, 0), (b"moved.ESP", 254, 0)]),
                         (gen.OK, history.MAP_OK))
        self.assertEqual(self.p.pair_test_ess_hash_count(), hashes + 1)
        self.assertEqual(order, [B, A, 7, 20])
        self.assertEqual(events, [7, 20, 40, 90])

    def test_callback_abort_invalid_calls_and_checked_limits_poison_the_whole_stage(self):
        def ignore_append_without_open(w):
            self.assertEqual(self.p.record_append(w, None, 0), 0)
            return 1
        def huge(w):
            self.assertEqual(self.p.record_write(w, 1, 1, b"x", 0xffffffff), 0)
            return 1
        cases = [(lambda _: 0, gen.STOPPED), (ignore_append_without_open, gen.ARG), (huge, gen.LIMIT)]
        prior = self.g(1)
        self.verify_publish(prior, gen.image(gen.section(A, [(1, 0, b"previous")])))
        for i, (bad, reason) in enumerate(cases):
            reg = self.registry(self.own(4, A, lambda w: self.p.record_write(w, 1, 2, b"new", 3)), self.own(5, B, bad))
            g = self.g(30 + i)
            rc, stage = self.native.create(self.store, g.key, prior)
            self.assertEqual(rc, gen.OK)
            self.addCleanup(self.native.discard, stage)
            self.assertEqual(self.p.save(reg, stage), (reason, 0))
            self.assertEqual(self.native.pin(stage, C.byref(g)), reason)
            self.assertEqual(self.native.data(self.store, g), (gen.MISSING, None))
        self.assertEqual(self.native.data(self.store, prior)[1], gen.image(gen.section(A, [(1, 0, b"previous")])))

    def test_combined_file_header_limit_counts_preserved_owners_and_new_chunks(self):
        original = gen.image(gen.section(gen.FOREIGN, [(1, 0, b"")] * 99999))
        g = self.g(6)
        stage = self.data_stage(g.key, original)
        registry = self.registry(self.own(5, A, lambda w: self.p.record_open(w, 15, 2)))
        self.assertEqual(self.p.save(registry, stage), (gen.LIMIT, 0))
        self.assertEqual(self.native.pin(stage, C.byref(g)), gen.LIMIT)

    def test_missing_base_invalid_read_and_late_stop_clear_every_private_receiver(self):
        first, second, callbacks = [b"stale"], [b"stale"], []
        g = self.g(12)
        def load_a(r):
            callbacks.append(A)
            self.assertEqual(self.p.next(r), (1, 2, 3))
            first.append(self.p.read(r, 5))
            return 1
        def load_b(r):
            callbacks.append(B)
            second.append(b"partial")
            self.assertEqual(self.p.read(r, 2), b"")
            return 1
        registry = self.registry(self.own(5, A, load=load_a, reset=first.clear),
                                 self.own(2, B, load=load_b, reset=second.clear))
        self.assertEqual(self.p.load(registry, self.store, g), (gen.MISSING, history.SOURCE))
        self.assertEqual((first, second, callbacks), ([], [], []))
        self.verify_publish(g, gen.image(gen.section(A, [(1, 2, b"new")])))
        self.assertEqual(self.p.load(registry, self.store, g), (gen.ARG, history.NO_HISTORY))
        self.assertEqual((first, second, callbacks), ([], [], [A, B]))
        callbacks.clear()
        self.ess.write_bytes(b"same-name other generation")
        self.assertEqual(self.p.load(registry, self.store, g), (gen.WRONG, history.SOURCE))
        self.assertEqual((first, second, callbacks), ([], [], []))

    def test_missing_history_dynamic_form_is_explicit_and_invalid_catalog_aborts_before_load(self):
        observed = []
        g = self.g(5)
        self.verify_publish(g, gen.image(gen.section(A, [])))
        def receive(r):
            out = U32(19)
            observed.append((self.p.record_resolve_form(r, 0xff123456, C.byref(out)), out.value))
            observed.append((self.p.record_resolve_form(r, 0x02123456, C.byref(out)), out.value))
            return 1
        reg = self.registry(self.own(4, A, load=receive, reset=observed.clear))
        self.assertEqual(self.p.load(reg, self.store, g), (gen.OK, history.NO_HISTORY))
        self.assertEqual(observed, [(history.DYNAMIC, 0xff123456), (history.NOT_READY, 0xff123456)])
        bad = [(b"One.esp", 3, 0), (b"Another.esp", 3, 0)]
        self.assertEqual(self.p.load(reg, self.store, g, bad), (gen.ARG, history.BAD_CURRENT))
        self.assertEqual(observed, [])

    def test_handle_ownership_reentry_and_no_receivers(self):
        for descriptors, expected in (((self.own(1, A), self.own(3, A)), gen.DUPLICATE),
                                      ((self.own(2, A), self.own(2, B)), gen.ARG),
                                      ((self.own(0xffffffff, A),), gen.ARG), ((self.own(1, 0),), gen.ARG),
                                      ((self.own(2, A, load=lambda _: 1),), gen.ARG)):
            rc, value = self.p.create(descriptors)
            self.assertEqual(rc, expected)
            self.assertFalse(value)
        g = self.g(56)
        stage = self.data_stage(g.key, gen.image())
        zero = self.registry()
        hashes = self.p.pair_test_ess_hash_count()
        self.assertEqual(self.p.save(zero, stage), (gen.OK, 0))
        self.assertEqual(self.p.load(zero, self.store, g), (gen.NO_OWNER, history.SOURCE))
        self.assertEqual(self.p.pair_test_ess_hash_count(), hashes)
        def check_busy(w):
            self.assertEqual(self.p.plugins_reset_scratch(working), gen.BUSY)
            self.assertEqual(self.p.save(working, stage), (gen.BUSY, 0))
            self.assertEqual(self.p.save(zero, stage), (gen.BUSY, 0))
            self.assertEqual(self.native.pin(stage, C.byref(g)), gen.BUSY)
            self.assertEqual(self.native.edit(stage, A, []), gen.BUSY)
            return self.p.record_write(w, 3, 3, None, 0)
        working = self.registry(self.own(1, B, check_busy))
        self.assertEqual(self.p.save(working, stage), (gen.OK, 1))
        self.assertEqual(self.native.commit(self.store, stage, C.byref(g)), gen.OK)

    def test_host_guest_vm_window_and_independent_base_control_dispatch(self):
        seen, marks = [], []
        payload = b"nested state"
        def read(r):
            seen.append(self.p.next(r))
            seen.append(self.p.read(r, len(payload)))
            return 1
        plugins = self.registry(self.own(4, A, save=lambda w: self.p.record_write(w, 3, 5, payload, len(payload)),
                                         load=read, reset=seen.clear))
        pending, obs, detail = Ptr(), br.Observation(), C.c_int()
        @br.NOTICE
        def in_guest(_, event, vm):
            if vm:
                n = C.c_size_t()
                marks.append((1, self.p.bridge_write_plugins(pending, plugins, C.byref(n)), n.value))
            else:
                rc = self.p.bridge_begin_save(self.bridge, event, None, C.byref(pending), C.byref(detail))
                marks.append((0, rc, detail.value))
        self.assertTrue(self.api.fixture_host_observe_with_notice(self.host, C.byref(self.action(88)),
                                                                 C.byref(obs), in_guest, None))
        self.addCleanup(self.api.drop, pending)
        self.addCleanup(os.close, obs.base.ess_fd)
        self.assertEqual(marks, [(0, br.OK, gen.OK), (1, gen.OK, 1)])
        self.assertEqual(self.api.capture(pending, obs), (br.OK, gen.OK))
        self.assertEqual(self.api.publish_in(pending, self.running), (br.OK, gen.OK))
        for flags, expected in ((0, br.OK), (br.NO_REVERT, br.TRACE)):
            loaded = self.observe(self.action(88, br.LOAD, flags=flags))
            mapping = C.c_int()
            result = self.p.bridge_load_plugins(self.bridge, C.byref(loaded.requested), C.byref(loaded.trace()),
                                                C.byref(loaded.base), plugins, None, 0,
                                                C.byref(mapping), C.byref(detail))
            self.assertEqual(result, expected)
            self.assertEqual(seen, [(3, 5, len(payload)), payload] if not flags else [])


if __name__ == "__main__":
    unittest.main()
