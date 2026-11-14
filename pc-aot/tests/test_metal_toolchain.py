"""Exercise the actual shell readiness loop without an Apple download."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('bash'), 'Bash required')
class MetalToolchainReadinessTests(unittest.TestCase):
    def run_case(self, mode):
        route = (ROOT / 'ios/dependencies/check-metal-alias.sh').read_text()
        functions = route[route.index('compile_fixture() {'):route.index('fixture_failed=0')]
        with tempfile.TemporaryDirectory(prefix='metal-ready-') as tmp:
            folder = Path(tmp)
            (folder / 'fixtures').mkdir()
            # The fake tool reports independently prescribed environment states.
            # No Apple tool, installation, retry delay, or shader is executed here.
            script = '''set -euo pipefail
OUT="$1"
MODE="$2"
calls=0
xcrun() {
  calls=$((calls + 1))
  printf '%s\\n' "$calls" >> "$OUT/calls"
  if [[ "$MODE" == never ]] || { [[ "$MODE" == delayed || "$MODE" == shader_after_wait ]] && (( calls <= 2 )); }; then
    echo "error: cannot execute tool 'metal' due to missing Metal Toolchain"
    return 1
  fi
  if [[ "$MODE" == shader || "$MODE" == shader_after_wait ]]; then
    echo 'error: invalid shader input'
    return 1
  fi
  [[ "$MODE" == no_output ]] || printf 'authored output' > "$OUT/fixtures/probe.air"
  return 0
}
sleep() { printf '%s\\n' "$1" >> "$OUT/waits"; }
''' + functions + '''
status=0
compile_after_component probe || status=$?
printf '%s\\n' "$status" > "$OUT/status"
'''
            result = subprocess.run(['bash', '-c', script, 'test', tmp, mode],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            count = len((folder / 'calls').read_text().splitlines())
            waits = (folder / 'waits').read_text().splitlines() if (folder / 'waits').exists() else []
            status = int((folder / 'status').read_text())
            logs = sorted(folder.glob('probe-after-component-*.log'))
            self.assertEqual(len(logs), count, 'Every attempt keeps its own log')
            self.assertTrue(all(value == '2' for value in waits))
            return status, count, len(waits)

    def test_missing_component_has_a_finite_readiness_window(self):
        self.assertEqual(self.run_case('ready'), (0, 1, 0))
        self.assertEqual(self.run_case('delayed'), (0, 3, 2))
        self.assertEqual(self.run_case('never'), (1, 11, 10))

    def test_shader_errors_and_missing_output_are_not_retried_or_passed(self):
        self.assertEqual(self.run_case('shader'), (1, 1, 0))
        self.assertEqual(self.run_case('shader_after_wait'), (1, 3, 2))
        self.assertEqual(self.run_case('no_output'), (1, 1, 0))


if __name__ == '__main__':
    unittest.main()
