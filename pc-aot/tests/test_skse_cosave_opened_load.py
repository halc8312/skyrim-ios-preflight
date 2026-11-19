"""Require the exact immutable input consumed by an authored load, including its VM return."""
import contextlib
import ctypes as C
import fcntl
import os
from pathlib import Path
import stat
import tempfile

import test_skse_cosave_bridge as br
import test_skse_cosave_generation as gen
import test_skse_cosave_snapshot as sn


class OpenedLoadHarness(sn.DeferredHarness):
    def setUp(self):
        super().setUp()
        self.prepare = self.api.lib.skse_bridge_prepare_load_input
        self.prepare.argtypes = [br.PTR, br.P(br.Event), C.c_int, C.c_int, br.P(br.PTR),
                                 br.P(C.c_int), br.P(C.c_int)]
        self.prepare.restype = C.c_int
        self.finish = self.api.lib.skse_bridge_finish_load_input
        self.finish.argtypes = [br.P(br.PTR), br.P(br.Trace), br.P(br.Evidence), br.P(br.PTR),
                                br.P(C.c_size_t), br.P(C.c_int)]
        self.finish.restype = C.c_int
        self.discard = self.api.lib.skse_bridge_drop_load_input
        self.discard.argtypes, self.discard.restype = [br.PTR], None

    def guest_load(self, source, *, n=10, flags=0, outcome=br.LOADED, before_vm=None):
        action = self.action(n, br.LOAD, flags, outcome, source)
        token, view, reason, obs = br.PTR(), C.c_int(-1), C.c_int(-1), br.Observation()
        opened, errors = [], []

        @br.NOTICE
        def observe(_, event, in_vm):
            try:
                if in_vm:
                    if before_vm:
                        before_vm(action.ess_fd)
                    return
                result = self.prepare(self.bridge, event, source, self.scratch_fd, C.byref(token),
                                      C.byref(view), C.byref(reason))
                opened.append((result, reason.value))
                if result == br.OK:
                    # This authored loader consumes the supplied descriptor in guest_vm.
                    action.ess_fd = view.value
            except Exception as error:
                errors.append(error)

        self.assertEqual(self.api.fixture_host_observe_with_notice(self.host, C.byref(action), C.byref(obs),
                                                                   observe, None), 1)
        if view.value >= 0:
            os.close(view.value)  # The loader drops its input; the coordinator duplicated the consumed FD.
        if obs.base.ess_fd >= 0:
            self.addCleanup(os.close, obs.base.ess_fd)
        self.addCleanup(lambda: self.discard(token) if token else None)
        if errors:
            raise errors[0]
        self.assertEqual(len(opened), 1)
        return opened[0], token, obs

    def complete(self, token, obs, evidence=None):
        data, length, detail = br.PTR(1), C.c_size_t(12345), C.c_int(-1)
        owner = obs.base if evidence is None else evidence
        result = self.finish(C.byref(token), C.byref(obs.trace()), C.byref(owner), C.byref(data),
                             C.byref(length), C.byref(detail))
        self.assertFalse(token)
        if result != br.OK:
            self.assertEqual((data.value, length.value), (None, 0))
            return result, detail.value, None
        try:
            return result, detail.value, C.string_at(data, length.value)
        finally:
            self.native.free(data)


class NativeOpenedLoadTests(OpenedLoadHarness):
    def test_real_immutable_file_has_independent_cursor_and_one_verified_result(self):
        self.saved()
        with contextlib.ExitStack() as opened:
            if hasattr(os, "memfd_create"):
                source, _ = opened.enter_context(self.sealed_input())
            else:
                source = opened.enter_context(self.ess.open("rb"))
            source.seek(17)
            reads = []

            def consume(fd):
                if fd == source.fileno():
                    return  # The authored game continued using its source after a clone refusal.
                self.assertEqual(os.lseek(fd, 0, os.SEEK_CUR), 0)
                self.assertTrue(fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC)
                self.assertEqual(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE, os.O_RDONLY)
                reads.append(os.read(fd, 7))

            hashes = self.deferred.hashes()
            start, token, obs = self.guest_load(source.fileno(), before_vm=consume)
            if start == (br.PAIR, gen.UNSUPPORTED) and not hasattr(os, "memfd_create"):
                self.skipTest("this filesystem cannot supply an immutable load input")
            self.assertEqual(start, (br.OK, gen.OK))
            self.assertEqual((reads, source.tell(), self.deferred.hashes()), ([self.original[:7]], 17, hashes))
            expected = gen.image(gen.section(gen.UID, [(11, 2, b"new state")]))
            self.assertEqual(self.complete(token, obs), (br.OK, gen.OK, expected))
            self.assertEqual(self.complete(token, obs), (br.STATE, gen.OK, None))
            self.assertEqual((source.tell(), self.deferred.hashes()), (17, hashes + 1))


class NamedOpenedLoadTests(OpenedLoadHarness):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory(prefix="classic-load-clone-double-")
        cls.addClassCleanup(tmp.cleanup)
        implementation = Path(tmp.name) / "clone.c"
        implementation.write_text(sn.NAMED_FIXTURE)
        cls.extra_sources = (*br.BridgeHarness.extra_sources, "-DSKSE_TEST_NAMED_CLONE", implementation)
        super().setUpClass()

    def test_guest_read_is_stable_while_original_is_overwritten_and_path_replaced(self):
        self.saved()
        hashes = self.deferred.hashes()
        reads = []

        def rewrite(fd):
            self.ess.write_bytes(b"other" * 100)
            self.ess.rename(self.root / "retired.ess")
            self.ess.write_bytes(self.original)  # Same filename and bytes as the saved generation.
            reads.append(os.pread(fd, len(self.original) + 1, 0))

        start, token, obs = self.guest_load(self.fd, before_vm=rewrite)
        self.assertEqual((start, reads), ((br.OK, gen.OK), [self.original]))
        self.assertEqual(self.complete(token, obs),
                         (br.OK, gen.OK, gen.image(gen.section(gen.UID, [(11, 2, b"new state")]))))
        self.assertEqual(self.deferred.hashes(), hashes + 1)
        self.assertEqual(list(self.scratch.iterdir()), [])

    def test_identical_reopened_path_at_the_same_fd_number_does_not_attest_the_guest_input(self):
        self.saved()
        start, token, obs = self.guest_load(self.fd)
        self.assertEqual(start, (br.OK, gen.OK))
        held = os.fstat(obs.base.ess_fd)
        os.dup2(self.fd, obs.base.ess_fd)
        self.assertNotEqual((held.st_dev, held.st_ino), (os.fstat(self.fd).st_dev, os.fstat(self.fd).st_ino))
        # The older bridge trusts the coordinator's late FD and sees identical bytes.
        self.assertEqual(self.api.restore(self.bridge, obs), (br.OK, gen.OK, [b"new state"]))
        hashes = self.deferred.hashes()
        self.assertEqual(self.complete(token, obs), (br.PAIR, gen.WRONG, None))
        self.assertEqual(self.deferred.hashes(), hashes)

    def test_changed_open_input_rejected_even_with_matching_descriptor_and_bytes(self):
        self.saved()
        hashes = self.deferred.hashes()
        start, token, obs = self.guest_load(self.fd)
        self.assertEqual(start, (br.OK, gen.OK))
        mode = stat.S_IMODE(os.fstat(obs.base.ess_fd).st_mode)
        os.fchmod(obs.base.ess_fd, mode ^ stat.S_IXUSR)
        self.assertEqual(self.complete(token, obs), (br.PAIR, gen.CHANGED, None))
        self.assertEqual(self.deferred.hashes(), hashes)

    def test_terminal_failure_cannot_be_retried_with_a_later_positive_hint(self):
        self.saved()
        hashes = self.deferred.hashes()
        for flags, outcome, expected in ((br.VM_FAIL, br.LOADED, br.REJECTED),
                                         (br.SLOT_FAIL, br.LOADED, br.REJECTED),
                                         (br.NO_REVERT, br.LOADED, br.TRACE),
                                         (0, br.FAILED, br.REJECTED), (0, br.NONE, br.NEEDS_BASE)):
            with self.subTest(flags=flags, outcome=outcome):
                start, token, obs = self.guest_load(self.fd, flags=flags, outcome=outcome)
                self.assertEqual(start, (br.OK, gen.OK))
                self.assertEqual(self.complete(token, obs), (expected, gen.OK, None))
                self.assertEqual(self.complete(token, obs), (br.STATE, gen.OK, None))
        self.assertEqual(self.deferred.hashes(), hashes)

    def test_other_transaction_missing_generation_and_wrong_digest_return_no_image(self):
        self.saved()
        _, one, first = self.guest_load(self.fd)
        _, two, second = self.guest_load(self.fd)
        hashes = self.deferred.hashes()
        self.assertEqual(self.complete(one, first, second.base), (br.EVENT, gen.OK, None))
        self.assertEqual(self.complete(two, second, first.base), (br.EVENT, gen.OK, None))
        _, missing, obs = self.guest_load(self.fd, n=55)
        self.assertEqual(self.complete(missing, obs), (br.PAIR, gen.MISSING, None))
        self.assertEqual(self.deferred.hashes(), hashes)
        self.ess.write_bytes(b"another completed base")
        _, wrong, obs = self.guest_load(self.fd)
        self.assertEqual(self.complete(wrong, obs), (br.PAIR, gen.WRONG, None))
        self.assertEqual(self.deferred.hashes(), hashes + 1)

    def test_capture_refusal_leaves_loader_and_existing_scratch_file_alone(self):
        self.saved()
        reserved = self.scratch / "base.clone"
        reserved.write_bytes(b"keep")
        hashes = self.deferred.hashes()
        read = []
        start, token, obs = self.guest_load(self.fd, before_vm=lambda fd: read.append(os.pread(fd, 3, 0)))
        self.assertEqual((start, bool(token), read), ((br.PAIR, gen.CONFLICT), False, [self.original[:3]]))
        self.assertEqual(self.complete(token, obs), (br.STATE, gen.OK, None))
        self.assertEqual((reserved.read_bytes(), self.ess.read_bytes(), self.deferred.hashes()),
                         (b"keep", self.original, hashes))
