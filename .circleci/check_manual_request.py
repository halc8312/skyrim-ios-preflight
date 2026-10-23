"""Admit a manually requested, externally reviewed public CircleCI checkout.

This checks request values and public helper bytes only. It neither builds nor
imports the helpers, and the source hash is not a signature or user identity.
"""
import hashlib
import os
from pathlib import Path
import platform
import re
import sys


SHA = re.compile(r"[0-9a-f]{40}\Z")
HELPER_PINS = {
    "ci/prepare-fix-build-route/prepare_route.py":
        "a8c087b7cd8a0ea10925d226adbe813cca8a9b928ddd79c20cd2adeff8bfb7ce",
    "ci/prepare-fix-build-route/reference-pins.json":
        "da73bc3d315bb2af101e49c667d01b758e321a90aaf229bfe0ec13a457c602fb",
    "ci/run-bounded-command.py":
        "f7391d02a43dc5dc97ea7f017cd5e502c70b7a4f06b4ce05a9e4972b49a890dd",
}


def validate_request(env):
    if env.get("CIRCLECI") != "true" or env.get("PUBLIC_DEVICE_BUILD_ENABLED") != "true":
        raise ValueError("explicit_manual_parameter_required")
    expected, actual = env.get("REVIEWED_SOURCE_COMMIT", ""), env.get("CIRCLE_SHA1", "")
    if not isinstance(expected, str) or not SHA.fullmatch(expected):
        raise ValueError("externally_reviewed_lowercase_commit_required")
    if not isinstance(actual, str) or not SHA.fullmatch(actual) or actual != expected:
        raise ValueError("circle_source_commit_mismatch")
    if env.get("SKYRT_OWNED_HOST_SCOPE", "") != "":
        raise ValueError("unexpected_route_scope")
    return expected


def validate_helpers(repo):
    repo = Path(repo).resolve(strict=True)
    for name, expected in HELPER_PINS.items():
        path = repo / name
        for component in [path, *path.parents]:
            if component == repo:
                break
            if component.is_symlink():
                raise ValueError("public_helper_symlink")
        if not path.resolve(strict=True).is_relative_to(repo):
            raise ValueError("public_helper_escape")
        with path.open("rb") as stream:
            data = stream.read(1_000_001)
        if len(data) > 1_000_000 or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("public_helper_pin_mismatch:" + name)


def main():
    validate_request(os.environ)
    if sys.platform != "darwin" or platform.machine() != "arm64":
        raise ValueError("apple_silicon_runner_required")
    if sys.version_info < (3, 11):
        raise ValueError("python_311_or_newer_required")
    validate_helpers(Path.cwd())
    print("public_circleci_request_admitted=true; source_signature_authenticated=false")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(2)
