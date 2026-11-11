"""Sinks from Python: subclasses of Sink, CallbackSink, the library's sinks,
and what happens when a sink raises."""

import gc
import os
import tempfile
import unittest

from support import Capture, LumenTestCase, lumen


class RecordingSink(lumen.Sink):
    """A sink written in Python that keeps what it receives."""

    def __init__(self):
        super().__init__()
        self.logs = []
        self.metrics = []
        self.progress = []
        self.flushes = 0

    def on_log(self, record):
        self.logs.append(record.message)

    def on_metric(self, record):
        self.metrics.append((record.name, record.value))

    def on_progress(self, record):
        self.progress.append((record.label, record.current, record.total))

    def flush(self):
        self.flushes += 1


class LogsOnlySink(lumen.Sink):
    def __init__(self):
        super().__init__()
        self.messages = []

    def on_log(self, record):
        self.messages.append(record.message)


class RaisingSink(lumen.Sink):
    def on_log(self, record):
        raise ValueError(f"cannot take {record.message}")


def _churn():
    """Allocates strings of every length up to 300 and frees them, so that
    the memory of a freed string of any size is reused."""
    for _ in range(20):
        garbage = ["".join(["#", "x" * n]) for n in range(300)]
        del garbage
    gc.collect()


class PythonSinkTest(LumenTestCase):
    def test_subclass_receives_every_record_type_and_flush(self):
        sink = RecordingSink()
        self.add_sink(sink)
        lumen.info("log")
        lumen.metric("m", 1.5)
        lumen.progress("bar", 2).finish()
        lumen.core().flush()
        self.assertEqual(sink.logs, ["log"])
        self.assertEqual(sink.metrics, [("m", 1.5)])
        self.assertEqual(sink.progress, [("bar", 2, 2)])
        self.assertEqual(sink.flushes, 1)

    def test_subclass_without_some_methods_ignores_those_records(self):
        sink = LogsOnlySink()
        self.add_sink(sink)
        lumen.metric("ignored", 1.0)
        lumen.info("kept")
        lumen.core().flush()
        self.assertEqual(sink.messages, ["kept"])

    def test_remove_sink_returns_the_same_python_object_and_stops_delivery(self):
        sink = RecordingSink()
        sink_id = lumen.core().add_sink(sink)
        lumen.info("delivered")
        lumen.core().flush()
        self.assertIs(lumen.core().remove_sink(sink_id), sink)
        lumen.info("not delivered")
        lumen.core().flush()
        self.assertEqual(sink.logs, ["delivered"])

    def test_a_removed_sink_can_be_registered_again(self):
        sink = RecordingSink()
        lumen.core().remove_sink(lumen.core().add_sink(sink))
        self.add_sink(sink)
        lumen.info("again")
        lumen.core().flush()
        self.assertEqual(sink.logs, ["again"])

    def test_registering_a_sink_that_is_already_registered_raises(self):
        sink = RecordingSink()
        self.add_sink(sink)
        with self.assertRaises(RuntimeError):
            lumen.core().add_sink(sink)

    def test_adding_none_raises_lumen_error_for_a_null_sink(self):
        with self.assertRaises(lumen.LumenError) as raised:
            lumen.core().add_sink(None)
        self.assertIs(type(raised.exception), lumen.LumenError)
        self.assertEqual(str(raised.exception), "sink is null")

    def test_callback_sink_rejects_a_callback_that_is_not_callable(self):
        with self.assertRaises(TypeError):
            lumen.CallbackSink(on_log="not callable")

    def test_predicate_selects_the_records_a_sink_receives(self):
        sink = RecordingSink()
        self.add_sink(sink, lumen.level_at_least(lumen.LogLevel.WARN) | lumen.tag_equals("k", 1))
        lumen.info("dropped")
        lumen.info("tag passes", tags={"k": 1})
        lumen.error("level passes")
        lumen.core().flush()
        self.assertEqual(sink.logs, ["tag passes", "level passes"])


class RaisingSinkTest(LumenTestCase):
    def setUp(self):
        super().setUp()
        self.lines = []
        lumen.core().set_diagnostic_handler(self.lines.append)

    def test_raising_sink_does_not_stop_the_other_sinks(self):
        raising_id = self.add_sink(RaisingSink())
        after = Capture()
        self.add_sink(after.sink())
        for i in range(3):
            lumen.info(f"r{i}")
        lumen.core().flush()
        self.assertEqual([r.message for r in self.capture.logs], ["r0", "r1", "r2"])
        self.assertEqual([r.message for r in after.logs], ["r0", "r1", "r2"])
        self.assertEqual(lumen.core().sink_exception_count(raising_id), 3)

    def test_exception_is_reported_once_through_the_diagnostic_handler(self):
        raising_id = self.add_sink(RaisingSink())
        lumen.info("first")
        lumen.info("second")
        lumen.core().flush()
        self.assertEqual(len(self.lines), 1, self.lines)
        self.assertIn(f"sink {raising_id} threw from on_log", self.lines[0])
        self.assertIn("ValueError: cannot take first", self.lines[0])

    def test_callback_sink_exceptions_are_counted_too(self):
        def fail(record):
            raise RuntimeError("callback failed")

        sink_id = self.add_sink(lumen.CallbackSink(on_log=fail, flush=fail))
        lumen.info("x")
        lumen.core().flush()
        self.assertEqual(lumen.core().sink_exception_count(sink_id), 2)
        self.assertEqual(len(self.lines), 1, self.lines)
        self.assertIn("RuntimeError: callback failed", self.lines[0])


class LibrarySinkTest(LumenTestCase):
    def setUp(self):
        super().setUp()
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def _path(self, name):
        # Built at run time, so the only reference is the one passed on.
        return "".join([self.directory.name, os.sep, name, ".log"])

    def _log_through(self, sink, message):
        sink_id = lumen.core().add_sink(sink)
        lumen.info(message)
        lumen.core().flush()
        del sink
        # Destroying the returned sink closes its file.
        lumen.core().remove_sink(sink_id)
        gc.collect()

    def test_file_sink_writes_to_a_path_assigned_from_a_temporary_string(self):
        config = lumen.FileConfig()
        config.path = self._path("file_sink")
        _churn()
        expected_path = self._path("file_sink")
        self.assertEqual(config.path, expected_path)
        self._log_through(lumen.FileSink(config), "file sink message 1234")
        with open(expected_path, encoding="utf-8") as f:
            self.assertIn("file sink message 1234", f.read())

    def test_json_sink_writes_to_a_path_given_as_a_temporary_string(self):
        sink = lumen.JsonSink(self._path("json_sink"))
        _churn()
        self._log_through(sink, "json sink message 5678")
        with open(self._path("json_sink"), encoding="utf-8") as f:
            self.assertIn("json sink message 5678", f.read())

    def test_terminal_config_keeps_strings_assigned_from_temporaries(self):
        config = lumen.TerminalConfig()
        config.time_format = "".join(["%H", ":%M"])
        config.inline_tags = ["".join(["run", str(i)]) for i in range(3)]
        _churn()
        self.assertEqual(config.time_format, "%H:%M")
        self.assertEqual(config.inline_tags, ["run0", "run1", "run2"])

    def test_null_sink_accepts_records(self):
        sink_id = self.add_sink(lumen.NullSink())
        lumen.info("discarded")
        lumen.core().flush()
        self.assertEqual(lumen.core().sink_exception_count(sink_id), 0)
        self.assertEqual([r.message for r in self.capture.logs], ["discarded"])


if __name__ == "__main__":
    unittest.main()
