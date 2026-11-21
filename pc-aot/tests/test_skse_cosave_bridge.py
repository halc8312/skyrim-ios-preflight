"""Authored dynamic guest, real lifecycle observer and a generation-aware native host."""
import ctypes as C
import concurrent.futures
import os
from pathlib import Path
import shlex
import subprocess
import sys
import threading
import unittest
import test_skse_cosave_generation as gen

OK, ARG, SCOPE, EVENT, TRACE, REJECTED, NEEDS_BASE, NO_EDITS, STATE, PAIR, IO = range(11)
NONE, FAILED, SAVED, LOADED = range(4)
SAVE, LOAD = 1, 2
VM_FAIL, SLOT_FAIL, NO_REVERT, NO_VM, RELOAD, NESTED_REVERT, NO_RETURN = (1 << n for n in range(7))
ID = C.c_uint8 * 16
PTR = gen.PTR
P = C.POINTER


class Scope(C.Structure):
    _fields_ = [("launch", ID), ("profile", ID), ("text", C.c_uint32), ("voices", C.c_uint32)]


def scope(launch=111, profile=1, text=0, voices=0):
    return Scope(ID(*launch.to_bytes(16, "big")), ID(*profile.to_bytes(16, "big")), text, voices)


class Event(C.Structure):
    _fields_ = [("scope", Scope), ("saved", gen.Key), ("ticket", C.c_uint32)]


NOTICE = C.CFUNCTYPE(None, PTR, P(Event), C.c_uint32)


class Row(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("site", "returning", "outcome", "ticket", "parent")]


class Trace(C.Structure):
    _fields_ = [("launch", ID), ("rows", P(Row)), ("count", C.c_size_t), ("dropped", C.c_uint64)]


class Evidence(C.Structure):
    _fields_ = [("event", Event), ("outcome", C.c_int), ("ess_fd", C.c_int)]


class Action(C.Structure):
    _fields_ = [("scope", Scope), ("saved", gen.Key), ("site", C.c_uint32), ("options", C.c_uint32),
                ("outcome", C.c_int), ("ess_fd", C.c_int)]


class Observation(C.Structure):
    _fields_ = [("requested", Event), ("base", Evidence), ("trace_launch", ID), ("rows", Row * 256),
                ("count", C.c_size_t), ("dropped", C.c_uint64)]

    def trace(self):
        return Trace(self.trace_launch, self.rows, self.count, self.dropped)


class Binding:
    def __init__(self, library):
        self.lib = C.CDLL(str(library))
        table = {
            "skse_bridge_open": ([C.c_char_p, P(Scope), P(PTR), P(C.c_int)], C.c_int),
            "skse_bridge_close": ([PTR], None),
            "skse_bridge_bind_named_ess": ([PTR, C.c_uint32, C.c_int, C.c_char_p, C.c_int, P(Event),
                                           P(C.c_int)], C.c_int),
            "skse_bridge_drop": ([PTR], None),
            "skse_bridge_check_call": ([P(Event), C.c_uint32, P(Trace)], C.c_int),
            "skse_bridge_capture_save": ([PTR, P(Trace), P(Evidence), P(C.c_int)], C.c_int),
            "skse_bridge_publish": ([PTR, P(Scope), P(C.c_int)], C.c_int),
            "skse_bridge_load": ([PTR, P(Event), P(Trace), P(Evidence), C.c_uint32, gen.REVERT, gen.VISIT,
                                  PTR, P(C.c_int)], C.c_int),
            "fixture_host_open": ([C.c_char_p, C.c_uint32, P(C.c_uint8)], PTR),
            "fixture_host_close": ([PTR], None),
            "fixture_host_act": ([PTR, P(Action), P(Observation)], C.c_int),
            "fixture_host_observe_with_notice": ([PTR, P(Action), P(Observation), NOTICE, PTR], C.c_int),
            "fixture_host_stage": ([PTR, PTR, P(Action), P(gen.Generation), C.c_uint32, P(gen.Record), C.c_size_t,
                                    P(PTR), P(Observation), P(C.c_int)], C.c_int),
        }
        for name, (args, result) in table.items():
            fun = getattr(self.lib, name)
            fun.argtypes, fun.restype = args, result
            setattr(self, name.removeprefix("skse_bridge_"), fun)

    def open_at(self, root, running):
        bridge, detail = PTR(), C.c_int(-1)
        result = self.open(os.fsencode(root), C.byref(running), C.byref(bridge), C.byref(detail))
        return result, detail.value, bridge

    def stage(self, host, bridge, action, payload, uid=gen.UID, base=None):
        observation, pending, detail = Observation(), PTR(), C.c_int(-1)
        record_data = C.create_string_buffer(payload)
        record = gen.Record(11, 2, len(payload), C.cast(record_data, PTR))
        result = self.fixture_host_stage(host, bridge, C.byref(action), C.byref(base) if base else None,
                                         uid, C.byref(record), 1, C.byref(pending),
                                         C.byref(observation), C.byref(detail))
        return result, detail.value, pending, observation

    def capture(self, pending, observation, evidence=None, trace=None):
        detail = C.c_int(-1)
        t = trace if trace is not None else observation.trace()
        result = self.capture_save(pending, C.byref(t),
                                   C.byref(observation.base if evidence is None else evidence), C.byref(detail))
        return result, detail.value

    def publish_in(self, pending, running):
        detail = C.c_int(-1)
        result = self.publish(pending, C.byref(running), C.byref(detail))
        return result, detail.value

    def restore(self, bridge, observation, uid=gen.UID, stop=False):
        scratch, detail = [b"stale"], C.c_int(-1)
        reset = gen.REVERT(lambda _: scratch.clear())

        def read(_, ptr):
            record = ptr.contents
            scratch.append(C.string_at(record.data, record.length))
            return int(not stop)

        result = self.load(bridge, C.byref(observation.requested), C.byref(observation.trace()),
                           C.byref(observation.base), uid, reset, gen.VISIT(read), None, C.byref(detail))
        return result, detail.value, scratch


def crash_child(library, guest, root, ess, event, mode):
    binding = Binding(library)
    running = scope(launch=900 + event)
    bound = binding.fixture_host_open(os.fsencode(guest), 1, running.launch)
    rc, _, bridge = binding.open_at(root, running)
    if not bound or rc != OK:
        return 1
    with open(ess, "rb") as source:
        action = Action(running, gen.key(event), SAVE, 0, SAVED, source.fileno())
        rc, _, pending, observed = binding.stage(bound, bridge, action, bytes(1024 * 1024))
        if rc != OK or binding.capture(pending, observed) != (OK, gen.OK):
            return 2
        if mode == "base-only":
            os._exit(80)
        os.environ["SKSE_TEST_CRASH"] = mode
        binding.publish_in(pending, running)
    return 3


class BridgeHarness(gen.GenerationHarness):
    extra_sources = (gen.SOURCE / "event_bridge.c", gen.SOURCE / "event_trace.c",
                     gen.SOURCE / "fixtures/host.c", *([] if sys.platform == "darwin" else ["-ldl"]))

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.bridge_api = Binding(cls.library)
        cls.guest = cls.library.parent / "authored-guest.so"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-pedantic", "-Wall", "-Wextra", "-Werror",
                        *shlex.split(os.environ.get("CFLAGS", "")), "-fPIC",
                        "-dynamiclib" if sys.platform == "darwin" else "-shared",
                        "-DSKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE=1", gen.SOURCE / "fixtures/guest.c",
                        gen.SOURCE / "observer/lifecycle.c", *shlex.split(os.environ.get("LDFLAGS", "")),
                        "-o", cls.guest], check=True)

    def setUp(self):
        super().setUp()
        self.api = self.bridge_api
        self.running = scope()
        self.private = self.root / "bridge-private"
        self.private.mkdir(mode=0o700)
        self.assertFalse(self.api.fixture_host_open(os.fsencode(self.guest), 2, self.running.launch))
        self.host = self.api.fixture_host_open(os.fsencode(self.guest), 1, self.running.launch)
        self.assertTrue(self.host)
        self.addCleanup(self.api.fixture_host_close, self.host)
        self.bridge = self.session(self.running)

    def session(self, running):
        rc, detail, bridge = self.api.open_at(self.private, running)
        self.assertEqual((rc, detail), (OK, gen.OK))
        self.addCleanup(self.api.close, bridge)
        return bridge

    def action(self, n=10, site=SAVE, flags=0, final=None, fd=None, running=None):
        run = running or self.running
        key = gen.key(n, profile=int.from_bytes(run.profile, "big"))
        return Action(run, key, site, flags, final if final is not None else (SAVED if site == SAVE else LOADED),
                      self.fd if fd is None else fd)

    def stage(self, action, data=b"new state", uid=gen.UID, base=None, bridge=None):
        rc, detail, pending, observed = self.api.stage(self.host, bridge or self.bridge, action, data, uid, base)
        self.assertEqual((rc, detail), (OK, gen.OK))
        self.addCleanup(self.api.drop, pending)
        if observed.base.ess_fd >= 0:
            self.addCleanup(os.close, observed.base.ess_fd)
        return pending, observed

    def observe(self, action):
        observed = Observation()
        self.assertEqual(self.api.fixture_host_act(self.host, C.byref(action), C.byref(observed)), 1)
        if observed.base.ess_fd >= 0:
            self.addCleanup(os.close, observed.base.ess_fd)
        return observed

    def saved(self, n=10, data=b"new state", base=None):
        pending, obs = self.stage(self.action(n), data, base=base)
        self.assertEqual(self.api.capture(pending, obs), (OK, gen.OK))
        self.assertEqual(self.api.publish_in(pending, self.running), (OK, gen.OK))
        return pending, obs


class BridgeTests(BridgeHarness):
    def test_named_file_binding_survives_a_new_launch_and_rejects_replacement(self):
        game = self.root / "named-saves"
        game.mkdir()
        path = game / "QuickSave.ess"
        path.write_bytes(b"same base bytes")
        folder = os.open(game, os.O_RDONLY | os.O_DIRECTORY)
        self.addCleanup(os.close, folder)
        next_launch = self.session(scope(launch=222))
        other_profile = self.session(scope(launch=333, profile=2))

        def bind(bridge, fd, leaf=b"QuickSave.ess"):
            event, detail = Event(), C.c_int(-1)
            result = self.api.bind_named_ess(bridge, 3, folder, leaf, fd, C.byref(event), C.byref(detail))
            if result != OK:
                self.assertEqual(bytes(event), bytes(Event()))
            return result, detail.value, event

        with path.open("rb") as known:
            initial = bind(self.bridge, known.fileno())
            relaunched = bind(next_launch, known.fileno())
            different = bind(other_profile, known.fileno())
            self.assertEqual(initial[:2], (OK, gen.OK))
            self.assertEqual(relaunched[:2], (OK, gen.OK))
            self.assertEqual(different[:2], (OK, gen.OK))
            self.assertEqual(bytes(initial[2].saved), bytes(relaunched[2].saved))
            self.assertNotEqual(bytes(initial[2].scope.launch), bytes(relaunched[2].scope.launch))
            self.assertNotEqual(bytes(initial[2].saved.slot), bytes(different[2].saved.slot))
            self.assertEqual(bind(self.bridge, known.fileno(), b"../QuickSave.ess")[0], ARG)
            with path.open("r+b") as writable:
                self.assertEqual(bind(self.bridge, writable.fileno())[0], ARG)
            alias = game / "Alias.ess"
            alias.symlink_to(path.name)
            self.assertEqual(bind(self.bridge, known.fileno(), b"Alias.ess")[:2], (PAIR, gen.CHANGED))
            fresh = game / "Replacement.ess"
            fresh.write_bytes(path.read_bytes())
            fresh.replace(path)
            self.assertEqual(bind(next_launch, known.fileno())[:2], (PAIR, gen.CHANGED))

        with path.open("rb") as replaced:
            current = bind(next_launch, replaced.fileno())
            self.assertEqual(current[:2], (OK, gen.OK))
            self.assertEqual(bytes(initial[2].saved.slot), bytes(current[2].saved.slot))
            self.assertNotEqual(bytes(initial[2].saved.event), bytes(current[2].saved.event))

    def test_nested_guest_callback_and_unknown_owner_survive(self):
        foreign = gen.section(gen.FOREIGN, [(1240, 9, b"arbitrary foreign bytes\xff")])
        wire = gen.image(foreign, gen.section(gen.UID, [(11, 2, b"previous")]))
        offered = self.root / "example.skse"
        offered.write_bytes(wire)
        raw_dir = self.private / "text-en-voice-en"
        rc, raw = self.native.store(raw_dir)
        self.assertEqual(rc, gen.OK)
        self.addCleanup(self.native.close, raw)
        base = gen.Generation(gen.key(1), self.fd)
        rc, initial = self.native.stage(base.key, offered.read_bytes())
        self.assertEqual(rc, gen.OK)
        self.addCleanup(self.native.discard, initial)
        self.assertEqual(self.native.commit(raw, initial, C.byref(base)), gen.OK)
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(1, site=LOAD))), (OK, gen.OK, [b"previous"]))
        pending, saved = self.saved(2, base=base)
        self.assertEqual(self.api.publish_in(pending, self.running), (OK, gen.ALREADY))
        self.assertEqual(self.api.capture(pending, saved), (STATE, gen.OK))
        actual = self.native.data(raw, gen.Generation(gen.key(2), self.fd))
        self.assertEqual(actual, (gen.OK, gen.image(foreign, gen.section(gen.UID, [(11, 2, b"new state")]))))
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(2, LOAD, NESTED_REVERT))),
                         (OK, gen.OK, [b"new state"]))
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(2, LOAD)), stop=True),
                         (PAIR, gen.STOPPED, []))
        self.assertEqual((offered.read_bytes(), self.ess.read_bytes()), (wire, self.original))

    def test_save_requires_independent_terminal_proof_and_exact_descriptor(self):
        pending, obs = self.stage(self.action(final=NONE))
        self.assertEqual(self.api.capture(pending, obs), (NEEDS_BASE, gen.OK))
        self.assertEqual(self.api.publish_in(pending, self.running), (STATE, gen.OK))
        bad = Evidence(obs.requested, LOADED, self.fd)
        self.assertEqual(self.api.capture(pending, obs, bad), (NEEDS_BASE, gen.OK))
        bad.outcome, bad.ess_fd = SAVED, -1
        self.assertEqual(self.api.capture(pending, obs, bad), (NEEDS_BASE, gen.OK))
        with self.ess.open("rb+") as writable:
            bad.ess_fd = writable.fileno()
            self.assertEqual(self.api.capture(pending, obs, bad), (PAIR, gen.ARG))
        with self.ess.open("rb") as trusted:
            good = Evidence(obs.requested, SAVED, trusted.fileno())
            self.assertEqual(self.api.capture(pending, obs, good), (OK, gen.OK))
        self.assertEqual(self.api.publish_in(pending, self.running), (OK, gen.OK))
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(site=LOAD))), (OK, gen.OK, [b"new state"]))

    def test_failed_saves_cannot_be_rehabilitated_by_a_later_positive_hint(self):
        for event, flags, final in ((31, VM_FAIL, SAVED), (32, 0, FAILED)):
            pending, obs = self.stage(self.action(event, flags=flags, final=final))
            self.assertEqual(self.api.capture(pending, obs), (REJECTED, gen.OK))
            positive = Evidence(obs.requested, SAVED, self.fd)
            self.assertEqual(self.api.capture(pending, obs, positive), (STATE, gen.OK))
            self.assertEqual(self.api.publish_in(pending, self.running), (STATE, gen.OK))
            self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(event, LOAD))), (PAIR, gen.MISSING, []))
        pending, obs = self.stage(self.action(33), uid=0)
        self.assertEqual(self.api.capture(pending, obs), (NO_EDITS, gen.OK))
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(33, LOAD))), (PAIR, gen.MISSING, []))

    def test_load_must_have_correct_revert_success_and_selected_base(self):
        self.saved()
        for flags, final, expected in ((VM_FAIL, LOADED, REJECTED), (SLOT_FAIL, LOADED, REJECTED),
                                      (0, FAILED, REJECTED), (0, NONE, NEEDS_BASE), (0, SAVED, NEEDS_BASE),
                                      (NO_VM, LOADED, TRACE), (NO_REVERT, LOADED, TRACE),
                                      (RELOAD, LOADED, TRACE), (NO_RETURN, LOADED, TRACE)):
            with self.subTest(flags=flags, final=final):
                obs = self.observe(self.action(site=LOAD, flags=flags, final=final))
                self.assertEqual(self.api.restore(self.bridge, obs), (expected, gen.OK, []))

    def test_quicksave_replacement_keeps_tickets_and_generations_separate(self):
        old, old_obs = self.stage(self.action(30), b"first")
        other = self.root / "second.ess"
        second = bytes(reversed(self.original))
        other.write_bytes(second)
        with other.open("rb") as new_ess:
            new, new_obs = self.stage(self.action(31, fd=new_ess.fileno()), b"second")
            self.assertEqual(self.api.capture(old, new_obs), (EVENT, gen.OK))
            self.assertEqual(self.api.capture(new, old_obs), (EVENT, gen.OK))
            self.assertEqual(self.api.capture(new, new_obs), (OK, gen.OK))
            self.assertEqual(self.api.capture(old, old_obs), (OK, gen.OK))
            self.assertEqual(self.api.publish_in(new, self.running), (OK, gen.OK))
            self.assertEqual(self.api.publish_in(old, self.running), (OK, gen.OK))
            self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(31, LOAD, fd=new_ess.fileno()))),
                             (OK, gen.OK, [b"second"]))
            self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(30, LOAD, fd=new_ess.fileno()))),
                             (PAIR, gen.WRONG, []))
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(31, LOAD))), (PAIR, gen.WRONG, []))
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(30, LOAD))), (OK, gen.OK, [b"first"]))
        self.assertEqual((other.read_bytes(), self.ess.read_bytes()), (second, self.original))

    def test_effective_languages_profiles_and_launches_isolate_pending_work(self):
        pending, obs = self.stage(self.action(), b"English")
        bad_trace = obs.trace()
        bad_trace.launch[3] ^= 63
        self.assertEqual(self.api.capture(pending, obs, trace=bad_trace), (EVENT, gen.OK))
        self.assertEqual(self.api.capture(pending, obs), (OK, gen.OK))
        for run in (scope(launch=222), scope(profile=2), scope(text=1), scope(voices=1), scope(text=1, voices=1)):
            self.assertEqual(self.api.publish_in(pending, run), (SCOPE, gen.OK))
        self.assertEqual(self.api.publish_in(pending, self.running), (OK, gen.OK))
        for run in (scope(profile=2), scope(text=1), scope(voices=1), scope(text=1, voices=1)):
            separate = self.session(run)
            requested = self.observe(self.action(site=LOAD, running=run))
            self.assertEqual(self.api.restore(self.bridge, requested), (SCOPE, gen.OK, []))
            self.assertEqual(self.api.restore(separate, requested), (PAIR, gen.MISSING, []))
        position = self.session(scope(launch=222))
        self.assertEqual(self.api.restore(position, self.observe(self.action(site=LOAD))), (SCOPE, gen.OK, []))
        # A second dynamically loaded guest, with a different launch nonce, can
        # load this exact durable generation in the same profile and language.
        second_guest = self.root / "different-guest.so"
        second_guest.write_bytes(self.guest.read_bytes())
        next_run = scope(launch=222)
        next_host = self.api.fixture_host_open(os.fsencode(second_guest), 1, next_run.launch)
        self.assertTrue(next_host)
        self.addCleanup(self.api.fixture_host_close, next_host)
        new_obs = Observation()
        self.assertTrue(self.api.fixture_host_act(next_host, C.byref(self.action(site=LOAD, running=next_run)), C.byref(new_obs)))
        self.addCleanup(os.close, new_obs.base.ess_fd)
        self.assertEqual(self.api.restore(position, new_obs), (OK, gen.OK, [b"English"]))

    def test_incomplete_corrupt_or_exhausted_trace_is_refused(self):
        pending, obs = self.stage(self.action())
        for field, value in (("count", obs.count - 1), ("count", 257), ("dropped", 1)):
            trace = obs.trace()
            setattr(trace, field, value)
            self.assertEqual(self.api.capture(pending, obs, trace=trace), (TRACE, gen.OK))
        for index, field, value in ((1, "parent", 0), (1, "ticket", 0), (2, "outcome", 0),
                                     (2, "parent", 0), (3, "ticket", 2), (3, "outcome", 2)):
            row = obs.rows[index]
            old = getattr(row, field)
            setattr(row, field, value)
            self.assertEqual(self.api.capture(pending, obs), (TRACE, gen.OK))
            setattr(row, field, old)
        self.assertEqual(self.api.capture(pending, obs), (OK, gen.OK))
        for _ in range(64):
            full = self.observe(self.action(site=LOAD))
        self.assertEqual(full.count, 256)
        self.assertGreater(full.dropped, 0)
        self.assertEqual(self.api.check_call(C.byref(full.requested), LOAD, C.byref(full.trace())), TRACE)

    def test_two_genuine_guest_threads_keep_their_parent_tickets(self):
        entered, resume, wait_failed = threading.Event(), threading.Event(), threading.Event()
        first = Observation()

        @NOTICE
        def pause_in_guest(_, event, in_vm):
            if in_vm:
                entered.set()
                if not resume.wait(5):
                    wait_failed.set()

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(self.api.fixture_host_observe_with_notice, self.host, C.byref(self.action(71)),
                                 C.byref(first), pause_in_guest, None)
            try:
                self.assertTrue(entered.wait(5))
                second = self.observe(self.action(72, LOAD))
                self.assertEqual(self.api.check_call(C.byref(second.requested), LOAD, C.byref(second.trace())), OK)
            finally:
                resume.set()
            self.assertEqual(future.result(timeout=5), 1)
        self.addCleanup(os.close, first.base.ess_fd)
        self.assertFalse(wait_failed.is_set())
        self.assertNotEqual(first.requested.ticket, second.requested.ticket)
        self.assertLess(second.count, first.count)
        self.assertEqual(self.api.check_call(C.byref(first.requested), SAVE, C.byref(first.trace())), OK)
        self.assertEqual(self.api.check_call(C.byref(first.requested), SAVE, C.byref(second.trace())), TRACE)
        self.assertEqual(self.api.check_call(C.byref(second.requested), SAVE, C.byref(first.trace())), TRACE)

    def test_exit_after_base_save_before_and_after_companion_publication(self):
        self.saved(5, b"previous")
        for n, mode, code in ((6, "base-only", 80), (7, "before", 81), (8, "after", 82)):
            child = subprocess.run([sys.executable, "-B", str(Path(__file__).resolve()), "crash",
                                    self.library, self.guest, self.private, self.ess, str(n), mode], check=False)
            self.assertEqual(child.returncode, code)
            state = self.api.restore(self.bridge, self.observe(self.action(n, LOAD)))
            if mode == "after":
                self.assertEqual(state, (OK, gen.OK, [bytes(1024 * 1024)]))
            else:
                self.assertEqual(state, (PAIR, gen.MISSING, []))
            self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(5, LOAD))),
                             (OK, gen.OK, [b"previous"]))
        self.assertEqual(self.ess.read_bytes(), self.original)


if __name__ == "__main__":
    if len(sys.argv) == 8 and sys.argv[1] == "crash":
        sys.exit(crash_child(*sys.argv[2:6], int(sys.argv[6]), sys.argv[7]))
    unittest.main()
