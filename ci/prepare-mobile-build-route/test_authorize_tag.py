"""Authored Git fixtures: no service requests, game inputs or build launches."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("authorize_tag", HERE / "authorize_tag.py")
route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(route)


class ExplicitTagTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.git("init", "-q")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "user.name", "Authored Fixture")
        self.git("config", "commit.gpgsign", "false")
        self.git("config", "tag.gpgsign", "false")
        self.git("config", "core.autocrlf", "false")
        self.write(route.MANIFEST, b"authored public manifest\n")
        self.write(".circleci/config.yml", b"authored prior route\n")
        self.write("source.rs", b"authored immutable source\n")
        self.anchor = self.commit()
        self.write(".circleci/config.yml", b"authored dispatch route\n")
        self.head = self.commit()
        self.tag = "skyrim-native-build19-once-" + self.head
        self.git("tag", self.tag, self.head)
        self.env = dict(CIRCLECI="true", CIRCLE_SHA1=self.head, CIRCLE_TAG=self.tag)
        self.patch = mock.patch.multiple(route, ANCHOR=self.anchor,
            MANIFEST_SHA256=hashlib.sha256(b"authored public manifest\n").hexdigest())
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def git(self, *args):
        return route.git(self.repo, *args)

    def write(self, relative, data):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def commit(self):
        self.git("add", "-f", "--", ".")
        self.git("commit", "-q", "-m", "Authored fixture")
        return self.git("rev-parse", "HEAD")

    def retag(self):
        self.head = self.git("rev-parse", "HEAD")
        self.tag = "skyrim-native-build19-once-" + self.head
        self.git("tag", self.tag, self.head)
        self.env.update(CIRCLE_SHA1=self.head, CIRCLE_TAG=self.tag)

    def test_exact_external_tag_accepts_one_dispatch_commit(self):
        self.assertEqual(route.authorize(self.repo, self.env), self.head)

    def test_branch_normal_release_and_malformed_tag_refuse(self):
        for tag in ("", "main", "v19", self.tag + "extra", self.tag.upper()):
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, "explicit_build19"):
                route.authorize(self.repo, dict(self.env, CIRCLE_TAG=tag))

    def test_ci_and_checkout_identity_cannot_be_inferred_or_mismatched(self):
        for changes in ({"CIRCLECI": "false"}, {"CIRCLE_SHA1": self.anchor}, {"CIRCLE_SHA1": ""}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                route.authorize(self.repo, dict(self.env, **changes))
        self.git("checkout", "-q", "--detach", self.anchor)
        with self.assertRaisesRegex(ValueError, "checkout_commit"):
            route.authorize(self.repo, self.env)

    def test_tag_pointing_elsewhere_refuses(self):
        self.git("tag", "-f", self.tag, self.anchor)
        with self.assertRaisesRegex(ValueError, "tag_target"):
            route.authorize(self.repo, self.env)

    def test_unknown_local_tag_refuses(self):
        self.git("tag", "-d", self.tag)
        with self.assertRaises(subprocess.CalledProcessError):
            route.authorize(self.repo, self.env)

    def test_extra_commit_above_reviewed_anchor_refuses(self):
        self.write(".circleci/config.yml", b"another dispatch change\n")
        self.commit()
        self.retag()
        with self.assertRaisesRegex(ValueError, "single_dispatch_commit"):
            route.authorize(self.repo, self.env)

    def test_changed_game_source_in_dispatch_commit_refuses(self):
        self.write("source.rs", b"unreviewed source\n")
        self.git("add", "--", "source.rs")
        self.git("commit", "-q", "--amend", "--no-edit")
        self.retag()
        with self.assertRaisesRegex(ValueError, "only_reviewed_dispatch"):
            route.authorize(self.repo, self.env)

    def test_dirty_and_untracked_checkout_refuse(self):
        self.write("source.rs", b"uncommitted\n")
        with self.assertRaisesRegex(ValueError, "clean_source"):
            route.authorize(self.repo, self.env)
        self.git("restore", "source.rs")
        self.write("unexpected.txt", b"untracked\n")
        with self.assertRaisesRegex(ValueError, "clean_source"):
            route.authorize(self.repo, self.env)

    def test_manifest_pin_refuses_even_with_other_git_checks_passing(self):
        with mock.patch.object(route, "MANIFEST_SHA256", "0" * 64):
            with self.assertRaisesRegex(ValueError, "manifest_mismatch"):
                route.authorize(self.repo, self.env)


if __name__ == "__main__":
    unittest.main()
