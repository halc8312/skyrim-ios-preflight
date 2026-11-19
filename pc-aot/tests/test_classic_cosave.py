"""Exercise the Classic co-save editor through its public CLI and callback entry point."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
PACK = struct.Struct("<III").pack
BASE, OWNER, OTHER = 0, 0x10203040, 0xffeeddcc
MAX = 64 * 1024 * 1024


def section(uid, records):
    data = b"".join(PACK(t, v, len(p)) + p for t, v, p in records)
    return PACK(uid, len(records), len(data)) + data


def image(*sections, skse=0x01070030):
    return struct.pack("<4sIIII", b"SKSE", 1, skse, 0x09200000, len(sections)) + b"".join(sections)


class ClassicCosaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc or os.name != "posix":
            raise unittest.SkipTest("POSIX C compiler required")
        cls.tools = tempfile.TemporaryDirectory(prefix="classic-cosave-tools-")
        cls.addClassCleanup(cls.tools.cleanup)
        dest = Path(cls.tools.name)
        cls.tool = dest / "classic-cosave"
        flags = [cc, "-std=gnu11", "-O2", "-Werror", "-Wall", "-Wextra", "-I", str(PORT)]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([*flags, str(PORT / "classic_cosave.c"), str(ROOT / "scripts/classic_cosave_tool.c"),
                        "-o", str(cls.tool)], check=True)
        cls.fault_tool = dest / "classic-cosave-faults"
        subprocess.run([*flags, str(ROOT / "scripts/classic_cosave_tool.c"),
                        str(ROOT / "tests/native/classic_cosave_faults.c"), "-o", str(cls.fault_tool)], check=True)
        # Exercise early exit and the actual payload bytes available to future
        # consumers, in addition to the metadata printed by the normal CLI.
        src = dest / "visitor.c"
        src.write_text('''#include "classic_cosave.h"
#include <stdio.h>
#include <stdlib.h>
static int visit(void *context, const SkyrimClassicRecord *record) {
    if (record->length && fwrite(record->data, 1, record->length, stdout) != record->length) abort();
    return *(int *)context;
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    int keep = atoi(argv[3]);
    SkyrimClassicResult r = skyrim_classic_cosave_visit(argv[1], strtoul(argv[2], NULL, 0), visit, &keep);
    fprintf(stderr, "%s", skyrim_classic_cosave_result(r));
    return r == SKYRIM_CLASSIC_OK ? 0 : 1;
}
''', encoding="utf-8")
        cls.visitor = dest / "visitor"
        subprocess.run([*flags, str(PORT / "classic_cosave.c"), str(src), "-o", str(cls.visitor)], check=True)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="classic-cosave-")
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.source = self.dir / "source.skse"
        self.out = self.dir / "copy.skse"

    def call(self, *arguments, preexec_fn=None):
        return subprocess.run([str(self.tool), *map(str, arguments)], cwd=self.dir, capture_output=True,
                              timeout=10, preexec_fn=preexec_fn)

    def expect(self, message, *arguments):
        result = self.call(*arguments)
        self.assertEqual(result.returncode, int(bool(message)), (arguments, result.stderr))
        self.assertEqual(result.stderr.decode().strip(), message)
        return result.stdout

    def test_replace_and_visit_preserve_foreign_bytes_and_section_position(self):
        core = section(BASE, [(0xdeadbeef, 0xffffffff, bytes(range(64)))])
        owner = section(OWNER, [(1, 2, b"old"), (1, 2, b"more")])
        other = section(OTHER, [(3, 9, b"\xff\x00St"), (3, 1, b"")])
        orig = image(core, owner, other, skse=0x01070020)
        self.source.write_bytes(orig)
        a, b = self.dir / "a", self.dir / "b"
        a.write_bytes(b"\x00replacement\xff")
        b.write_bytes(b"")
        self.expect("", "replace", self.source, self.out, hex(OWNER), "0xffffffff", 0, a, 3, 9, b)
        new = section(OWNER, [(0xffffffff, 0, a.read_bytes()), (3, 9, b"")])
        self.assertEqual(self.out.read_bytes(), image(core, new, other, skse=0x01070020))
        self.assertEqual(self.source.read_bytes(), orig)
        self.assertEqual(self.expect("", "list", self.out, OWNER), b"0xffffffff 0 13\n0x00000003 9 0\n")
        for uid, expected in [(OWNER, a.read_bytes()), (BASE, bytes(range(64))), (OTHER, b"\xff\x00St")]:
            r = subprocess.run([self.visitor, self.out, str(uid), "1"], capture_output=True, check=True)
            self.assertEqual(r.stdout, expected)
        r = subprocess.run([self.visitor, self.out, str(OTHER), "0"], capture_output=True)
        self.assertEqual((r.returncode, r.stdout, r.stderr), (1, b"\xff\x00St", b"stopped"))

    def test_new_append_remove_and_missing_uid(self):
        p = self.dir / "p"
        p.write_bytes(b"x")
        self.expect("", "replace", "-", self.source, OWNER, 20, 1, p)
        initial = section(OWNER, [(20, 1, b"x")])
        self.assertEqual(self.source.read_bytes(), image(initial))
        self.expect("", "replace", self.source, self.out, OTHER, 1, 2, p)
        appended = section(OTHER, [(1, 2, b"x")])
        self.assertEqual(self.out.read_bytes(), image(initial, appended))
        removed = self.dir / "removed.skse"
        self.expect("", "replace", self.out, removed, OWNER)
        self.assertEqual(removed.read_bytes(), image(appended))
        self.expect("UID missing", "list", removed, OWNER)
        self.expect("", "replace", "-", self.dir / "empty.SKSE", OWNER)
        self.assertEqual((self.dir / "empty.SKSE").read_bytes(), image())
        self.expect("source missing", "replace", self.dir / "missing.skse", self.dir / "none.skse", OWNER)
        self.assertFalse((self.dir / "none.skse").exists())
        self.expect("invalid argument", "replace", "-", self.dir / "core.skse", 0, 1, 1, p)
        ess = self.dir / "original.eSs"
        ess.write_bytes(b"TESV_SAVEGAME")
        fresh_ess = self.dir / "typo.ess"
        self.expect("invalid argument", "replace", self.source, fresh_ess, OWNER)
        self.expect("invalid argument", "replace", ess, self.dir / "none.skse", OWNER)
        self.expect("invalid argument", "list", ess, OWNER)
        self.assertFalse(fresh_ess.exists())
        self.assertEqual(ess.read_bytes(), b"TESV_SAVEGAME")

    def test_invalid_inputs_never_publish_or_visit(self):
        payload = b"x"
        good = section(OWNER, [(1, 7, payload)])
        other = section(OTHER, [(2, 3, payload)])
        cases = [
            ("truncated", image(good)[:-1]),
            ("malformed", image(good) + b"x"),
            ("malformed", image(good, PACK(OTHER, 1, 12) + PACK(2, 3, 99))),
            ("duplicate UID", image(good, good)),
            ("duplicate UID", image(good, other, other)),
            ("unsupported", image(good)[:4] + struct.pack("<I", 2) + image(good)[8:]),
            ("unsupported", image(good)[:12] + bytes(4) + image(good)[16:]),
            ("limit", image(PACK(OWNER, 100000, 1200000) + bytes(1200000))),
        ]
        for message, data in cases:
            with self.subTest(message=message):
                self.source.write_bytes(data)
                self.expect(message, "replace", self.source, self.out, OWNER)
                r = subprocess.run([self.visitor, self.source, str(OWNER), "1"], capture_output=True)
                self.assertEqual((r.stdout, r.stderr), (b"", message.encode()))
                self.assertEqual(self.source.read_bytes(), data)
                self.assertFalse(self.out.exists())
        with self.source.open("wb") as f:
            f.truncate(MAX + 1)
        self.expect("limit", "replace", self.source, self.out, OWNER)
        self.expect("limit", "replace", "-", self.out, OWNER, 1, 1, self.source)
        self.assertFalse(self.out.exists())

    def test_existing_names_interrupted_write_and_racing_publish(self):
        import resource
        import signal
        data = image(section(OTHER, [(9, 3, bytes(8192))]))
        self.source.write_bytes(data)
        self.expect("destination exists", "replace", self.source, self.source, OWNER)
        os.link(self.source, self.out)
        self.expect("destination exists", "replace", self.source, self.out, OWNER)
        self.out.unlink()
        self.out.symlink_to(self.dir / "missing")
        self.expect("destination exists", "replace", self.source, self.out, OWNER)
        self.out.unlink()

        def interrupt_write():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        failed = self.call("replace", self.source, self.out, OWNER, preexec_fn=interrupt_write)
        self.assertEqual((failed.returncode, failed.stderr.strip()), (1, b"I/O error"))
        self.assertFalse(self.out.exists())
        self.assertEqual(list(self.dir.glob(".classic-cosave-*")), [])
        jobs = []
        for index in range(6):
            p = self.dir / str(index)
            p.write_bytes(bytes([index]))
            jobs.append(subprocess.Popen([self.tool, "replace", self.source, self.out, str(OWNER),
                                          "1", "1", p], stdout=subprocess.PIPE, stderr=subprocess.PIPE))
        winners = []
        for i, job in enumerate(jobs):
            _, err = job.communicate(timeout=10)
            if job.returncode == 0:
                winners.append(i)
            else:
                self.assertEqual(err.strip(), b"destination exists")
        self.assertEqual(len(winners), 1)
        result = subprocess.run([self.visitor, self.out, str(OWNER), "1"], check=True, capture_output=True)
        self.assertEqual(result.stdout, bytes(winners))
        self.assertEqual(self.source.read_bytes(), data)

    def test_valid_input_at_byte_or_header_limit_cannot_grow_past_it(self):
        # A fully valid maximum-sized unknown section. The sparse hole stands
        # in for opaque bytes; the app must include these bytes in its budget.
        p = self.dir / "small"
        p.write_bytes(b"x")
        payload_length = MAX - 20 - 12 - 12
        with self.source.open("wb") as f:
            f.write(image()[:16] + struct.pack("<I", 1))
            f.write(PACK(OTHER, 1, 12 + payload_length) + PACK(77, 8, payload_length))
            f.truncate(MAX)
        self.assertEqual(self.expect("", "list", self.source, OTHER).strip(),
                         f"0x0000004d 8 {payload_length}".encode())
        self.expect("limit", "replace", self.source, self.out, OWNER, 1, 1, p)
        self.assertFalse(self.out.exists())
        self.expect("", "replace", self.source, self.out, OTHER, 1, 1, p)
        self.assertEqual(self.out.read_bytes(), image(section(OTHER, [(1, 1, b"x")])))
        self.out.unlink()
        records = 99999
        self.source.write_bytes(image(PACK(OTHER, records, 12 * records) + PACK(1, 0, 0) * records))
        self.expect("limit", "replace", self.source, self.out, OWNER, 1, 1, p)
        self.assertFalse(self.out.exists())
        self.expect("", "replace", self.source, self.out, OTHER)
        self.assertEqual(self.out.read_bytes(), image())

    def test_sync_link_and_process_failure_leave_only_complete_files_publishable(self):
        import signal
        original = image(section(OTHER, [(6, 8, b"preserve me")]))
        self.source.write_bytes(original)
        for fault in ("CLASSIC_TEST_SYNC_ERROR", "CLASSIC_TEST_LINK_ERROR", "CLASSIC_TEST_KILL_BEFORE_PUBLISH"):
            process = subprocess.run([self.fault_tool, "replace", self.source, self.out, str(OWNER)],
                                     env={**os.environ, fault: "1"}, capture_output=True, timeout=10)
            if fault == "CLASSIC_TEST_KILL_BEFORE_PUBLISH":
                self.assertEqual(process.returncode, -signal.SIGKILL)
            else:
                self.assertEqual((process.returncode, process.stderr.strip()), (1, b"I/O error"))
                self.assertEqual(list(self.dir.glob(".classic-cosave-*")), [])
            self.assertFalse(self.out.exists())
            self.assertEqual(self.source.read_bytes(), original)
        self.expect("", "replace", self.source, self.out, OWNER)
        self.assertEqual(self.out.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
