"""Shared fixtures for the tests of the Lumen Python module.

Every test runs against the one process-wide core. LumenTestCase isolates
tests from each other: it delivers whatever an earlier test left queued
before registering its own sink, and removes every sink, tag and setting a
test changed when it ends.
"""

import os
import subprocess
import sys
import textwrap
import threading
import unittest

try:
    import lumen_bindings as lumen
except ImportError:
    # ctest's FAIL_REGULAR_EXPRESSION turns this line into a failure, and
    # re-raising makes unittest report the import error as well.
    print("SKIP: lumen_bindings module not available (build with LUMEN_ENABLE_PYTHON=ON)")
    raise


class Capture:
    """The records a CallbackSink received, in arrival order."""

    def __init__(self):
        self.logs = []
        self.metrics = []
        self.progress = []
        self.flushes = 0
        self._lock = threading.Lock()

    def sink(self):
        def flush():
            self.flushes += 1

        return lumen.CallbackSink(
            on_log=self._adder(self.logs),
            on_metric=self._adder(self.metrics),
            on_progress=self._adder(self.progress),
            flush=flush,
        )

    def _adder(self, target):
        def add(record):
            with self._lock:
                target.append(record)

        return add


class LumenTestCase(unittest.TestCase):
    """Registers a capturing sink for every record; undoes all global changes."""

    def setUp(self):
        lumen.core().flush()
        lumen.clear_thread_tags()
        lumen.clear_process_tags()
        self.capture = Capture()
        self.sink_id = self.add_sink(self.capture.sink())
        self.addCleanup(self._restore_globals)

    def add_sink(self, sink, predicate=None):
        """Registers `sink` for the duration of the test and returns its id."""
        core = lumen.core()
        sink_id = core.add_sink(sink) if predicate is None else core.add_sink(sink, predicate)
        self.addCleanup(self._remove_if_registered, sink_id)
        return sink_id

    def flushed_logs(self):
        lumen.core().flush()
        return self.capture.logs

    def only_log(self):
        logs = self.flushed_logs()
        self.assertEqual(len(logs), 1, [r.message for r in logs])
        return logs[0]

    @staticmethod
    def _remove_if_registered(sink_id):
        try:
            lumen.core().remove_sink(sink_id)
        except lumen.InvalidSinkIdError:
            pass  # the test removed it itself

    @staticmethod
    def _restore_globals():
        lumen.core().flush()
        lumen.clear_thread_tags()
        lumen.clear_process_tags()
        lumen.set_source_capture(True)
        lumen.core().reset_diagnostic_handler()


def child_command(script):
    """The command and environment of a new interpreter that runs `script`
    and can import the module."""
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(p for p in sys.path if p)
    return [sys.executable, "-c", textwrap.dedent(script)], env


def run_child(test, script, timeout=120):
    """Runs `script` in a new interpreter that can import the module.

    Returns the CompletedProcess. A child that does not finish within
    `timeout` seconds fails `test` (a hang must not hang the suite).
    """
    command, env = child_command(script)
    try:
        return subprocess.run(
            command,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as e:
        stderr = e.stderr.decode() if isinstance(e.stderr, bytes) else e.stderr
        test.fail(f"child process did not finish within {timeout} s; stderr:\n{stderr}")
