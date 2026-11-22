"""An authored PE32 callback and a verified multi-generation Classic co-save."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VM = ROOT / "experiments/skse-plugin-vm"
COSAVE = ROOT / "experiments/skse-cosave"
sys.path.insert(0, str(VM))
from test_skse_plugin_vm import coff_tools, compile_interpreter
from test_probe_local_file import owner_bootstrap_words


@unittest.skipUnless(os.name == "posix", "requires the POSIX co-save generation store")
class InterpretedCoSaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        host_cc = shutil.which(os.environ.get("CC", "cc"))
        if not host_cc:
            raise unittest.SkipTest("host C compiler required")
        sources = (
            "generation.c", "records.c", "digest.c", "immutable_ess.c",
            "mod_history.c", "core_snapshot.c", "plugin_records.c", "plugin_stream.c",
            "event_bridge.c", "event_trace.c", "observer/lifecycle.c", "interpreted_records.c", "copy_pair.c",
        )
        flags = ["-I", VM, "-I", COSAVE, *(COSAVE / source for source in sources),
                 VM / "probe_local_file.c", VM / "fixtures/owned_storage.c", "-lsqlite3"]
        if sys.platform != "darwin":
            flags.extend(("-lcrypto", "-lm", "-pthread"))
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags.extend(("-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"))
            if sys.platform.startswith("linux"):
                flags.extend(("-fno-pie", "-no-pie"))

        temporary = tempfile.TemporaryDirectory(prefix="classic-interpreted-cosave-")
        cls.addClassCleanup(temporary.cleanup)
        cls.work = Path(temporary.name)
        cls.test = cls.work / "test"
        cls.host_cc, cls.flags = host_cc, flags
        compile_interpreter(host_cc, cls.test, COSAVE / "fixtures/interpreted_generation.c", flags)

    def exercise(self, directory, dll, expected, *args, runner=None):
        store, scratch = directory / "store", directory / "scratch"
        store.mkdir(mode=0o700)
        scratch.mkdir(mode=0o700)
        result = subprocess.run([runner or self.test, dll, store, scratch, *args], capture_output=True, text=True, timeout=20)
        if result.returncode == 77:
            self.skipTest("filesystem lacks the required immutable save capture")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(expected, result.stdout)

    def test_exact_input_controls_record_restore_and_callbacks(self):
        coff_cc, linker, code_path = coff_tools()
        for opt in ("-O0", "-O2"):
            with self.subTest(optimization=opt):
                directory = self.work / opt[1:]
                directory.mkdir()
                obj, dll = directory / "fixture.obj", directory / "fixture.dll"
                subprocess.run([coff_cc, "--target=i686-pc-windows-msvc", "-ffreestanding",
                                "-fno-stack-protector", "-DFIXTURE_VARIANT=22", "-DFIXTURE_RESOLVE_LOAD_IDS", opt, "-c",
                                code_path(VM / "test_fixture_dll.c"), "-o", code_path(obj)],
                               check=True, timeout=60)
                subprocess.run([linker, "/dll", "/nodefaultlib", "/machine:x86", "/noentry", "/base:0x10000000",
                                f"/out:{code_path(dll)}", code_path(obj)], check=True, timeout=60)
                self.exercise(directory, dll, "restored m,n,m from verified generations")

    def test_owned_papyrusutil_restores_a_verified_generation(self):
        archive = os.environ.get("SKYRIM_TEST_OWNED_PAPYRUS_ZIP")
        executable = os.environ.get("SKYRIM_TEST_OWNED_LE_EXE")
        if not archive or not executable:
            self.skipTest("optional owner's PapyrusUtil 3.3 archive and pinned LE executable required")
        directory = self.work / "owned"
        directory.mkdir(mode=0o700)
        dll, words = directory / "StorageUtil.dll", directory / "boot.bin"
        words.write_bytes(owner_bootstrap_words(Path(executable)))
        with zipfile.ZipFile(archive) as mod:
            matching = [entry for entry in mod.infolist() if entry.filename == "SKSE/Plugins/StorageUtil.dll"]
            self.assertEqual(len(matching), 1)
            self.assertGreater(matching[0].file_size, 0)
            self.assertLessEqual(matching[0].file_size, 64 * 1024 * 1024)
            dll.write_bytes(mod.read(matching[0]))
        self.exercise(directory, dll, "restored a signed integer from the exact persisted generation", words)
        copied = self.work / "owned-copied"
        copied.mkdir(mode=0o700)
        runner = self.test
        if sys.platform.startswith("linux"):
            from test_skse_cosave_snapshot import NAMED_FIXTURE

            clone_fixture = copied / "clone.c"
            clone_fixture.write_text(NAMED_FIXTURE, encoding="utf-8")
            runner = copied / "test"
            compile_interpreter(self.host_cc, runner, COSAVE / "fixtures/interpreted_generation.c",
                                [*self.flags, "-DSKSE_TEST_NAMED_CLONE", clone_fixture])
        self.exercise(copied, dll, "restored a signed integer from the copied profile", words, "copy", runner=runner)
