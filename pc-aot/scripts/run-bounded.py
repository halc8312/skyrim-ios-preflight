"""Run one CI command with a deadline; terminate only the child session we own."""
import os
import signal
import subprocess
import sys


def main():
    if os.name != "posix" or len(sys.argv) < 3:
        print("Usage on macOS/Linux: run-bounded.py SECONDS COMMAND [ARGS...]", file=sys.stderr)
        return 2
    seconds = int(sys.argv[1])
    if seconds <= 0:
        return 2
    child = subprocess.Popen(sys.argv[2:], start_new_session=True)
    try:
        return child.wait(timeout=seconds)
    except (subprocess.TimeoutExpired, KeyboardInterrupt):
        print(f"Owned command exceeded deadline or was interrupted: {seconds}s", file=sys.stderr, flush=True)
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
        return 124


if __name__ == "__main__":
    raise SystemExit(main())
