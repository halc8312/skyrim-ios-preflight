"""Validate one explicit reviewed-source tag before the unchanged public route.

The tag carries the externally reviewed commit. It does not turn HEAD into its
own approval. Only a single CI-dispatch commit above the reviewed renderer is
allowed, and no source, build recipe, dependency, or source pin may change.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess


ANCHOR = "da24eaad817e08e4c9b60ed13cf8a4e4ed32eb55"
MANIFEST = "ci/owned-host/public-source-manifest.json"
MANIFEST_SHA256 = "63e0e05d0b9af6ab35339a66653b52dd479e3def3a497dcb261e7e22f110c87a"
TAG = re.compile(r"skyrim-native-build19-once-([0-9a-f]{40})\Z")
DISPATCH_ONLY = frozenset({
    ".circleci/config.yml",
    "ci/prepare-mobile-build-route/authorize_tag.py",
    "ci/prepare-mobile-build-route/test_authorize_tag.py",
    "ci/prepare-mobile-build-route/test_ui_route.py",
})


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], check=True,
                            capture_output=True, timeout=15)
    return result.stdout.decode("utf-8", errors="strict").strip()


def authorize(repo, env):
    if env.get("CIRCLECI") != "true":
        raise ValueError("circleci_context_required")
    tag = env.get("CIRCLE_TAG", "")
    match = TAG.fullmatch(tag)
    if match is None:
        raise ValueError("explicit_build19_commit_tag_required")
    commit = match.group(1)
    if env.get("CIRCLE_SHA1") != commit or git(repo, "rev-parse", "HEAD") != commit:
        raise ValueError("tag_checkout_commit_mismatch")
    if git(repo, "rev-parse", "refs/tags/" + tag + "^{commit}") != commit:
        raise ValueError("tag_target_mismatch")
    if git(repo, "rev-list", "--parents", "-n", "1", "HEAD").split() != [commit, ANCHOR]:
        raise ValueError("single_dispatch_commit_above_reviewed_source_required")
    if git(repo, "status", "--porcelain=v1", "--untracked-files=normal"):
        raise ValueError("clean_source_checkout_required")
    changed = set(git(repo, "diff", "--name-only", "--no-renames", "-z", ANCHOR, commit).split("\0")) - {""}
    if not changed or not changed <= DISPATCH_ONLY:
        raise ValueError("only_reviewed_dispatch_files_may_change")
    manifest = Path(repo) / MANIFEST
    if manifest.is_symlink() or not manifest.is_file():
        raise ValueError("regular_public_manifest_required")
    if hashlib.sha256(manifest.read_bytes()).hexdigest() != MANIFEST_SHA256:
        raise ValueError("reviewed_source_manifest_mismatch")
    return commit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    args = parser.parse_args()
    print(authorize(args.repo.resolve(strict=True), os.environ))


if __name__ == "__main__":
    main()
