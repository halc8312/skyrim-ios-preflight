"""Immutable ESS handoff. The named-clone fixture explicitly models only file lifecycle."""
import concurrent.futures
import contextlib
import ctypes as C
import errno
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

import test_skse_cosave_bridge as br
import test_skse_cosave_generation as gen


class DeferredAPI:
    def __init__(self, binding):
        lib = binding.lib
        self.capture = lib.skse_bridge_defer_save
        self.capture.argtypes = [br.PTR, br.P(br.Trace), br.P(br.Evidence), C.c_int, br.P(br.PTR), br.P(C.c_int)]
        self.capture.restype = C.c_int
        self.submit = lib.skse_bridge_publish_deferred
        self.submit.argtypes = [br.PTR, br.PTR, br.P(C.c_int)]
        self.submit.restype = C.c_int
        self.drop = lib.skse_bridge_drop_deferred
        self.drop.argtypes, self.drop.restype = [br.PTR], None
        self.hashes = lib.skse_pair_test_ess_hash_count
        self.hashes.argtypes, self.hashes.restype = [], C.c_uint

    def take(self, pending, observed, directory):
        detail, job = C.c_int(-1), br.PTR()
        result = self.capture(pending, C.byref(observed.trace()), C.byref(observed.base), directory,
                              C.byref(job), C.byref(detail))
        return result, detail.value, job

    def publish(self, bridge, job):
        detail = C.c_int(-1)
        return self.submit(bridge, job, C.byref(detail)), detail.value


class DeferredHarness(br.BridgeHarness):
    def setUp(self):
        super().setUp()
        self.deferred = DeferredAPI(self.api)
        self.scratch = self.root / "one-capture"
        self.scratch.mkdir(mode=0o700)
        self.scratch_fd = os.open(self.scratch, os.O_RDONLY | os.O_DIRECTORY)
        self.addCleanup(os.close, self.scratch_fd)

    def take(self, pending, observed):
        result, detail, job = self.deferred.take(pending, observed, self.scratch_fd)
        if job:
            self.addCleanup(self.deferred.drop, job)
        return result, detail, job

    @contextlib.contextmanager
    def sealed_input(self):
        if not hasattr(os, "memfd_create"):
            self.skipTest("requires Linux anonymous file seals")
        writer = os.memfd_create("authored-ess", os.MFD_ALLOW_SEALING | os.MFD_CLOEXEC)
        try:
            os.write(writer, self.original)
            fcntl.fcntl(writer, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK)
            with open(f"/proc/self/fd/{writer}", "rb") as reader:
                yield reader, writer
        finally:
            os.close(writer)


class HostSnapshotTests(DeferredHarness):
    def test_native_immutable_handoff_from_real_guest_to_a_worker_hashes_once(self):
        source_scope = self.running
        rc, detail, source = self.api.open_at(self.private, source_scope)
        self.assertEqual((rc, detail), (br.OK, gen.OK))
        with self.sealed_input() as (reader, writer):
            try:
                pending, obs = self.stage(self.action(fd=reader.fileno()), b"belongs to the old ESS", bridge=source)
                hashes = self.deferred.hashes()
                rc, reason, job = self.take(pending, obs)
                self.assertEqual((rc, reason), (br.OK, gen.OK))
                self.assertEqual(self.deferred.hashes(), hashes)
                self.assertEqual(self.api.capture(pending, obs), (br.STATE, gen.OK))
                self.assertEqual(self.api.publish_in(pending, source_scope), (br.STATE, gen.OK))
                with self.assertRaises(OSError) as failure:
                    os.pwrite(writer, b"overwrite", 0)
                self.assertEqual(failure.exception.errno, errno.EPERM)
            finally:
                self.api.close(source)
        sender = threading.get_ident()

        def consume():
            self.assertNotEqual(sender, threading.get_ident())
            rc, reason, worker = self.api.open_at(self.private, source_scope)
            self.assertEqual((rc, reason), (br.OK, gen.OK))
            try:
                first = self.deferred.publish(worker, job)
                second = self.deferred.publish(worker, job)
                return first, second, self.deferred.hashes()
            finally:
                self.api.close(worker)

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as worker:
            self.assertEqual(worker.submit(consume).result(5), ((br.OK, gen.OK), (br.OK, gen.ALREADY), hashes + 1))
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(site=br.LOAD))),
                         (br.OK, gen.OK, [b"belongs to the old ESS"]))

    def test_rejected_worker_root_profile_effective_languages_and_launch_do_no_hashing(self):
        with self.sealed_input() as (reader, _):
            pending, obs = self.stage(self.action(fd=reader.fileno()))
            rc, reason, job = self.take(pending, obs)
            self.assertEqual((rc, reason), (br.OK, gen.OK))
        hashes = self.deferred.hashes()
        separate = self.root / "another-private-store"
        separate.mkdir(mode=0o700)
        rc, reason, different = self.api.open_at(separate, self.running)
        self.assertEqual((rc, reason), (br.OK, gen.OK))
        self.addCleanup(self.api.close, different)
        workers = [different, *(self.session(run) for run in (br.scope(text=1), br.scope(voices=1),
                                                              br.scope(profile=2), br.scope(launch=2)))]
        for worker in workers:
            self.assertEqual(self.deferred.publish(worker, job), (br.SCOPE, gen.OK))
        self.assertEqual(self.deferred.hashes(), hashes)
        self.assertEqual(self.deferred.publish(self.bridge, job), (br.OK, gen.OK))

    def test_system_reflink_or_explicit_unavailable_keeps_original_path_safe(self):
        pending, obs = self.stage(self.action())
        before = self.deferred.hashes()
        code, detail, job = self.take(pending, obs)
        self.assertEqual(self.deferred.hashes(), before)
        if code == br.PAIR and detail == gen.UNSUPPORTED:
            self.assertFalse(job)
            self.assertEqual(self.api.capture(pending, obs), (br.OK, gen.OK))
            self.assertEqual(self.deferred.hashes(), before + 1)
            return
        self.assertEqual((code, detail), (br.OK, gen.OK))
        self.ess.write_bytes(b"other quicksave at same path")
        self.assertEqual(self.deferred.publish(self.bridge, job), (br.OK, gen.OK))
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(site=br.LOAD))),
                         (br.PAIR, gen.WRONG, []))
        self.assertEqual(list(self.scratch.iterdir()), [])


# This is an ordinary byte copy solely inside a test double. It drives the
# Apple-style name, open and unlink error paths on host filesystems without
# a clone primitive. It does not measure APFS or change the native backend.
NAMED_FIXTURE = r"""
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int skse_test_fclonefileat(int source, int dir, const char *name, unsigned flags) {
    (void)flags;
    const char *mode = getenv("SKSE_TEST_CLONE_FAULT");
    if (mode && !strcmp(mode, "full")) { errno = ENOSPC; return -1; }
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    char bytes[4096]; off_t offset = 0;
    for (;;) {
        ssize_t n = pread(source, bytes, sizeof bytes, offset);
        if (!n) break;
        if (n < 0 || write(fd, bytes, (size_t)n) != n) abort();
        offset += n;
    }
    if (close(fd)) abort();
    if (mode && !strcmp(mode, "crash")) _exit(84);
    const char *change = getenv("SKSE_TEST_CLONE_CHANGE_SOURCE");
    if (change && truncate(change, 1)) abort();
    return 0;
}
"""


class NamedSnapshotTests(DeferredHarness):
    @classmethod
    def setUpClass(cls):
        fixture = tempfile.TemporaryDirectory(prefix="classic-clone-test-double-")
        cls.addClassCleanup(fixture.cleanup)
        implementation = Path(fixture.name) / "clone_double.c"
        implementation.write_text(NAMED_FIXTURE)
        cls.extra_sources = (*br.BridgeHarness.extra_sources, "-DSKSE_TEST_NAMED_CLONE", implementation)
        super().setUpClass()

    def test_in_place_rewrite_and_path_replacement_cannot_retarget_accepted_job(self):
        pending, obs = self.stage(self.action(), b"old owner")
        hashes = self.deferred.hashes()
        status, detail, job = self.take(pending, obs)
        self.assertEqual((status, detail, self.deferred.hashes()), (br.OK, gen.OK, hashes))
        self.assertEqual(list(self.scratch.iterdir()), [])
        self.ess.write_bytes(b"rewritten in the same inode")
        self.ess.rename(self.root / "moved.ess")
        self.ess.write_bytes(b"new quicksave at the original path")
        self.assertEqual(self.deferred.publish(self.bridge, job), (br.OK, gen.OK))
        for candidate in (self.ess, self.root / "moved.ess"):
            with candidate.open("rb") as file:
                self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(site=br.LOAD, fd=file.fileno()))),
                                 (br.PAIR, gen.WRONG, []))
        copy = self.root / "exact-original.ess"
        copy.write_bytes(self.original)
        with copy.open("rb") as file:
            self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(site=br.LOAD, fd=file.fileno()))),
                             (br.OK, gen.OK, [b"old owner"]))

    def test_reserved_file_symlink_space_and_directory_access_fail_closed_before_hash(self):
        pending, obs = self.stage(self.action())
        hashes = self.deferred.hashes()
        name = self.scratch / "base.clone"
        name.symlink_to(self.ess)
        self.assertEqual(self.take(pending, obs)[:2], (br.PAIR, gen.CONFLICT))
        self.assertTrue(name.is_symlink())
        name.unlink()
        name.write_bytes(b"belongs to another capture")
        self.assertEqual(self.take(pending, obs)[:2], (br.PAIR, gen.CONFLICT))
        self.assertEqual(name.read_bytes(), b"belongs to another capture")
        name.unlink()
        self.scratch.chmod(0o750)
        self.assertEqual(self.take(pending, obs)[:2], (br.PAIR, gen.ARG))
        self.scratch.chmod(0o700)
        with patch.dict(os.environ, {"SKSE_TEST_CLONE_FAULT": "full"}):
            self.assertEqual(self.take(pending, obs)[:2], (br.PAIR, gen.STORAGE))
        self.assertEqual(self.ess.read_bytes(), self.original)
        self.assertEqual(self.deferred.hashes(), hashes)
        self.assertEqual(self.take(pending, obs)[:2], (br.OK, gen.OK))

    def test_source_changes_during_capture_and_unconfirmed_base_never_publish(self):
        pending, obs = self.stage(self.action(final=br.NONE))
        hashes = self.deferred.hashes()
        self.assertEqual(self.take(pending, obs)[:2], (br.NEEDS_BASE, gen.OK))
        obs.base.outcome = br.SAVED
        self.assertEqual(self.take(pending, obs)[:2], (br.NEEDS_BASE, gen.OK))
        obs.base.ess_fd = self.fd
        with patch.dict(os.environ, {"SKSE_TEST_CLONE_CHANGE_SOURCE": str(self.ess)}):
            self.assertEqual(self.take(pending, obs)[:2], (br.PAIR, gen.CHANGED))
        self.assertEqual(self.deferred.hashes(), hashes)
        self.assertEqual(list(self.scratch.iterdir()), [])
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(site=br.LOAD))), (br.PAIR, gen.MISSING, []))

    def test_exit_during_capture_cannot_publish_an_orphan_or_overwrite_it_on_retry(self):
        self.saved(3, b"previous owner")
        child = subprocess.run([sys.executable, "-B", Path(__file__).resolve(), "crash", self.library,
                                self.guest, self.private, self.ess, self.scratch], check=False)
        self.assertEqual(child.returncode, 84)
        orphan = self.scratch / "base.clone"
        self.assertEqual(orphan.read_bytes(), self.original)
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(8, br.LOAD))), (br.PAIR, gen.MISSING, []))
        self.assertEqual(self.api.restore(self.bridge, self.observe(self.action(3, br.LOAD))),
                         (br.OK, gen.OK, [b"previous owner"]))
        pending, obs = self.stage(self.action(8))
        self.assertEqual(self.take(pending, obs)[:2], (br.PAIR, gen.CONFLICT))
        self.assertEqual(orphan.read_bytes(), self.original)
        self.assertEqual(self.ess.read_bytes(), self.original)


def interrupt_capture(library, guest, private, ess, scratch):
    api, running = br.Binding(library), br.scope(launch=888)
    deferred = DeferredAPI(api)
    dynamic = api.fixture_host_open(os.fsencode(guest), 1, running.launch)
    rc, _, host = api.open_at(private, running)
    if rc != br.OK or not dynamic:
        return 1
    with open(ess, "rb") as source:
        action = br.Action(running, gen.key(8), br.SAVE, 0, br.SAVED, source.fileno())
        rc, _, pending, obs = api.stage(dynamic, host, action, b"unpublished")
        if rc != br.OK:
            return 2
        os.environ["SKSE_TEST_CLONE_FAULT"] = "crash"
        deferred.take(pending, obs, os.open(scratch, os.O_RDONLY | os.O_DIRECTORY))
    return 3


if __name__ == "__main__":
    if len(sys.argv) == 7 and sys.argv[1] == "crash":
        sys.exit(interrupt_capture(*sys.argv[2:]))
    unittest.main()
