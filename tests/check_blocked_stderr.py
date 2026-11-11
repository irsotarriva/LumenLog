"""Runs lumen_blocked_stderr with standard error on a pipe that nobody reads.

Usage: check_blocked_stderr.py <path to lumen_blocked_stderr> MODE

deliver:        the program must report that its counting sink received every
                record although the TerminalSink and its diagnostics block;
                it then ends itself with SIGKILL (blocked_stderr.cpp says why).
fatal:          LUMEN_LOG_FATAL must end the process with SIGABRT instead of
                waiting for the blocked stream forever.
fatal-handler:  the same, through the program's own terminate handler, which
                prints a line on stdout before it aborts.
Exit status 0 when the check passes.
"""

import os
import signal
import subprocess
import sys

TIMEOUT_SECONDS = 60

# Per mode: the exit status (a negative signal number) and the whole stdout.
EXPECTED = {
    "deliver": (-signal.SIGKILL, "received 2000 of 2000\n"),
    "fatal": (-signal.SIGABRT, "fatal\n"),
    "fatal-handler": (-signal.SIGABRT, "fatal\nterminate handler\n"),
}


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
    if len(sys.argv) != 3 or sys.argv[2] not in EXPECTED:
        print(__doc__, file=sys.stderr)
        return 2
    program, mode = sys.argv[1], sys.argv[2]
    status, out = run(program, mode)
    if status is None:
        print(f"{mode}: still running after {TIMEOUT_SECONDS} s; stdout: {out!r}")
        return 1
    expected_status, expected_out = EXPECTED[mode]
    if status != expected_status or out != expected_out:
        print(f"{mode}: exit status {status}, stdout {out!r}; expected {expected_status}, "
              f"{expected_out!r}")
        return 1
    print(f"{mode}: passed (exit status {status})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
