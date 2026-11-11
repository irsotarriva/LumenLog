"""Interpreter exit with Python sinks still registered, in child processes.

Each script registers a Python sink and never removes it. The module's exit
hook must deliver what was logged, detach the sink while the interpreter is
alive, and let the process end with its own exit status.
"""

import unittest

from support import run_child

# Prints each message as it arrives; registered by every script below.
PRELUDE = """
import sys
import threading
import lumen_bindings as lumen

def show(record):
    print(record.message, flush=True)
"""


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

    def test_unhandled_exception_delivers_records_flushes_and_exits_one(self):
        result = run_child(self, PRELUDE + """
lumen.core().add_sink(lumen.CallbackSink(on_log=show, flush=lambda: print("flushed")))
lumen.error("about to fail")
raise RuntimeError("unhandled")
""")
        self.assert_clean_exit(result, 1, ["about to fail", "flushed"])
        self.assertIn("RuntimeError: unhandled", result.stderr)

    def test_sys_exit_while_daemon_threads_keep_logging_exits_with_its_status(self):
        # Each record a Python sink receives costs one GIL hand-over to the
        # dispatch thread, so the threads log at a modest rate rather than
        # flooding the queue, which the exit hook would then have to drain.
        result = run_child(self, PRELUDE + """
import time
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
        time.sleep(0.001)

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

    def test_python_sink_subclass_left_registered_exits_zero(self):
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
        # The two records may arrive in either order (the dispatch thread may
        # deliver the first before the second is queued); the exit hook's
        # flush comes after both.
        lines = result.stdout.splitlines()
        self.assert_clean_exit(result, 0, lines)
        self.assertEqual(sorted(lines[:-1]), ["answer=42.0", "last words"])
        self.assertEqual(lines[-1:], ["flushed"])

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


if __name__ == "__main__":
    unittest.main()
