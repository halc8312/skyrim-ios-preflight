import json
import hashlib
from pathlib import Path
import sys
import tempfile
import time
import unittest
import run_sdk_tests as runner


class PublicDriverTests(unittest.TestCase):
    def test_normal_command_and_exclusive_logs(self):
        with tempfile.TemporaryDirectory(prefix="public-resident-driver-") as temporary:
            root = Path(temporary)
            self.assertEqual(runner.run_bounded([sys.executable, "-c", "print('public')"],
                                               root, root / "normal", 3), 0)
            before = (root / "normal.stdout.log").read_bytes()
            with self.assertRaises(FileExistsError):
                runner.run_bounded([sys.executable, "-c", "raise SystemExit(99)"],
                                   root, root / "normal", 3)
            self.assertEqual((root / "normal.stdout.log").read_bytes(), before)

    @unittest.skipUnless(sys.platform != "win32", "process-group timeout requires POSIX")
    def test_hung_public_command_is_bounded(self):
        with tempfile.TemporaryDirectory(prefix="public-resident-timeout-") as temporary:
            root = Path(temporary)
            start = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, "deadline exceeded"):
                runner.run_bounded([sys.executable, "-c", "import time; time.sleep(30)"],
                                   root, root / "hang", 0.1)
            self.assertLess(time.monotonic() - start, 5)

    def test_no_test_or_duplicate_pass_never_counts_as_success(self):
        report = dict(kind="public_apple_resident_xctest", passed=True, checked_flags=0x3fff,
                      frequency=50_000_000, cleanup_count=1, dispatched_imports=2, exit_code=0,
                      function_count=3, inner_call_deadline_seconds=15, xctest_allowance_seconds=20,
                      fresh_dedicated_app_process=True, physical_iphone_verified=False,
                      game_executed=False, ready_to_play=False, compatibility_verified=False,
                      mapped_inode_attested=False, signature_authority_authenticated=False,
                      private_guest_framework_verified=False, loader_integration_allowed=False,
                      independent_post_sign_sha256="a" * 64)
        case = ("Test Case '-[ResidentAppleSDKProbeTests.ResidentAppleProbeTests "
                + runner.TEST + "]' passed (0.1 seconds).\n")
        valid = case + runner.PREFIX + json.dumps(report) + "\n** TEST SUCCEEDED **\n"
        self.assertEqual(runner.validate_result(valid), report)
        for bad in ["** TEST SUCCEEDED **", case + valid, valid + runner.PREFIX + json.dumps(report)]:
            with self.assertRaises(ValueError):
                runner.validate_result(bad)
        for key, value in [("cleanup_count", 0), ("game_executed", True), ("frequency", True)]:
            changed = valid.replace(json.dumps(report), json.dumps(dict(report, **{key: value})))
            with self.assertRaises(ValueError):
                runner.validate_result(changed)

    def test_packaged_hash_resource_and_no_early_link_are_required(self):
        with tempfile.TemporaryDirectory(prefix="public-resident-package-") as temporary:
            root = Path(temporary)
            framework = root / "Frameworks/ResidentAppleFixture.framework"
            framework.mkdir(parents=True)
            binary = framework / "ResidentAppleFixture"
            binary.write_bytes(b"self-authored non-executable bytes")
            trusted = hashlib.sha256(binary.read_bytes()).hexdigest().encode() + b"\n"
            (root / "ResidentAppleFixture.signed-sha256.txt").write_bytes(trusted)
            runner.verify_package(root, trusted, "/usr/lib/libSystem.B.dylib")
            with self.assertRaises(ValueError):
                runner.verify_package(root, trusted, "@rpath/ResidentAppleFixture.framework/ResidentAppleFixture")
            binary.write_bytes(b"changed self-authored bytes")
            with self.assertRaises(ValueError):
                runner.verify_package(root, trusted, "")


if __name__ == "__main__":
    unittest.main()
