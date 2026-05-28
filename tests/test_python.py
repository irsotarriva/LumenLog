#!/usr/bin/env python3
"""Integration tests for the Lumen Python bindings.

Run with: python tests/test_python.py

Requires LUMEN_ENABLE_PYTHON=ON and the lumen_bindings module available.
"""

import os
import sys
import time
import tempfile
import unittest
import importlib.util

try:
    import lumen_bindings as lumen
except ImportError:
    print("SKIP: lumen_bindings module not available (build with LUMEN_ENABLE_PYTHON=ON)")
    sys.exit(0)


class TestLoggingAPI(unittest.TestCase):
    def setUp(self):
        self.captured = []

        def on_log(rec):
            self.captured.append(rec)

        self.sink = lumen.CallbackSink(on_log=on_log)
        self.sid = lumen.core().add_sink(self.sink, lumen.always())

    def tearDown(self):
        if self.sid is not None:
            lumen.core().remove_sink(self.sid)

    def test_info(self):
        lumen.info("test info message")
        lumen.flush()
        time.sleep(0.05)

        self.assertGreaterEqual(len(self.captured), 1)
        rec = self.captured[0]
        self.assertEqual(rec.message, "test info message")

    def test_warn(self):
        lumen.warn("test warn message")
        lumen.flush()
        time.sleep(0.05)

        self.assertGreaterEqual(len(self.captured), 1)
        rec = self.captured[0]
        self.assertEqual(rec.message, "test warn message")

    def test_error(self):
        lumen.error("test error message")
        lumen.flush()
        time.sleep(0.05)

        self.assertGreaterEqual(len(self.captured), 1)
        rec = self.captured[0]
        self.assertEqual(rec.message, "test error message")


class TestMetricAPI(unittest.TestCase):
    def setUp(self):
        self.captured = []

        def on_metric(rec):
            self.captured.append(rec)

        self.sink = lumen.CallbackSink(on_metric=on_metric)
        self.sid = lumen.core().add_sink(self.sink, lumen.always())

    def tearDown(self):
        if self.sid is not None:
            lumen.core().remove_sink(self.sid)

    def test_metric_emits_record(self):
        lumen.metric("loss", 0.42)
        lumen.flush()
        time.sleep(0.05)

        self.assertEqual(len(self.captured), 1)
        self.assertEqual(self.captured[0].name, "loss")
        self.assertEqual(self.captured[0].value, 0.42)


class TestProgressAPI(unittest.TestCase):
    def setUp(self):
        self.captured = []

        def on_progress(rec):
            self.captured.append(rec)

        self.sink = lumen.CallbackSink(on_progress=on_progress)
        self.sid = lumen.core().add_sink(self.sink, lumen.always())

    def tearDown(self):
        if self.sid is not None:
            lumen.core().remove_sink(self.sid)

    def test_progress_finish_emits_record(self):
        p = lumen.progress("upload", 100)
        p.update(50)
        p.finish()

        lumen.flush()
        time.sleep(0.05)

        self.assertGreaterEqual(len(self.captured), 1)


class TestSinks(unittest.TestCase):
    def setUp(self):
        self.sids = []

    def tearDown(self):
        for sid in self.sids:
            lumen.core().remove_sink(sid)

    def test_null_sink(self):
        sink = lumen.NullSink()
        sid = lumen.core().add_sink(sink, lumen.always())
        self.sids.append(sid)
        self.assertIsNotNone(sid)

    def test_terminal_sink(self):
        cfg = lumen.TerminalConfig()
        sink = lumen.TerminalSink(cfg)
        sid = lumen.core().add_sink(sink, lumen.always())
        self.sids.append(sid)
        self.assertIsNotNone(sid)

    def test_file_sink(self):
        with tempfile.NamedTemporaryFile(suffix=".log", delete=False) as f:
            path = f.name
        try:
            cfg = lumen.FileConfig()
            cfg.path = path
            sink = lumen.FileSink(cfg)
            sid = lumen.core().add_sink(sink, lumen.always())
            self.sids.append(sid)

            lumen.info("file sink test")
            lumen.flush()
            time.sleep(0.1)

            self.assertTrue(os.path.exists(path))
        finally:
            if os.path.exists(path):
                os.unlink(path)

    def test_json_sink(self):
        with tempfile.NamedTemporaryFile(suffix=".jsonl", delete=False) as f:
            path = f.name
        try:
            sink = lumen.JsonSink(path)
            sid = lumen.core().add_sink(sink, lumen.always())
            self.sids.append(sid)

            lumen.info("json sink test")
            lumen.flush()
            time.sleep(0.1)

            self.assertTrue(os.path.exists(path))
        finally:
            if os.path.exists(path):
                os.unlink(path)


class TestPredicates(unittest.TestCase):
    def test_predicate_builders(self):
        self.assertTrue(lumen.always().evaluate(lumen.TagSet(), lumen.LogLevel.INFO))
        self.assertFalse(lumen.never().evaluate(lumen.TagSet(), lumen.LogLevel.INFO))

        p = lumen.level_at_least(lumen.LogLevel.WARN)
        self.assertTrue(p.evaluate(lumen.TagSet(), lumen.LogLevel.ERROR))

        ts = lumen.TagSet()
        ts.add("app", "myapp")
        p = lumen.tag_equals("app", "myapp")
        self.assertTrue(p.evaluate(ts, lumen.LogLevel.INFO))

        p = lumen.tag_exists("app")
        self.assertTrue(p.evaluate(ts, lumen.LogLevel.INFO))

    def test_predicate_operators(self):
        a = lumen.tag_exists("app")
        b = lumen.level_at_least(lumen.LogLevel.WARN)
        combined = a & b

        ts = lumen.TagSet()
        ts.add("app", "myapp")
        self.assertTrue(combined.evaluate(ts, lumen.LogLevel.ERROR))
        self.assertFalse(combined.evaluate(lumen.TagSet(), lumen.LogLevel.INFO))

        alt = a | b
        self.assertTrue(alt.evaluate(lumen.TagSet(), lumen.LogLevel.ERROR))

        neg = ~lumen.always()
        self.assertFalse(neg.evaluate(lumen.TagSet(), lumen.LogLevel.INFO))


class TestProcessTags(unittest.TestCase):
    def setUp(self):
        self.captured = []

        def on_log(rec):
            self.captured.append(rec)

        self.sink = lumen.CallbackSink(on_log=on_log)
        self.sid = lumen.core().add_sink(self.sink, lumen.always())

    def tearDown(self):
        if self.sid is not None:
            lumen.core().remove_sink(self.sid)

    def test_process_tags(self):
        lumen.set_process_tag("env", "test")
        lumen.info("with process tag")
        lumen.flush()
        time.sleep(0.05)

        self.assertGreaterEqual(len(self.captured), 1)


class TestLoggingHandler(unittest.TestCase):
    def setUp(self):
        self.captured = []

        def on_log(rec):
            self.captured.append(rec)

        self.sink = lumen.CallbackSink(on_log=on_log)
        self.sid = lumen.core().add_sink(self.sink, lumen.always())

    def tearDown(self):
        if self.sid is not None:
            lumen.core().remove_sink(self.sid)

    def test_handler_forwards_to_lumen(self):
        import logging
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))
        from logging_handler import LumenHandler

        handler = LumenHandler()
        logger = logging.getLogger("test_logger")
        logger.addHandler(handler)
        logger.setLevel(logging.DEBUG)

        logger.info("from stdlib logging")
        lumen.flush()
        time.sleep(0.05)

        logger.removeHandler(handler)
        self.assertGreaterEqual(len(self.captured), 1)


if __name__ == "__main__":
    unittest.main()
