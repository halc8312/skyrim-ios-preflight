"""Refuse mutations of the frozen public kit before any output is created."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("owned_kit_preparer", Path(__file__).with_name("prepare-owned-host-kit.py"))
preparer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preparer)
SOURCE = Path(__file__).with_name("owned-host")


class PublicKitRefusalTests(unittest.TestCase):
    def test_public_audit_dependency_does_not_allow_private_tool_siblings(self):
        self.assertTrue(preparer.allowed_source_root("tools/windows-ios-guest-probe/audit_private_guest_symbols.py"))
        for name in ("tools/windows-ios-guest-probe/create_private_binding_sidecar.py",
                     "tools/windows-ios-guest-probe/audit_private_guest_symbols.py.extra",
                     "tools/windows-ios-guest-probe/private-index.json"):
            self.assertFalse(preparer.allowed_source_root(name))

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "keep"
            output.mkdir()
            (output / "public-marker").write_bytes(b"keep")
            with self.assertRaises(ValueError):
                preparer.prepare(SOURCE, output)
            self.assertEqual((output / "public-marker").read_bytes(), b"keep")

    def check_mutation(self, mutate):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            clone = root / "public-kit"
            shutil.copytree(SOURCE, clone)
            mutate(clone)
            output = root / "stage"
            with self.assertRaises(ValueError):
                preparer.prepare(clone, output)
            self.assertFalse(output.exists())

    def test_changed_public_source_hash_refuses(self):
        self.check_mutation(lambda root: (root / "skyrim-recomp-web/cpp/ppc_context.h").write_bytes(b"self-authored changed header"))

    def test_changed_allowlist_refuses_before_path_resolution(self):
        def mutate(root):
            p = root / "source-allowlist.txt"
            p.write_bytes(p.read_bytes() + b"../unrelated-synthetic-input\n")
        self.check_mutation(mutate)

    def test_symbolic_source_refuses(self):
        def mutate(root):
            p = root / "skyrim-recomp-web/cpp/ppc_context.h"
            saved = root / "public-header-copy"
            p.rename(saved)
            p.symlink_to(saved)
        self.check_mutation(mutate)


if __name__ == "__main__":
    unittest.main()
