"""Runs lumen_blocked_stderr with standard error on a pipe that nobody reads.

Usage: check_blocked_stderr.py <path to lumen_blocked_stderr> deliver|fatal

deliver: the program must report that its counting sink received every
         record although the TerminalSink and its diagnostics block.
fatal:   LUMEN_LOG_FATAL must reach std::terminate instead of waiting for the
         blocked stream forever.
Either way the program ends itself with SIGKILL (blocked_stderr.cpp says why).
Exit status 0 when the check passes.
"""

import os
import subprocess
import sys

TIMEOUT_SECONDS = 60


def run(program, mode):
    read_end, write_end = os.pipe()
    try:
        child = subprocess.Popen(
            [program, mode], stdout=subprocess.PIPE, stderr=write_end, text=True
        )
        os.close(write_end)
        write_end = -1
        try:
            out, _ = child.communicate(timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            child.kill()
            out, _ = child.communicate()
            return None, out
        return child.returncode, out
    finally:
        # The read end stays open, and unread, until the child is gone.
        os.close(read_end)
        if write_end >= 0:
            os.close(write_end)


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("deliver", "fatal"):
        print(__doc__, file=sys.stderr)
        return 2
    program, mode = sys.argv[1], sys.argv[2]
    status, out = run(program, mode)
    if status is None:
        print(f"{mode}: still running after {TIMEOUT_SECONDS} s; stdout: {out!r}")
        return 1
    if mode == "deliver":
        expected = "received 2000 of 2000\n"
        if status != -9 or out != expected:
            print(f"deliver: exit status {status}, stdout {out!r}, expected {expected!r}")
            return 1
    elif status != -9 or out != "fatal\n":
        print(f"fatal: exit status {status} (expected SIGKILL), stdout {out!r}")
        return 1
    print(f"{mode}: passed (exit status {status})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
