"""Exercise the native Classic co-save generation adapter with authored files only."""
import concurrent.futures
import ctypes as C
from pathlib import Path
import os
import shlex
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "experiments/skse-cosave"
UID, FOREIGN = 0x10203040, 0xffeeddcc
OK, ALREADY, ARG, MISSING, WRONG, CHANGED, CONFLICT, UNSUPPORTED, INVALID, DUPLICATE, LIMIT, NO_OWNER, STOPPED, STORAGE, BUSY, DAMAGED, SEALED, UNPINNED = range(18)
RECORD_HEADER = struct.Struct("<III")
HEADER = struct.Struct("<4sIIII")


def section(uid, records):
    chunks = b"".join(RECORD_HEADER.pack(t, v, len(payload)) + payload for t, v, payload in records)
    return RECORD_HEADER.pack(uid, len(records), len(chunks)) + chunks


def image(*sections, skse=0x01070030):
    return HEADER.pack(b"SKSE", 1, skse, 0x09200000, len(sections)) + b"".join(sections)


class Key(C.Structure):
    _fields_ = [(name, C.c_ubyte * 16) for name in ("profile", "slot", "event")]


class Generation(C.Structure):
    _fields_ = [("key", Key), ("ess_fd", C.c_int)]


class Record(C.Structure):
    _fields_ = [("type", C.c_uint32), ("version", C.c_uint32),
                ("length", C.c_uint32), ("data", C.c_void_p)]


def key(event, *, slot=2, profile=1):
    k = Key()
    for field, number in (("profile", profile), ("slot", slot), ("event", event)):
        getattr(k, field)[:] = number.to_bytes(16, "big")
    return k


REVERT = C.CFUNCTYPE(None, C.c_void_p)
VISIT = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(Record))
PTR = C.c_void_p


class Adapter:
    def __init__(self, path):
        lib = C.CDLL(str(path))
        definitions = {
            "open": [C.c_char_p, C.POINTER(PTR)], "close": [PTR],
            "begin": [PTR, C.POINTER(Key), C.POINTER(Generation), C.POINTER(PTR)],
            "import": [C.POINTER(Key), PTR, C.c_size_t, C.POINTER(PTR)],
            "replace": [PTR, C.c_uint32, C.POINTER(Record), C.c_size_t],
            "pin": [PTR, C.POINTER(Generation)], "commit": [PTR, PTR, C.POINTER(Generation)], "discard": [PTR],
            "export": [PTR, C.POINTER(Generation), C.POINTER(PTR), C.POINTER(C.c_size_t)],
            "restore": [PTR, C.POINTER(Generation), C.c_uint32, REVERT, VISIT, PTR]
        }
        for name, types in definitions.items():
            fn = getattr(lib, "skse_pair_" + name)
            fn.argtypes = types
            fn.restype = None if name in ("close", "discard") else C.c_int
            aliases = {"import": "from_bytes", "commit": "publish_pinned"}
            setattr(self, aliases.get(name, name), fn)
        self.free = C.CDLL(None).free
        self.free.argtypes = [PTR]
        self.free.restype = None

    def store(self, directory):
        handle = PTR()
        rc = self.open(os.fsencode(directory), C.byref(handle))
        return rc, handle

    def commit(self, store, stage, completed):
        rc = self.pin(stage, completed)
        return self.publish_pinned(store, stage, completed) if rc in (OK, ALREADY) else rc

    def stage(self, k, data):
        stage = PTR()
        rc = self.from_bytes(C.byref(k), data, len(data), C.byref(stage))
        return rc, stage

    def create(self, store, k, base=None):
        stage = PTR()
        rc = self.begin(store, C.byref(k), C.byref(base) if base else None, C.byref(stage))
        return rc, stage

    def edit(self, stage, uid, records):
        blobs = [C.create_string_buffer(data) for _, _, data in records]
        rows = (Record * len(records))(
            *(Record(t, v, len(data), C.cast(blob, PTR)) for (t, v, data), blob in zip(records, blobs)))
        return self.replace(stage, uid, rows, len(rows))

    def data(self, store, g):
        ptr, size = PTR(), C.c_size_t()
        rc = self.export(store, C.byref(g), C.byref(ptr), C.byref(size))
        if ptr:
            try:
                return rc, C.string_at(ptr, size.value)
            finally:
                self.free(ptr)
        if size.value:
            raise AssertionError("failed export returned a nonzero size")
        return rc, None


FAULTS = r"""
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static _Atomic unsigned hashes;
unsigned skse_pair_test_ess_hash_count(void) { return atomic_load(&hashes); }
void skse_pair_test_ess_progress(void) {
    atomic_fetch_add(&hashes, 1);
    const char *path = getenv("SKSE_TEST_TRUNCATE_DURING_HASH");
    if (path && truncate(path, 1)) abort();
}
void skse_pair_test_interruption(sqlite3 *db, int committed) {
    const char *phase = getenv("SKSE_TEST_CRASH");
    if (!phase) return;
    if (!committed && !strcmp(phase, "before")) {
        if (sqlite3_db_cacheflush(db) != SQLITE_OK) abort();
        _exit(81);
    }
    if (committed && !strcmp(phase, "after")) _exit(82);
}
"""


def interrupted_child(library, directory, owner):
    if os.environ.get("SKSE_TEST_LIMIT_FILE_SIZE") == "1":
        import resource
        import signal
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (128 * 1024, 128 * 1024))
    adapter = Adapter(library)
    rc, store = adapter.store(directory)
    if rc != OK:
        return 3
    rc, stage = adapter.create(store, key(owner))
    if rc != OK or adapter.edit(stage, UID, [(23, 1, bytes(5 * 1024 * 1024))]) != OK:
        return 4
    with (Path(directory).parent / "Quicksave.ess").open("rb") as game:
        return adapter.commit(store, stage, C.byref(Generation(key(owner), game.fileno())))


@unittest.skipUnless(os.name == "posix" and shutil.which(os.environ.get("CC", "cc")),
                     "requires POSIX, SQLite development files, and a C compiler")
class GenerationHarness(unittest.TestCase):
    extra_sources = ()

    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix="classic-generation-build-")
        cls.addClassCleanup(temp.cleanup)
        build = Path(temp.name)
        (build / "faults.c").write_text(FAULTS)
        cls.library = build / "fixture.so"
        cc = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-pedantic", "-Wall", "-Wextra", "-Werror",
              *shlex.split(os.environ.get("CFLAGS", "")), "-fPIC",
              "-dynamiclib" if sys.platform == "darwin" else "-shared", "-DSKSE_PAIR_TEST_INTERRUPTION",
              SOURCE / "generation.c", SOURCE / "records.c", SOURCE / "digest.c", SOURCE / "immutable_ess.c",
              SOURCE / "mod_history.c", SOURCE / "core_snapshot.c", SOURCE / "registration_history.c", SOURCE / "menu_history.c",
              SOURCE / "object_history.c", SOURCE / "spawn_wire.c", SOURCE / "delayed_history.c",
              SOURCE / "plugin_records.c", SOURCE / "plugin_stream.c",
              *cls.extra_sources, build / "faults.c",
              *shlex.split(os.environ.get("LDFLAGS", "")), "-lsqlite3"]
        if sys.platform != "darwin":
            cc.append("-lcrypto")
        subprocess.run([*cc, "-o", cls.library], check=True)
        cls.native = Adapter(cls.library)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="classic-generation-data-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.directory = self.root / "private"
        self.directory.mkdir(mode=0o700)
        self.ess = self.root / "Quicksave.ess"
        self.original = b"authored opaque ESS fixture\0" + bytes(range(256)) * 350
        self.ess.write_bytes(self.original)
        self.fd = os.open(self.ess, os.O_RDONLY)
        self.addCleanup(os.close, self.fd)
        rc, self.store = self.native.store(self.directory)
        self.assertEqual(rc, OK)
        self.addCleanup(self.native.close, self.store)
        self.db = self.directory / "classic-generations.sqlite3"

    def g(self, event, fd=None, **kwargs):
        return Generation(key(event, **kwargs), self.fd if fd is None else fd)

    def stage(self, k, data):
        rc, value = self.native.stage(k, data)
        self.assertEqual(rc, OK)
        self.addCleanup(self.native.discard, value)
        return value

    def begin(self, k, base=None):
        rc, value = self.native.create(self.store, k, base)
        self.assertEqual(rc, OK)
        self.addCleanup(self.native.discard, value)
        return value

    def publish(self, g, data):
        self.assertEqual(self.native.commit(self.store, self.stage(g.key, data), C.byref(g)), OK)


class GenerationTests(GenerationHarness):
    def test_multi_owner_commit_preserves_unknown_bytes_and_original_game_file(self):
        core = section(0, [(0x4D454E52, 4, b"opaque\x00core")])
        other = section(FOREIGN, [(0xffffffff, 99, bytes(range(256))), (6, 1, b"")])
        owned = section(UID, [(1, 2, b"old")])
        initial = image(core, owned, other, skse=0x01070020)
        a, b = self.g(1), self.g(2, slot=8)
        os.lseek(self.fd, 23, os.SEEK_SET)
        self.publish(a, initial)
        stage = self.begin(b.key, a)
        fresh = [(7, 2, b"x\0y"), (7, 0, b"")]
        self.assertEqual(self.native.edit(stage, UID, fresh), OK)
        self.assertEqual(self.native.edit(stage, 9, [(8, 0, b"added")]), OK)
        self.assertEqual(self.native.data(self.store, b), (MISSING, None))
        self.assertEqual(self.native.commit(self.store, stage, C.byref(b)), OK)
        result = image(core, section(UID, fresh), other, section(9, [(8, 0, b"added")]), skse=0x01070020)
        self.assertEqual(self.native.data(self.store, b), (OK, result))
        c = self.g(3, slot=8)
        remove = self.begin(c.key, b)
        self.assertEqual(self.native.edit(remove, UID, []), OK)
        self.assertEqual(self.native.commit(self.store, remove, C.byref(c)), OK)
        self.assertEqual(self.native.data(self.store, c), (OK, image(core, other, section(9, [(8, 0, b"added")]), skse=0x01070020)))
        self.assertEqual(self.native.data(self.store, a), (OK, initial))
        self.assertEqual(os.lseek(self.fd, 0, os.SEEK_CUR), 23)
        self.assertEqual(self.ess.read_bytes(), self.original)
        self.assertEqual(list(self.root.glob("*.skse")), [])

    def test_same_name_same_size_same_mtime_and_stable_open_descriptor(self):
        a = self.g(11)
        payload = image(section(UID, [(1, 0, b"first")]))
        self.publish(a, payload)
        old_stat = self.ess.stat()
        new = self.root / "fresh.ess"
        replacement = bytearray(self.original)
        replacement[len(replacement) // 2] ^= 0x80
        new.write_bytes(replacement)
        os.utime(new, ns=(old_stat.st_atime_ns, old_stat.st_mtime_ns))
        os.replace(new, self.ess)
        with self.ess.open("rb") as current:
            reused_name, next_event = self.g(11, current.fileno()), self.g(12, current.fileno())
            self.assertEqual(self.native.data(self.store, reused_name), (WRONG, None))
            self.assertEqual(self.native.data(self.store, next_event), (MISSING, None))
            self.assertEqual(self.native.data(self.store, a), (OK, payload))
            candidate = self.begin(next_event.key)
            self.assertEqual(self.native.edit(candidate, UID, [(3, 1, b"B")]), OK)
            self.assertEqual(self.native.commit(self.store, candidate, C.byref(next_event)), OK)
            self.assertEqual(self.native.data(self.store, next_event), (OK, image(section(UID, [(3, 1, b"B")]))))
            self.assertEqual(self.native.data(self.store, self.g(12)), (WRONG, None))

    def test_identical_ess_bytes_separate_events_slots_and_profiles(self):
        for g, value in [(self.g(8), b"A"), (self.g(9), b"B"), (self.g(8, slot=4), b"C"), (self.g(8, profile=4), b"D")]:
            self.publish(g, image(section(UID, [(1, 1, value)])))
        for g, value in [(self.g(8), b"A"), (self.g(9), b"B"), (self.g(8, slot=4), b"C"), (self.g(8, profile=4), b"D")]:
            self.assertEqual(self.native.data(self.store, g), (OK, image(section(UID, [(1, 1, value)]))))
        for bad, target, expected in [(self.g(8, profile=4), key(10), WRONG),
                                      (self.g(10), key(11), MISSING)]:
            rc, empty = self.native.create(self.store, target, bad)
            self.assertEqual(rc, expected)
            self.assertFalse(empty)
        self.assertEqual(self.native.data(self.store, self.g(22)), (MISSING, None))

    def test_pin_detects_same_inode_rewrite_during_deferred_commit(self):
        a = self.g(33)
        stage = self.stage(a.key, image(section(UID, [(9, 1, b"A")])))
        self.assertEqual(self.native.publish_pinned(self.store, stage, C.byref(a)), UNPINNED)
        self.assertEqual(self.native.pin(stage, C.byref(a)), OK)
        self.assertEqual(self.native.edit(stage, UID, []), SEALED)
        st = self.ess.stat()
        different = bytearray(self.original)
        different[-12] ^= 3
        self.ess.write_bytes(different)
        os.utime(self.ess, ns=(st.st_atime_ns, st.st_mtime_ns))
        self.assertEqual(self.native.pin(stage, C.byref(a)), WRONG)
        self.assertEqual(self.native.publish_pinned(self.store, stage, C.byref(a)), WRONG)
        self.assertEqual(self.native.data(self.store, a), (MISSING, None))
        self.ess.write_bytes(self.original)
        self.assertEqual(self.native.publish_pinned(self.store, stage, C.byref(a)), OK)

    def test_conflict_idempotence_and_sticky_edit_failure(self):
        a, b = self.g(1), self.g(2)
        stage = self.begin(a.key)
        self.assertEqual(self.native.edit(stage, UID, [(3, 2, b"ok")]), OK)
        self.assertEqual(self.native.commit(self.store, stage, C.byref(b)), WRONG)
        self.assertEqual(self.native.data(self.store, b), (MISSING, None))
        self.assertEqual(self.native.commit(self.store, stage, C.byref(a)), OK)
        self.assertEqual(self.native.commit(self.store, stage, C.byref(a)), ALREADY)
        self.assertEqual(self.native.edit(stage, UID, []), SEALED)
        different = self.stage(a.key, image())
        self.assertEqual(self.native.commit(self.store, different, C.byref(a)), CONFLICT)
        invalid = self.begin(b.key, a)
        self.assertEqual(self.native.edit(invalid, UID, [(1, 2, b"uncommitted")]), OK)
        self.assertEqual(self.native.edit(invalid, 0, []), ARG)
        self.assertEqual(self.native.edit(invalid, UID, []), ARG)
        self.assertEqual(self.native.commit(self.store, invalid, C.byref(b)), ARG)
        self.assertEqual(self.native.data(self.store, b), (MISSING, None))
        self.assertEqual(self.native.data(self.store, a), (OK, image(section(UID, [(3, 2, b"ok")]))))

    def test_revert_missing_damaged_and_partially_read_state(self):
        a = self.g(4)
        owned = [(1, 1, b"first"), (2, 1, b"second")]
        self.publish(a, image(section(UID, owned)))
        values, resets = [], []
        stop = False
        @REVERT
        def revert(_):
            resets.append(1)
            values.clear()
        @VISIT
        def visit(_, pointer):
            row = pointer.contents
            values.append(C.string_at(row.data, row.length))
            return not stop or len(values) < 2
        def restore(g, uid, expected):
            before = len(resets)
            self.assertEqual(self.native.restore(self.store, C.byref(g), uid, revert, visit, None), expected)
            self.assertEqual(len(resets) - before, 2 if expected == STOPPED else 1)
        restore(a, UID, OK)
        self.assertEqual(values, [b"first", b"second"])
        restore(self.g(6), UID, MISSING)
        self.assertEqual(values, [])
        restore(a, FOREIGN, NO_OWNER)
        stop = True
        restore(a, UID, STOPPED)
        self.assertEqual(values, [])
        with sqlite3.connect(self.db) as db:
            db.execute("UPDATE generations SET companion=?", (image(section(UID, [(1, 1, b"corrupt")])),))
        restore(a, UID, DAMAGED)
        self.assertEqual(values, [])
        self.assertEqual(self.native.data(self.store, a), (DAMAGED, None))

    def test_wire_limits_reserved_owner_and_invalid_inputs(self):
        valid = image(section(UID, [(0, 99, b"")]))
        examples = [(valid[:-1], INVALID), (valid + b"x", INVALID), (b"x" * 20, INVALID),
                    (valid[:4] + b"\x02\0\0\0" + valid[8:], UNSUPPORTED),
                    (valid[:12] + bytes(4) + valid[16:], UNSUPPORTED),
                    (image(section(UID, []), section(UID, [])), DUPLICATE),
                    (HEADER.pack(b"SKSE", 1, 1, 0x09200000, 100001) + bytes(12 * 100001), LIMIT)]
        for data, expected in examples:
            with self.subTest(expected=expected, size=len(data)):
                rc, value = self.native.stage(key(3), data)
                self.assertEqual(rc, expected)
                self.assertFalse(value)
        blank = PTR()
        self.assertEqual(self.native.from_bytes(C.byref(key(1)), b"x", 64 * 1024 * 1024 + 1, C.byref(blank)), LIMIT)
        self.assertFalse(blank)
        self.assertEqual(self.native.stage(key(0), valid)[0], ARG)
        self.assertEqual(self.native.create(self.store, key(1, slot=0))[0], ARG)
        for records, count, expected in [(None, 1, ARG), ((Record * 1)(Record(0, 1, 3, None)), 1, ARG),
                                         ((Record * 1)(), 100000, LIMIT)]:
            stage = self.begin(key(21))
            self.assertEqual(self.native.replace(stage, UID, records, count), expected)
            self.assertEqual(self.native.commit(self.store, stage, C.byref(self.g(21))), expected)

    def test_metadata_transposition_is_detected_even_for_identical_ess_payloads(self):
        a, b = self.g(3), self.g(9)
        self.publish(a, image(section(UID, [(6, 2, b"original")])))
        with sqlite3.connect(self.db) as db:
            db.execute("UPDATE generations SET identity=?", (bytes(b.key),))
        self.assertEqual(self.native.data(self.store, b), (DAMAGED, None))
        self.assertEqual(self.native.data(self.store, a), (MISSING, None))

    def test_read_only_stable_regular_ess_and_hash_budget(self):
        a = self.g(5)
        stage = self.begin(a.key)
        with self.ess.open("r+b") as writable:
            self.assertEqual(self.native.commit(self.store, stage, C.byref(self.g(5, writable.fileno()))), ARG)
        large = self.root / "large.ess"
        with large.open("wb") as f:
            f.truncate(512 * 1024 * 1024 + 1)
        with large.open("rb") as f:
            self.assertEqual(self.native.commit(self.store, stage, C.byref(self.g(5, f.fileno()))), LIMIT)
        read_end, write_end = os.pipe()
        try:
            self.assertEqual(self.native.commit(self.store, stage, C.byref(self.g(5, read_end))), ARG)
        finally:
            os.close(read_end)
            os.close(write_end)
        from unittest.mock import patch
        changing = self.root / "changing.ess"
        changing.write_bytes(bytes(131072))
        with changing.open("rb") as file, patch.dict(os.environ, SKSE_TEST_TRUNCATE_DURING_HASH=str(changing)):
            self.assertEqual(self.native.data(self.store, self.g(5, file.fileno())), (MISSING, None))
            self.assertEqual(changing.stat().st_size, 131072)
            self.assertEqual(self.native.commit(self.store, stage, C.byref(self.g(5, file.fileno()))), CHANGED)
        self.assertEqual(self.native.data(self.store, a), (MISSING, None))
        self.assertEqual(self.ess.read_bytes(), self.original)

    def test_hot_journal_recovery_and_crash_after_commit(self):
        a = self.g(1)
        self.publish(a, image(section(0, [(1, 1, b"protected")])))
        for phase, exit_code, event in (("before", 81, 15), ("after", 82, 16)):
            with self.subTest(phase=phase):
                process = subprocess.run([sys.executable, "-B", __file__, "--interrupt",
                                          str(self.library), str(self.directory), str(event)],
                                         env={**os.environ, "SKSE_TEST_CRASH": phase}, timeout=15)
                self.assertEqual(process.returncode, exit_code)
                if phase == "before":
                    journal = Path(str(self.db) + "-journal")
                    self.assertGreater(journal.stat().st_size, 512)
                rc, reopened = self.native.store(self.directory)
                self.assertEqual(rc, OK)
                try:
                    rc, result = self.native.data(reopened, self.g(event))
                    self.assertEqual(rc, MISSING if phase == "before" else OK)
                    if phase == "after":
                        self.assertEqual(result, image(section(UID, [(23, 1, bytes(5 * 1024 * 1024))])))
                        retry = self.stage(key(event), result)
                        self.assertEqual(self.native.commit(reopened, retry, C.byref(self.g(event))), ALREADY)
                    self.assertEqual(self.native.data(reopened, a), (OK, image(section(0, [(1, 1, b"protected")]))))
                finally:
                    self.native.close(reopened)
        self.assertEqual(self.ess.read_bytes(), self.original)

    def test_failed_storage_write_keeps_old_generation_and_allows_retry(self):
        old, attempted = self.g(50), self.g(51)
        payload = image(section(0, [(1, 5, b"old registry")]))
        self.publish(old, payload)
        child = subprocess.run([sys.executable, "-B", __file__, "--interrupt", str(self.library),
                                str(self.directory), "51"],
                               env={**os.environ, "SKSE_TEST_LIMIT_FILE_SIZE": "1"}, timeout=15)
        self.assertEqual(child.returncode, STORAGE)
        self.assertEqual(self.native.data(self.store, old), (OK, payload))
        self.assertEqual(self.native.data(self.store, attempted), (MISSING, None))
        updated = image(section(UID, [(23, 1, bytes(5 * 1024 * 1024))]))
        self.publish(attempted, updated)
        self.assertEqual(self.native.data(self.store, attempted), (OK, updated))

    def test_concurrent_writers_and_busy_retry(self):
        a = self.g(10)
        def publish(value):
            rc, store = self.native.store(self.directory)
            if rc != OK:
                return rc
            rc, stage = self.native.stage(a.key, image(section(UID, [(1, 0, bytes([value]))])))
            try:
                return self.native.commit(store, stage, C.byref(a)) if rc == OK else rc
            finally:
                self.native.discard(stage)
                self.native.close(store)
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
            results = list(executor.map(publish, range(6)))
        self.assertEqual(sorted(results), sorted([OK] + [CONFLICT] * 5))
        winner = results.index(OK)
        self.assertEqual(publish(winner), ALREADY)
        self.assertEqual(self.native.data(self.store, a), (OK, image(section(UID, [(1, 0, bytes([winner]))]))))
        stage = self.begin(key(11))
        db = sqlite3.connect(self.db)
        try:
            db.execute("BEGIN IMMEDIATE")
            self.assertEqual(self.native.commit(self.store, stage, C.byref(self.g(11))), BUSY)
        finally:
            db.close()
        self.assertEqual(self.native.commit(self.store, stage, C.byref(self.g(11))), OK)

    def test_refuses_other_databases_and_symlinks(self):
        ancestor = self.root / "container-alias"
        ancestor.symlink_to(self.root, target_is_directory=True)
        rc, through_alias = self.native.store(ancestor / self.directory.name)
        self.assertEqual(rc, OK)
        self.native.close(through_alias)

        other = self.root / "other"
        other.mkdir()
        dbpath = other / self.db.name
        with sqlite3.connect(dbpath) as unrelated:
            unrelated.execute("CREATE TABLE owner(data)")
            unrelated.execute("INSERT INTO owner VALUES('original')")
        old = dbpath.read_bytes()
        self.assertEqual(self.native.store(other)[0], UNSUPPORTED)
        self.assertEqual(dbpath.read_bytes(), old)
        linkdir = self.root / "linked-directory"
        linkdir.symlink_to(other, target_is_directory=True)
        self.assertEqual(self.native.store(linkdir)[0], ARG)
        more = self.root / "more"
        more.mkdir()
        (more / self.db.name).symlink_to(dbpath)
        self.assertEqual(self.native.store(more)[0], STORAGE)
        self.assertEqual(dbpath.read_bytes(), old)
        hard = self.root / "hard"
        hard.mkdir()
        unowned = self.root / "unowned.ess"
        unowned.touch()
        os.link(unowned, hard / self.db.name)
        self.assertEqual(self.native.store(hard)[0], STORAGE)
        self.assertEqual(unowned.read_bytes(), b"")


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--interrupt":
        raise SystemExit(interrupted_child(sys.argv[2], sys.argv[3], int(sys.argv[4])))
    unittest.main()
