"""Interpreter exit with Python sinks still registered, in child processes.

Each script registers a Python sink and never removes it. The module's exit
hook must deliver what was logged within its time limits, detach the sink
while the interpreter is alive, report what it could not deliver, and let the
process end with its own exit status (python/interpreter_exit.hpp).
"""

import os
import re
import signal
import subprocess
import tempfile
import time
import unittest

from support import child_command, run_child

# Prints each message as it arrives; registered by every script below.
PRELUDE = """
import sys
import threading
import lumen_bindings as lumen

def show(record):
    print(record.message, flush=True)
"""

# The exit hook's two limits (exit_delivery_limit + exit_callback_limit), and
# a margin for starting a sanitized interpreter on a loaded machine. A child
# that took longer than this waited for something the limits should cut off.
EXIT_LIMITS_S = 2.0
STARTUP_MARGIN_S = 15.0


def timed_child(test, script):
    """run_child, and the seconds the child took."""
    start = time.monotonic()
    result = run_child(test, script)
    return result, time.monotonic() - start


class ExitTest(unittest.TestCase):
    def assert_clean_exit(self, result, status, messages):
        self.assertNotIn("Fatal Python error", result.stderr)
        self.assertEqual(result.returncode, status, result.stderr)
        self.assertEqual(result.stdout.splitlines(), messages)

    def test_normal_exit_delivers_queued_records_and_exits_zero(self):
        result = run_child(self, PRELUDE + """
lumen.core().add_sink(lumen.CallbackSink(on_log=show))
for i in range(3):
    lumen.info(f"record {i}")
""")
        self.assert_clean_exit(result, 0, ["record 0", "record 1", "record 2"])
        self.assertNotIn("lumen:", result.stderr)

    def test_unhandled_exception_delivers_records_flushes_and_exits_one(self):
        result = run_child(self, PRELUDE + """
lumen.core().add_sink(lumen.CallbackSink(on_log=show, flush=lambda: print("flushed")))
lumen.error("about to fail")
raise RuntimeError("unhandled")
""")
        self.assert_clean_exit(result, 1, ["about to fail", "flushed"])
        self.assertIn("RuntimeError: unhandled", result.stderr)

    def test_sys_exit_while_daemon_threads_flood_the_queue_exits_with_its_status_in_time(self):
        result, seconds = timed_child(self, PRELUDE + """
seen = threading.Event()
count = [0]
def tally(record):
    count[0] += 1
    if count[0] == 50:
        seen.set()
lumen.core().add_sink(lumen.CallbackSink(on_log=tally))
lumen.core().add_sink(lumen.CallbackSink(on_log=lambda r: None))

def spin(worker):
    lumen.set_thread_tag("worker", worker)
    while True:
        lumen.info("spin")

for w in range(8):
    threading.Thread(target=spin, args=(w,), daemon=True).start()

def worker_exit():
    lumen.info("from a thread")
    sys.exit(5)   # ends only this thread
threading.Thread(target=worker_exit).start()

seen.wait()
print("main exits", flush=True)
sys.exit(3)
""")
        self.assert_clean_exit(result, 3, ["main exits"])
        self.assertLess(seconds, EXIT_LIMITS_S + STARTUP_MARGIN_S, result.stderr)

    def test_python_sink_subclass_left_registered_receives_both_records_then_flush(self):
        result = run_child(self, PRELUDE + """
class Printer(lumen.Sink):
    def on_log(self, record):
        show(record)
    def on_metric(self, record):
        print(f"{record.name}={record.value}", flush=True)
    def flush(self):
        print("flushed", flush=True)

lumen.core().add_sink(Printer())
lumen.info("last words")
lumen.metric("answer", 42.0)
""")
        self.assertNotIn("Fatal Python error", result.stderr)
        self.assertEqual(result.returncode, 0, result.stderr)
        # The two records may arrive in either order (the dispatch thread may
        # deliver the first before the second is queued); the exit hook's
        # flush comes after both.
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 3, lines)
        self.assertEqual(sorted(lines[:2]), ["answer=42.0", "last words"])
        self.assertEqual(lines[2], "flushed")

    def test_python_diagnostic_handler_left_installed_does_not_disturb_exit(self):
        result = run_child(self, PRELUDE + """
lumen.core().set_diagnostic_handler(lambda line: print("diag", flush=True))
lumen.core().add_sink(lumen.CallbackSink(on_log=lambda r: 1 / 0))
lumen.info("x")
lumen.core().flush()
""")
        self.assert_clean_exit(result, 0, ["diag"])

    def test_python_sink_added_after_the_exit_hook_is_refused(self):
        # atexit runs functions in reverse order of registration, so this one,
        # registered before the module is imported, runs after its hook.
        result = run_child(self, """
import atexit
def late():
    import lumen_bindings as lumen
    try:
        lumen.core().add_sink(lumen.CallbackSink(on_log=print))
    except RuntimeError as e:
        print("refused", flush=True)
    lumen.core().add_sink(lumen.NullSink())
    print("C++ sink accepted", flush=True)
atexit.register(late)
import lumen_bindings
""")
        self.assert_clean_exit(result, 0, ["refused", "C++ sink accepted"])

    def test_records_produced_from_python_after_the_exit_hook_are_dropped_and_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "late.log")
            result = run_child(self, """
import atexit
def late():
    import lumen_bindings as lumen
    lumen.info("late")
    lumen.metric("late", 1.0)
    lumen.progress("late", 2).tick()
atexit.register(late)
import lumen_bindings as lumen
lumen.core().add_sink(lumen.FileSink(lumen.FileConfig(PATH)))
lumen.info("in time")
""".replace("PATH", repr(path)))
            with open(path, encoding="utf-8") as f:
                written = f.read()
        self.assert_clean_exit(result, 0, [])
        self.assertIn("in time", written)
        self.assertNotIn("late", written)
        self.assertIn("lumen: 3 record(s) produced from Python after the interpreter began to "
                      "exit were dropped", result.stderr)

    def test_a_single_record_produced_after_the_exit_hook_is_reported_too(self):
        result = run_child(self, """
import atexit
def late():
    import lumen_bindings as lumen
    lumen.info("late")
atexit.register(late)
import lumen_bindings as lumen
lumen.info("in time")
""")
        self.assert_clean_exit(result, 0, [])
        self.assertIn("lumen: 1 record(s) produced from Python after the interpreter began to "
                      "exit were dropped", result.stderr)

    def test_slow_python_sink_gets_what_it_takes_in_time_and_the_rest_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "all.log")
            result, seconds = timed_child(self, PRELUDE + """
import time
def slow(record):
    time.sleep(0.01)
    show(record)
lumen.core().add_sink(lumen.CallbackSink(on_log=slow))
lumen.core().add_sink(lumen.FileSink(lumen.FileConfig(PATH)))
for i in range(1000):
    lumen.info(f"record {i}")
""".replace("PATH", repr(path)))
            with open(path, encoding="utf-8") as f:
                written = f.read().splitlines()
        self.assertEqual(result.returncode, 0, result.stderr)
        received = result.stdout.splitlines()
        self.assertLess(len(received), 1000)
        self.assertEqual(received, [f"record {i}" for i in range(len(received))])
        self.assertEqual(len(written), 1000)
        self.assertIn(f"lumen: {1000 - len(received)} record(s) were not delivered to Python "
                      "sinks: they had not taken them 1000 ms after the interpreter began to "
                      "exit\n", result.stderr)
        self.assertLess(seconds, EXIT_LIMITS_S + STARTUP_MARGIN_S, result.stderr)

    def test_blocked_python_sink_ends_the_process_with_status_one_and_says_why(self):
        result, seconds = timed_child(self, PRELUDE + """
never = threading.Event()
def blocking(record):
    show(record)
    never.wait()
lumen.core().add_sink(lumen.CallbackSink(on_log=blocking))
lumen.info("blocks")
lumen.info("queued behind it")
""")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["blocks"])
        self.assertIn("lumen: a Python sink or diagnostic handler was still running when the "
                      "interpreter exited", result.stderr)
        self.assertLess(seconds, EXIT_LIMITS_S + STARTUP_MARGIN_S, result.stderr)

    def test_interrupt_during_the_exit_hook_ends_its_wait_for_python_sinks(self):
        # os.write is a C function, so no Python code runs between the line
        # and the exit hook, and the interrupt reaches the hook, not a script.
        command, env = child_command(PRELUDE + """
import atexit
import os
import time
def slow(record):
    time.sleep(0.01)
lumen.core().add_sink(lumen.CallbackSink(on_log=slow))
for i in range(1000):
    lumen.info(f"record {i}")
atexit.register(os.write, 1, b"exiting\\n")
""")
        with subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True) as child:
            try:
                self.assertEqual(child.stdout.readline(), "exiting\n")
                child.send_signal(signal.SIGINT)
                _, stderr = child.communicate(timeout=EXIT_LIMITS_S + STARTUP_MARGIN_S)
            finally:
                child.kill()
        self.assertEqual(child.returncode, 0, stderr)
        self.assertRegex(stderr, re.compile(
            r"lumen: \d+ record\(s\) were not delivered to Python sinks: .*, or the wait for "
            r"them was interrupted"))


if __name__ == "__main__":
    unittest.main()
