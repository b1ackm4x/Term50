#!/usr/bin/env python3
"""Keep a route helper bounded even when its Python adapter is hard-killed."""
import os
import json
import signal
import subprocess
import sys
import time

cancel_requested = False


def run(parent, helper, arguments, timeout=8):
    if cancel_requested or os.getppid() != parent:
        return 130
    child = subprocess.Popen([helper, *arguments], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + timeout
        while True:
            if cancel_requested or os.getppid() != parent:
                return 130
            if time.monotonic() >= deadline:
                return 124
            try:
                output, _ = child.communicate(timeout=0.05)
                if child.returncode == 0:
                    sys.stdout.buffer.write(output)
                    sys.stdout.buffer.flush()
                else:
                    # Only forward the owned helper's stable code, never raw
                    # diagnostics, UI content, or an arbitrary helper response.
                    try:
                        code = json.loads(output).get("error")
                        if isinstance(code, str) and code.startswith("MICROPHONE_ROUTE_") and len(code) <= 128 and all(c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789" for c in code):
                            print(json.dumps({"error": code}), flush=True)
                    except (ValueError, AttributeError):
                        pass
                return child.returncode
            except subprocess.TimeoutExpired:
                pass
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait()


def cancelled(signum, frame):
    # Do not throw between fork/exec and assigning the child handle.
    global cancel_requested
    cancel_requested = True


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, cancelled)
    signal.signal(signal.SIGINT, cancelled)
    try:
        raise SystemExit(run(int(sys.argv[1]), sys.argv[2], sys.argv[3:]))
    except KeyboardInterrupt:
        raise SystemExit(130)
