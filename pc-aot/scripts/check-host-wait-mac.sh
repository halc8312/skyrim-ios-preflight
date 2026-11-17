#!/bin/bash
# Independent Apple API checks. No app build, signing, game or device inputs.
set -euo pipefail
umask 077
[[ "$(uname -s)" == Darwin ]] || { echo 'Apple host required' >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
python3 scripts/check-public-tree.py
mkdir -p local/host-wait-checks
RUN="$(mktemp -d "$ROOT/local/host-wait-checks/run-XXXXXX")"
exec > >(tee "$RUN/checks.log") 2>&1
xcodebuild -version
xcrun --sdk macosx --show-sdk-version
python3 --version
git rev-parse HEAD

# Fetch only the pinned public source. Never modify an existing checkout.
python3 - <<'PY'
from pathlib import Path
import json, subprocess
root = Path.cwd()
pin = json.loads((root / 'source-lock.json').read_text())['sources'][0]
checkout = root / 'vendor/skydream'
checkout.parent.mkdir(exist_ok=True)
if not checkout.exists():
    subprocess.run(['git', 'init', str(checkout)], check=True)
    subprocess.run(['git', '-C', str(checkout), 'remote', 'add', 'origin', pin['url']], check=True)
    subprocess.run(['git', '-C', str(checkout), 'fetch', '--depth=1', 'origin', pin['revision']], check=True)
    subprocess.run(['git', '-C', str(checkout), 'checkout', '--detach', pin['revision']], check=True)
head = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
if head != pin['revision']:
    raise SystemExit('Wrong pinned upstream revision')
print('Pinned public source:', head, flush=True)
PY

unset IPHONEOS_DEPLOYMENT_TARGET SDKROOT SKYRIM_TEST_SANITIZERS SKYRIM_TSAN_DISABLE_ASLR
export CXX="$(xcrun --sdk macosx --find clang++)"
export PYTHONPATH=tests
status=0
python3 -m unittest discover -s tests -p test_dxvk_host_wait.py -v || status=1
SKYRIM_TEST_SANITIZERS=thread python3 -m unittest \
  test_dxvk_host_wait.HostWaitNativeTests.test_actual_locks_publication_exception_fast_path_and_contended_samples \
  test_dxvk_host_wait.HostWaitNativeTests.test_cs_qos_exact_optin_errno_failure_reporting_and_concurrent_ordinals \
  -v || status=1
echo "HOST_WAIT_CHECKS exit=$status iphone_verified=0 performance_verified=0"
exit "$status"
