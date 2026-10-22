"""Bound a public CI subprocess and its process group. POSIX only."""
import os
import signal
import subprocess
import sys


def main(arguments):
    if os.name != "posix" or len(arguments) < 2:
        print("bounded_command_usage", file=sys.stderr)
        return 2
    try:
        seconds = int(arguments[0])
        if not 1 <= seconds <= 1500:
            raise ValueError
    except ValueError:
        print("bounded_command_limit", file=sys.stderr)
        return 2
    child = subprocess.Popen(arguments[1:], start_new_session=True)
    try:
        result = child.wait(timeout=seconds)
    except (subprocess.TimeoutExpired, KeyboardInterrupt):
        # The child starts a new session, so this never addresses the CI parent.
        # CoreSimulator services are managed separately by the caller's trap.
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait()
        print("bounded_command_timeout_or_interruption", file=sys.stderr)
        return 124
    return result if result >= 0 else 128 - result


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
