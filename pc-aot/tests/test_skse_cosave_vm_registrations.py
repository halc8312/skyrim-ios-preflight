"""A separately loaded synthetic game, Classic registration rows, and an epoch-aware fake VM."""
import ctypes as C
from concurrent.futures import ThreadPoolExecutor
import os
import struct
import unittest
import test_skse_cosave_bridge as br
import test_skse_cosave_callbacks as cb
import test_skse_cosave_generation as gen
import test_skse_cosave_history as hist
import test_skse_cosave_menu as menu
import test_skse_cosave_opened_load as opened
import test_skse_cosave_registrations as reg

P, Ptr, U64 = C.POINTER, gen.PTR, C.c_uint64
OK, ARG, BUSY, OCCUPIED, BRIDGE, CORE, FORMAT, LIMIT, MEMORY, POLICY, UNAVAILABLE, RETIRED, INACTIVE, WRONG_LOAD = range(14)
UNKNOWN_WORLD, CURRENT_WORLD, RETIRED_WORLD = range(3)
ERROR, RETAINED, MISSING = range(3)
LOADED = C.CFUNCTYPE(C.c_int, Ptr, P(br.ID), P(br.Event))
WORLD = C.CFUNCTYPE(C.c_int, Ptr, P(br.ID))
TOKEN = C.CFUNCTYPE(C.c_int, Ptr, P(br.ID), cb.Name, P(U64))
RETAIN = C.CFUNCTYPE(C.c_int, Ptr, P(br.ID), U64)
RELEASE = C.CFUNCTYPE(None, Ptr, P(br.ID), U64)
VISIT = C.CFUNCTYPE(None, Ptr, C.c_int, cb.Key, cb.Name, U64)


class Policy(C.Structure):
    _fields_ = [("ctx", Ptr), ("loaded", LOADED), ("world", WORLD), ("name", TOKEN), ("retain", RETAIN), ("release", RELEASE)]


class Report(C.Structure):
    _fields_ = [(n, C.c_int) for n in ("bridge", "pair", "map", "format")] + [
        (n, C.c_uint32) for n in ("rows", "missing_mod", "duplicates", "missing_handle", "retained", "retained_bytes")]


class Binding:
    def __init__(self, lib):
        signatures = {
            "vm_registrations_create": ([P(Policy), P(Ptr)], C.c_int),
            "bridge_load_registrations": ([Ptr, P(br.Event), P(br.Trace), P(br.Evidence), P(hist.Loaded), C.c_size_t,
                                           Ptr, P(Report)], C.c_int),
            "bridge_prepare_registrations": ([P(Ptr), P(br.Trace), P(br.Evidence), P(hist.Loaded), C.c_size_t,
                                               P(Ptr), P(Report)], C.c_int),
            "vm_registrations_apply": ([Ptr, P(Ptr), P(Report)], C.c_int),
            "vm_registrations_drop": ([P(Ptr)], None),
            "vm_registrations_clear": ([Ptr], C.c_int), "vm_registrations_close": ([P(Ptr)], C.c_int),
            "vm_registrations_visit": ([Ptr, VISIT, Ptr], C.c_int),
        }
        for name, (args, out) in signatures.items():
            fn = getattr(lib, "skse_" + name)
            fn.argtypes, fn.restype = args, out
            setattr(self, name, fn)


class FixtureVM:
    def __init__(self, binding):
        self.api = binding
        self.epoch = br.ID(*bytes(range(16)))
        self.loaded = None
        self.result = CURRENT_WORLD
        self.ops, self.live, self.errors = [], [], []
        self.tokens = {}
        self.miss, self.fail = set(), set()
        self.fail_name = set()
        self.on_retain = self.on_release = None

        @LOADED
        def loaded(_, epoch, event):
            self.ops.append("loaded")
            if not self.loaded:
                return 0
            epoch[0], event[0] = self.loaded
            return 1

        @WORLD
        def world(_, epoch):
            self.ops.append("world")
            return self.result if bytes(epoch[0]) == bytes(self.epoch) else RETIRED_WORLD

        @TOKEN
        def token(_, epoch, name, out):
            text = C.string_at(name.data, name.size)
            self.ops.append(("name", text))
            if text in self.fail_name:
                return 0
            # The stub elects to intern ASCII spellings together. Production must use its VM policy.
            value = self.tokens.setdefault(text.lower(), len(self.tokens) + 7)
            out[0] = value
            return 1

        @RETAIN
        def retain(_, epoch, value):
            self.ops.append(("retain", value))
            if bytes(epoch[0]) != bytes(self.epoch) or self.result != CURRENT_WORLD:
                self.errors.append("retained stale handle")
                return ERROR
            if self.on_retain:
                self.on_retain(value)
            if value in self.fail:
                return ERROR
            if value in self.miss:
                return MISSING
            self.live.append(value)
            return RETAINED

        @RELEASE
        def release(_, epoch, value):
            self.ops.append(("release", value))
            if bytes(epoch[0]) != bytes(self.epoch) or self.result != CURRENT_WORLD or value not in self.live:
                self.errors.append("released stale/unowned handle")
                return
            self.live.remove(value)
            if self.on_release:
                self.on_release(value)

        self.policy = Policy(None, loaded, world, token, retain, release)
        self.state = Ptr()
        assert binding.vm_registrations_create(C.byref(self.policy), C.byref(self.state)) == OK

    def clear(self):
        return self.api.vm_registrations_clear(self.state)

    def close(self):
        return self.api.vm_registrations_close(C.byref(self.state))

    def visit(self, hook=None):
        obtained = []
        @VISIT
        def visit(_, family, key, callback, handle):
            obtained.append((family, C.string_at(key.name.data, key.name.size) if key.kind == 1 else key.number,
                             C.string_at(callback.data, callback.size), handle))
            if hook:
                hook()
        return self.api.vm_registrations_visit(self.state, visit, None), obtained

    def retire(self):
        self.result = RETIRED_WORLD
        self.live.clear()  # The fake VM owner discarded its reference table first.

    def confirm_load(self, obs):
        self.loaded = br.ID.from_buffer_copy(self.epoch), br.Event.from_buffer_copy(obs.requested)


class ActivationTests(br.BridgeHarness):
    extra_sources = br.BridgeHarness.extra_sources + ("-DSKSE_REGISTRATION_TEST_ALLOC", gen.SOURCE / "registration_activation.c")

    def setUp(self):
        super().setUp()
        self.bind = Binding(self.api.lib)
        self.vm = FixtureVM(self.bind)
        self.addCleanup(self.assertEqual, self.vm.errors, [])
        self.addCleanup(self.vm.close)
        rc, self.raw = self.native.store(self.private / "text-en-voice-en")
        self.assertEqual(rc, gen.OK)
        self.addCleanup(self.native.close, self.raw)
        self.mods = (hist.MODS, 0, hist.history(b"Skyrim.esm", b"Owner.esp", b"Missing.esp"))
        self.current = (hist.Loaded * 2)(hist.Loaded(b"Skyrim.esm", 10, 0, 0), hist.Loaded(b"OWNER.ESP", 9, 5, 0))
        self.end = (menu.REGE, 1, b"")

    def image(self, n, rows, with_mods=True, foreign_bytes=b""):
        g = self.g(n)
        core = gen.section(0, ([self.mods] if with_mods else []) + rows)
        data = gen.image(core, gen.section(gen.FOREIGN, [(1, 1, foreign_bytes)])) if foreign_bytes else gen.image(core)
        rc, stage = self.native.stage(g.key, data)
        self.assertEqual(rc, gen.OK)
        self.addCleanup(self.native.discard, stage)
        self.assertEqual(self.native.commit(self.raw, stage, C.byref(g)), gen.OK)

    def activate(self, observation, bridge=None):
        report = Report()
        code = self.bind.bridge_load_registrations(bridge or self.bridge, C.byref(observation.requested),
            C.byref(observation.trace()), C.byref(observation.base), self.current, len(self.current),
            self.vm.state, C.byref(report))
        return code, report

    def completed_load(self, n, flags=0):
        observation = self.observe(self.action(n, br.LOAD, flags))
        self.vm.confirm_load(observation)
        return observation

    def test_valid_load_uses_per_key_first_subscription_and_per_epoch_refs(self):
        a, b, target = 0xabbc000001000012, 0xabbc0000ff000103, 0xabbc000005000012
        self.image(7, [(cb.MCBR, 1, b""), cb.group(b"MyEvent", [(a, b"First"), (a, b"Second"), (0x02000123, b"Lost")]),
                       cb.group(b"myevent", [(a, b"Third")]), cb.group(b"AnotherEvent", [(a, b"Separate")]), self.end,
                       (reg.code(6), 1, reg.handles(a, b)), (reg.code(7), 1, reg.handles(a))])
        self.vm.miss.add(b)
        obs = self.completed_load(7, br.NESTED_REVERT)
        result, report = self.activate(obs)
        self.assertEqual((result, report.rows, report.missing_mod, report.duplicates, report.missing_handle, report.retained),
                         (OK, 7, 1, 2, 1, 3))
        self.assertEqual(self.vm.visit(), (OK, [(1, b"MyEvent", b"First", target), (1, b"AnotherEvent", b"Separate", target),
                                               (6, 0, b"", target)]))
        self.assertEqual(self.activate(obs)[0], OCCUPIED)
        self.assertEqual(self.vm.live, [target, target, target])
        self.assertEqual(self.vm.clear(), OK)
        self.assertEqual(self.vm.live, [])
        self.assertEqual(self.vm.clear(), OK)
        self.assertEqual(self.vm.visit(), (INACTIVE, []))

    def test_load_vm_outer_revert_proof_and_pair_must_all_agree_before_any_policy_call(self):
        self.image(9, [(reg.code(5), 1, reg.handles(1))])
        cases = [(br.VM_FAIL, br.LOADED, br.REJECTED), (br.SLOT_FAIL, br.LOADED, br.REJECTED),
                 (0, br.FAILED, br.REJECTED), (0, br.NONE, br.NEEDS_BASE), (br.RELOAD, br.LOADED, br.TRACE),
                 (br.NO_REVERT, br.LOADED, br.TRACE), (128, br.LOADED, br.TRACE)]
        for flags, proof, expected in cases:
            with self.subTest(flags=flags, proof=proof):
                result, r = self.activate(self.observe(self.action(9, br.LOAD, flags, proof)))
                self.assertEqual((result, r.bridge, self.vm.ops), (BRIDGE, expected, []))
        result, r = self.activate(self.observe(self.action(10, br.LOAD)))
        self.assertEqual((result, r.pair, self.vm.ops), (BRIDGE, gen.MISSING, []))
        wrong = self.root / "another.ess"
        wrong.write_bytes(b"unrelated fixture")
        with wrong.open("rb") as f:
            result, r = self.activate(self.observe(self.action(9, br.LOAD, fd=f.fileno())))
            self.assertEqual((result, r.pair, self.vm.ops), (BRIDGE, gen.WRONG, []))
        obs = self.observe(self.action(9, br.LOAD))
        result, r = self.activate(obs, self.session(br.scope(text=1)))
        self.assertEqual((result, r.bridge, self.vm.ops), (BRIDGE, br.SCOPE, []))

    def test_all_family_validation_total_limit_and_unrecognized_writer_tag(self):
        valid = [(reg.code(5), 1, reg.handles(1, 2))]
        self.image(11, valid + [(reg.code(6), 1, b"truncated")])
        result, r = self.activate(self.observe(self.action(11, br.LOAD)))
        self.assertEqual((result, r.format, self.vm.ops), (FORMAT, menu.INVALID, []))
        self.image(12, valid, with_mods=False)
        result, r = self.activate(self.observe(self.action(12, br.LOAD)))
        self.assertEqual((result, r.map, self.vm.ops), (CORE, hist.NO_HISTORY, []))
        max_group = struct.pack("<I", 65536) + bytes(65536 * 8)
        self.image(13, valid + [(reg.code(6), 1, max_group)])
        self.assertEqual((self.activate(self.observe(self.action(13, br.LOAD)))[0], self.vm.ops), (LIMIT, []))
        self.image(14, [(reg.code(7), 1, b"opaque ignored writer tag")])
        result, r = self.activate(self.completed_load(14))
        self.assertEqual((result, r.retained, self.vm.visit()), (OK, 0, (OK, [])))

    def test_failed_partial_load_returns_references_in_reverse_without_publishing(self):
        self.image(17, [(reg.code(5), 1, reg.handles(1, 2, 3, 4))])
        self.vm.fail.add(3)
        obs = self.completed_load(17)
        result, r = self.activate(obs)
        self.assertEqual((result, r.retained, self.vm.live, self.vm.visit()), (POLICY, 0, [], (INACTIVE, [])))
        self.assertEqual([x for x in self.vm.ops if isinstance(x, tuple) and x[0] == "release"],
                         [("release", 2), ("release", 1)])
        self.vm.fail.clear()
        self.assertEqual(self.activate(obs)[0], OK)
        self.assertEqual(self.vm.live, [1, 2, 3, 4])

    def test_world_retired_before_and_during_release_or_temporarily_unavailable(self):
        self.image(19, [(reg.code(5), 1, reg.handles(1, 2, 3))])
        obs = self.completed_load(19)
        self.assertEqual(self.activate(obs)[0], OK)
        self.vm.on_release = lambda _: setattr(self.vm, "result", UNKNOWN_WORLD)
        self.assertEqual(self.vm.clear(), UNAVAILABLE)
        self.assertEqual(self.vm.live, [1, 2])
        self.assertEqual(self.vm.close(), UNAVAILABLE)
        self.assertTrue(self.vm.state.value)
        self.assertEqual(self.activate(obs)[0], OCCUPIED)
        self.assertEqual(self.vm.visit(), (INACTIVE, []))
        self.vm.retire()
        self.assertEqual(self.vm.clear(), OK)
        self.assertEqual([x for x in self.vm.ops if isinstance(x, tuple) and x[0] == "release"], [("release", 3)])
        self.assertEqual(self.activate(obs)[0], RETIRED)
        self.vm.result, self.vm.on_release = CURRENT_WORLD, None
        self.vm.epoch = br.ID(*bytes(range(1, 17)))
        self.assertEqual(self.activate(obs)[0], RETIRED)
        new_load = self.completed_load(19)
        self.assertEqual(self.activate(obs)[0], WRONG_LOAD)
        obs = new_load
        self.assertEqual(self.activate(obs)[0], OK)
        self.vm.retire()
        self.assertEqual(self.vm.visit(), (RETIRED, []))
        self.assertEqual(self.vm.close(), OK)
        self.assertFalse(self.vm.state.value)

    def test_failed_acquisition_retains_cleanup_obligation_until_policy_returns(self):
        self.image(21, [(reg.code(5), 1, reg.handles(1, 2, 3))])
        self.vm.fail = {2}
        self.vm.on_retain = lambda h: setattr(self.vm, "result", UNKNOWN_WORLD) if h == 2 else None
        obs = self.completed_load(21)
        self.assertEqual(self.activate(obs)[0], UNAVAILABLE)
        self.assertEqual(self.vm.live, [1])
        self.assertEqual(self.vm.visit(), (INACTIVE, []))
        self.assertEqual(self.activate(obs)[0], OCCUPIED)
        self.vm.result, self.vm.on_retain = CURRENT_WORLD, None
        self.assertEqual(self.vm.clear(), OK)
        self.assertEqual(self.vm.live, [])

    def test_reentrancy_and_missing_canonical_name_cannot_leave_references(self):
        self.image(23, [(cb.MCBR, 1, b""), cb.group(b"Authored", [(1, b"OnEvent"), (2, b"Other")]), self.end])
        obs = self.completed_load(23)
        self.vm.fail_name.add(b"Authored")
        self.assertEqual(self.activate(obs)[0], POLICY)
        self.assertEqual(self.vm.live, [])
        self.vm.fail_name.clear()
        nested = []
        self.vm.on_retain = lambda _: nested.append((self.vm.clear(), self.activate(obs)[0], self.vm.close()))
        self.assertEqual(self.activate(obs)[0], OK)
        self.assertEqual(nested, [(BUSY, BUSY, BUSY)] * 2)
        seen = []
        self.assertEqual(self.vm.visit(lambda: seen.append(self.vm.clear()))[0], OK)
        self.assertEqual(seen, [BUSY, BUSY])

    def test_vm_must_independently_identify_the_loaded_scope_slot_generation_and_ticket(self):
        self.image(27, [(reg.code(5), 1, reg.handles(1))])
        obs = self.observe(self.action(27, br.LOAD))
        self.assertEqual(self.activate(obs)[0], UNAVAILABLE)
        for key in ("ticket", "slot", "event", "profile", "launch", "text", "voices"):
            with self.subTest(key=key):
                self.vm.confirm_load(obs)
                event = self.vm.loaded[1]
                if key == "ticket":
                    event.ticket += 1
                elif key in ("slot", "event"):
                    getattr(event.saved, key)[0] ^= 1
                elif key in ("profile", "launch"):
                    getattr(event.scope, key)[0] ^= 1
                else:
                    setattr(event.scope, key, 1)
                self.assertEqual(self.activate(obs)[0], WRONG_LOAD)
                self.assertEqual(self.vm.live, [])
        self.vm.confirm_load(obs)
        self.vm.loaded = br.ID(), self.vm.loaded[1]
        self.assertEqual(self.activate(obs)[0], UNAVAILABLE)

    def test_large_foreign_owner_and_duplicate_subscriptions_do_not_remain_resident(self):
        event_name, callback = b"AuthoredEvent", b"OnRestore"
        rows = [(cb.MCBR, 1, b""), cb.group(event_name, [(1, callback)] * 1000), self.end]
        self.image(30, rows, foreign_bytes=b"v" * (8 * 1024 * 1024))
        status, r = self.activate(self.completed_load(30))
        self.assertEqual((status, r.duplicates, r.retained), (OK, 999, 1))
        self.assertGreater(r.retained_bytes, len(event_name) + len(callback))
        self.assertLess(r.retained_bytes, 512)
        self.assertEqual(self.vm.visit(), (OK, [(1, event_name, callback, 1)]))

    def test_name_storage_allocation_failure_after_retain_releases_every_reference(self):
        rows = [(cb.MCBR, 1, b""), cb.group(b"SomeEvent", [(1, b"First"), (2, b"Second")]), self.end]
        self.image(32, rows)
        obs = self.completed_load(32)
        fail = self.api.lib.skse_vm_test_fail_name_allocation
        fail.argtypes, fail.restype = [], None
        fail()
        status, report = self.activate(obs)
        self.assertEqual((status, report.retained, self.vm.live, self.vm.visit()), (MEMORY, 0, [], (INACTIVE, [])))
        self.assertEqual([x for x in self.vm.ops if isinstance(x, tuple) and x[0] == "release"],
                         [("release", 2), ("release", 1)])
        self.assertEqual(self.activate(obs)[0], OK)



class Scope(C.Structure):
    _fields_ = [("epoch", U64), ("registry", C.c_uint32)]


class DeferredActivationTests(opened.OpenedLoadHarness):
    extra_sources = (*br.BridgeHarness.extra_sources, gen.SOURCE / "registration_activation.c",
                     gen.SOURCE.parents[1] / "ios/runtime-port/classic_vm_epoch.c", "-pthread")

    def setUp(self):
        super().setUp()
        self.bind = Binding(self.api.lib)
        signatures = {
            "registered": ([C.c_uint32], None), "before_revert": ([C.c_uint], None),
            "after_load": ([C.c_int], None), "scope_current": ([P(Scope)], C.c_bool),
            "commit_begin": ([P(Scope)], C.c_bool), "commit_end": ([], None),
        }
        self.epoch = {}
        for name, (args, result) in signatures.items():
            prefix = "skyrim_vm_" if name.startswith(("scope_", "commit_")) else "skyrim_vm_epoch_"
            fn = getattr(self.api.lib, prefix + name)
            fn.argtypes, fn.restype = args, result
            self.epoch[name] = fn
        self.epoch["registered"](0x12340000)
        self.epoch["after_load"](1)
        self.scope = Scope()
        self.assertTrue(self.epoch["scope_current"](C.byref(self.scope)))

    def prepared(self, *, flags=0, invalid=False):
        prefix = hist.history(b"Skyrim.esm", b"Owner.esp")
        name, handle = b"Strong Name", 0xabcd000001001234
        data = gen.image(gen.section(0, [(hist.MODS, 0, prefix), (cb.MCBR, 1, b""),
            cb.group(name, [(handle, b"OnFoo"), (handle, b"OnOther")]), (menu.REGE, 1, b"")]))
        rc, store = self.native.store(self.private / "text-en-voice-en")
        self.assertEqual(rc, gen.OK)
        try:
            g = self.g(10)
            rc, stage = self.native.stage(g.key, data)
            self.assertEqual(rc, gen.OK)
            try:
                self.assertIn(self.native.commit(store, stage, C.byref(g)), (gen.OK, gen.ALREADY))
            finally:
                self.native.discard(stage)
        finally:
            self.native.close(store)
        rc, detail, bridge = self.api.open_at(self.private, self.running)
        self.assertEqual((rc, detail), (br.OK, gen.OK))
        original, self.bridge = self.bridge, bridge
        try:
            with self.sealed_input() as (source, _):
                start, pending, observed = self.guest_load(source.fileno(), flags=flags)
                self.assertEqual(start, (br.OK, gen.OK))
            mods = (hist.Loaded * 2)(hist.Loaded(b"Skyrim.esm", 10, 0, 0), hist.Loaded(b"Owner.esp", 9, 5, 0))
            proof = observed.base
            if invalid:
                proof.ess_fd = self.fd
            hashes = self.deferred.hashes()
            out, report = Ptr(), Report()
            result = self.bind.bridge_prepare_registrations(C.byref(pending), C.byref(observed.trace()),
                C.byref(proof), mods, len(mods), C.byref(out), C.byref(report))
            self.assertFalse(pending)
            self.assertEqual(self.deferred.hashes(), hashes + (not flags and not invalid))
            return result, out, observed, report
        finally:
            self.bridge = original
            self.api.close(bridge)

    def test_copied_names_cross_to_vm_owner_after_bridge_close_and_guard_release(self):
        outcome, prepared, observed, report = self.prepared()
        self.assertEqual((outcome, report.rows, report.retained), (OK, 2, 0))
        self.addCleanup(self.bind.vm_registrations_drop, C.byref(prepared))
        hashes = self.deferred.hashes()

        def vm_owner():
            vm = FixtureVM(self.bind)
            try:
                # The production guard protects the handoff decision. Release it before policy calls.
                self.assertTrue(self.epoch["commit_begin"](C.byref(self.scope)))
                self.epoch["commit_end"]()
                vm.confirm_load(observed)
                seen = []

                def reopen_epoch(_):
                    actual = Scope()
                    ready = self.epoch["scope_current"](C.byref(actual))
                    seen.append((ready, actual.epoch, actual.registry))

                vm.on_retain = reopen_epoch
                result = self.bind.vm_registrations_apply(vm.state, C.byref(prepared), C.byref(report))
                self.assertEqual((result, report.duplicates, report.retained, bool(prepared)), (OK, 1, 1, False))
                self.assertEqual(seen, [(True, self.scope.epoch, self.scope.registry)])
                self.assertEqual(vm.visit(), (OK, [(1, b"Strong Name", b"OnFoo", 0xabcd000005001234)]))
                self.assertEqual(self.bind.vm_registrations_apply(vm.state, C.byref(prepared), None), ARG)
                self.assertEqual(vm.errors, [])
            finally:
                self.assertEqual(vm.close(), OK)

        with ThreadPoolExecutor(max_workers=1) as worker:
            worker.submit(vm_owner).result(10)
        self.assertEqual(self.deferred.hashes(), hashes)

    def test_retired_scope_and_another_completed_load_reject_old_preparation(self):
        outcome, prepared, previous, _ = self.prepared()
        self.assertEqual(outcome, OK)
        self.addCleanup(self.bind.vm_registrations_drop, C.byref(prepared))
        self.epoch["before_revert"](1)
        self.epoch["after_load"](1)
        accepted = self.epoch["commit_begin"](C.byref(self.scope))
        if accepted:
            self.epoch["commit_end"]()
        self.assertFalse(accepted)
        next_scope = Scope()
        self.assertTrue(self.epoch["scope_current"](C.byref(next_scope)))
        self.assertNotEqual(next_scope.epoch, self.scope.epoch)
        now = self.observe(self.action(10, br.LOAD))
        self.assertNotEqual(previous.requested.ticket, now.requested.ticket)
        vm = FixtureVM(self.bind)
        self.addCleanup(vm.close)
        vm.confirm_load(now)
        self.assertEqual(self.bind.vm_registrations_apply(vm.state, C.byref(prepared), None), WRONG_LOAD)
        self.assertFalse(prepared)
        self.assertEqual((vm.ops, vm.live), (["loaded"], []))

    def test_unconsumed_descriptor_or_failed_vm_returns_no_prepared_records(self):
        for flags, invalid, detail in ((br.VM_FAIL, False, br.REJECTED), (0, True, br.PAIR)):
            with self.subTest(flags=flags, invalid=invalid):
                result, prepared, _, report = self.prepared(flags=flags, invalid=invalid)
                self.assertEqual((result, prepared.value, report.bridge), (BRIDGE, None, detail))

if __name__ == "__main__":
    unittest.main()
