"""Run one CI command with a deadline; terminate only the child session we own."""
import os
import signal
import subprocess
import sys
import time


def stop_group(child, grace_seconds=10):
    """Reap the leader, but also kill surviving descendants in its own session."""
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        child.wait()
        return
    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        child.poll()  # Reap an exited leader without abandoning its descendants.
        try:
            os.killpg(child.pid, 0)
        except ProcessLookupError:
            child.wait()
            return
        time.sleep(0.05)
    try:
        os.killpg(child.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        # An uninterruptible kernel wait can outlast SIGKILL. Do not turn the
        # wrapper itself into an unbounded wait; the CI step remains the ceiling.
        print("Owned command did not reap within 5s after SIGKILL.", file=sys.stderr, flush=True)


def main():
    if os.name != "posix" or len(sys.argv) < 3:
        print("Usage on macOS/Linux: run-bounded.py SECONDS COMMAND [ARGS...]", file=sys.stderr)
        return 2
    try:
        seconds = int(sys.argv[1])
    except ValueError:
        print("Deadline must be a positive integer number of seconds.", file=sys.stderr)
        return 2
    if seconds <= 0:
        return 2
    interrupted = []

    def on_signal(signum, _frame):
        # Do not raise inside Popen or cleanup. The wait loop notices promptly.
        if not interrupted:
            interrupted.append(signum)

    previous_handlers = {sig: signal.signal(sig, on_signal) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        try:
            child = subprocess.Popen(sys.argv[2:], start_new_session=True)
        except OSError as error:
            print(f"Cannot start bounded command: {error}", file=sys.stderr)
            return 127
        deadline = time.monotonic() + seconds
        while not interrupted and time.monotonic() < deadline:
            try:
                code = child.wait(timeout=min(0.2, max(0.001, deadline - time.monotonic())))
                if interrupted:
                    stop_group(child)
                    return 128 + interrupted[0]
                return code if code >= 0 else 128 - code
            except subprocess.TimeoutExpired:
                pass
        code = 128 + interrupted[0] if interrupted else 124
        reason = f"signal {interrupted[0]}" if interrupted else f"deadline {seconds}s"
        print(f"Terminating owned command group after {reason}.", file=sys.stderr, flush=True)
        stop_group(child)
        return code
    finally:
        for sig, handler in previous_handlers.items():
            signal.signal(sig, handler)


if __name__ == "__main__":
    raise SystemExit(main())
