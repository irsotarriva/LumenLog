"""Producers: the log functions, log(), fatal(), metric() and progress()."""

import inspect
import os
import signal
import tempfile
import unittest

from support import LumenTestCase, lumen, run_child


class _Index:
    """An object usable as an int through __index__, like a NumPy integer."""

    def __init__(self, value):
        self.value = value

    def __index__(self):
        return self.value


class _Clearing:
    """A tag value whose __index__ empties the dict it is in."""

    def __init__(self, tags):
        self.tags = tags

    def __index__(self):
        self.tags.clear()
        return 1


class _Replacing:
    """A tag value whose __index__ replaces the value of another key in its dict."""

    def __init__(self, tags, key):
        self.tags = tags
        self.key = key

    def __index__(self):
        self.tags[self.key] = "replaced"
        return 1


class _Growing:
    """A tag value whose __index__ adds an entry to the dict it is in."""

    def __init__(self, tags):
        self.tags = tags

    def __index__(self):
        self.tags["added"] = 2
        return 1


class LevelFunctionTest(LumenTestCase):
    def test_each_level_function_logs_its_level_and_the_message_verbatim(self):
        calls = [
            (lumen.trace, lumen.LogLevel.TRACE),
            (lumen.debug, lumen.LogLevel.DEBUG),
            (lumen.info, lumen.LogLevel.INFO),
            (lumen.warn, lumen.LogLevel.WARN),
            (lumen.error, lumen.LogLevel.ERROR),
        ]
        for function, _ in calls:
            function(f"{{}} {function.__name__} 100%")
        logs = self.flushed_logs()
        self.assertEqual([(r.level, r.message) for r in logs],
                         [(level, f"{{}} {f.__name__} 100%") for f, level in calls])

    def test_log_takes_the_level_as_an_argument(self):
        lumen.log(lumen.LogLevel.WARN, "explicit level")
        record = self.only_log()
        self.assertEqual((record.level, record.message), (lumen.LogLevel.WARN, "explicit level"))

    def test_log_refuses_fatal_with_value_error_and_logs_nothing(self):
        with self.assertRaises(ValueError):
            lumen.log(lumen.LogLevel.FATAL, "must not terminate")
        self.assertEqual(self.flushed_logs(), [])

    def test_log_refuses_a_plain_int_as_level_with_type_error(self):
        with self.assertRaises(TypeError):
            lumen.log(4, "int level")
        self.assertEqual(self.flushed_logs(), [])

    def test_message_must_be_a_str(self):
        with self.assertRaises(TypeError):
            lumen.info(b"bytes")

    def test_timestamp_is_taken_at_submission_on_the_library_clock(self):
        before = lumen.now_ns()
        lumen.info("stamped")
        after = lumen.now_ns()
        record = self.only_log()
        self.assertGreaterEqual(record.timestamp_ns, before)
        self.assertLessEqual(record.timestamp_ns, after)


class TagArgumentTest(LumenTestCase):
    def test_tags_of_every_supported_type_arrive_formatted_like_the_cpp_api(self):
        lumen.info(
            "typed",
            tags={
                "text": "v",
                "count": -3,
                "big": 2**64 - 1,
                "ratio": 0.1,
                "large": 1e300,
                "yes": True,
                "no": False,
                "index": _Index(7),
            },
        )
        self.assertEqual(
            self.only_log().tags,
            {
                "text": "v",
                "count": "-3",
                "big": "18446744073709551615",
                "ratio": "0.1",
                "large": "1e+300",
                "yes": "true",
                "no": "false",
                "index": "7",
            },
        )

    def test_metric_and_log_accept_the_same_tags(self):
        lumen.metric("loss", 0.25, tags={"epoch": 3})
        lumen.core().flush()
        self.assertEqual(len(self.capture.metrics), 1)
        metric = self.capture.metrics[0]
        self.assertEqual((metric.name, metric.value, metric.tags), ("loss", 0.25, {"epoch": "3"}))

    def test_unsupported_tag_value_raises_type_error_and_logs_nothing(self):
        with self.assertRaises(TypeError):
            lumen.info("bad", tags={"k": [1, 2]})
        with self.assertRaises(TypeError):
            lumen.info("bad", tags={"k": None})
        self.assertEqual(self.flushed_logs(), [])

    def test_non_str_tag_key_raises_type_error(self):
        with self.assertRaises(TypeError):
            lumen.info("bad", tags={1: "v"})

    def test_int_beyond_64_bits_raises_overflow_error(self):
        with self.assertRaises(OverflowError):
            lumen.info("bad", tags={"k": 2**64})
        with self.assertRaises(OverflowError):
            lumen.info("bad", tags={"k": -(2**63) - 1})

    def test_tags_must_be_a_dict(self):
        with self.assertRaises(TypeError):
            lumen.info("bad", tags=[("k", "v")])

    def test_dict_emptied_by_a_tag_values_index_raises_runtime_error_and_logs_nothing(self):
        # The entries read before the value are freed by its __index__ unless
        # the module holds references to them. Built at run time, so that the
        # dict holds the only reference to them.
        tags = {"".join(["key_", "a" * 40]): "".join(["value_", "b" * 40])}
        tags["clearing"] = _Clearing(tags)
        with self.assertRaisesRegex(RuntimeError, "changed while it was read"):
            lumen.info("mutated", tags=tags)
        tags = {"".join(["key_", "a" * 40]): "".join(["value_", "b" * 40])}
        tags["clearing"] = _Clearing(tags)
        with self.assertRaisesRegex(RuntimeError, "changed while it was read"):
            lumen.metric("mutated", 1.0, tags=tags)
        lumen.core().flush()
        self.assertEqual((self.capture.logs, self.capture.metrics), ([], []))

    def test_tag_value_whose_index_adds_an_entry_raises_runtime_error(self):
        tags = {"first": 1}
        tags["growing"] = _Growing(tags)
        with self.assertRaisesRegex(RuntimeError, "changed while it was read"):
            lumen.info("mutated", tags=tags)
        self.assertEqual(self.flushed_logs(), [])

    def test_tag_value_whose_index_replaces_another_value_raises_runtime_error(self):
        # The dict keeps its size, so only comparing each value read with the
        # value in the dict afterwards notices the change.
        tags = {"first": "original"}
        tags["replacing"] = _Replacing(tags, "first")
        with self.assertRaisesRegex(RuntimeError, "changed while it was read"):
            lumen.info("mutated", tags=tags)
        self.assertEqual(self.flushed_logs(), [])

    def test_tags_are_keyword_only(self):
        with self.assertRaises(TypeError):
            lumen.info("bad", {"k": "v"})

    def test_tags_past_the_record_limit_are_dropped_and_counted(self):
        dropped_before = lumen.core().statistics().tags_dropped
        lumen.info("many", tags={f"k{i}": i for i in range(20)})
        record = self.only_log()
        self.assertEqual(record.tags, {f"k{i}": str(i) for i in range(16)})
        self.assertEqual(lumen.core().statistics().tags_dropped - dropped_before, 4)


class SourceTest(LumenTestCase):
    def test_source_is_the_calling_python_file_line_and_function(self):
        line = inspect.currentframe().f_lineno + 1
        lumen.info("located")
        source = self.only_log().source
        self.assertEqual(source.file, __file__)
        self.assertEqual(source.line, line)
        self.assertEqual(source.function, "SourceTest.test_source_is_the_calling_python_file_line_and_function")
        self.assertEqual(source.class_name, "SourceTest")

    def test_function_outside_a_class_has_an_empty_class_name(self):
        def helper():
            lumen.info("nested")

        helper()
        source = self.only_log().source
        self.assertTrue(source.function.endswith(".<locals>.helper"), source.function)
        self.assertEqual(source.class_name, "")

    def test_source_capture_off_leaves_the_source_empty(self):
        lumen.set_source_capture(False)
        self.assertFalse(lumen.source_capture())
        lumen.info("anonymous")
        source = self.only_log().source
        self.assertEqual((source.file, source.line, source.function, source.class_name),
                         ("", 0, "", ""))

    def test_log_records_an_explicit_source_tuple_instead_of_the_caller(self):
        lumen.log(lumen.LogLevel.INFO, "given", source=("/srv/job.py", 42, "run"))
        source = self.only_log().source
        self.assertEqual((source.file, source.line, source.function, source.class_name),
                         ("/srv/job.py", 42, "run", ""))

    def test_explicit_source_must_be_a_three_tuple(self):
        with self.assertRaises(TypeError):
            lumen.log(lumen.LogLevel.INFO, "bad", source=("/srv/job.py", 42))
        with self.assertRaises(TypeError):
            lumen.log(lumen.LogLevel.INFO, "bad", source=["/srv/job.py", 42, "run"])


class ProgressTest(LumenTestCase):
    def test_progress_sends_each_new_state_once_and_finish_sends_total(self):
        bar = lumen.progress("files", 3)
        bar.update(1)
        bar.update(1)  # same state again: not sent
        bar.tick()
        bar.finish()
        bar.tick()  # after finish: nothing
        lumen.core().flush()
        records = self.capture.progress
        self.assertEqual([(r.label, r.current, r.total) for r in records],
                         [("files", 1, 3), ("files", 2, 3), ("files", 3, 3)])
        self.assertEqual(len({r.id for r in records}), 1)
        self.assertEqual((bar.current, bar.total, bar.finished), (3, 3, True))

    def test_creating_a_bar_sends_nothing(self):
        lumen.progress("idle", 10)
        lumen.core().flush()
        self.assertEqual(self.capture.progress, [])


class FatalTest(unittest.TestCase):
    def test_fatal_delivers_to_python_sinks_then_aborts_the_process(self):
        result = run_child(self, """
            import sys
            import lumen_bindings as lumen
            def on_log(record):
                print(f"{record.level.name} {record.message} {record.tags}", flush=True)
            lumen.core().add_sink(lumen.CallbackSink(on_log=on_log))
            lumen.info("before")
            lumen.fatal("the end", tags={"code": 7})
            print("not reached", flush=True)
        """)
        self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         ["INFO before {}", "FATAL the end {'code': '7'}"])

    def test_fatal_while_the_dispatch_thread_waits_for_the_gil_delivers_everything_and_aborts(self):
        # The dispatch thread holds the Core's delivery lock while it waits
        # for the GIL to call the sink; fatal() needs that lock, so it must
        # not keep the GIL while it waits, or neither thread can go on.
        result = run_child(self, """
            import lumen_bindings as lumen
            def on_log(record):
                print(record.message, flush=True)
            lumen.core().add_sink(lumen.CallbackSink(on_log=on_log))
            for i in range(1000):
                lumen.info(f"record {i}")
            lumen.fatal("the end")
        """, timeout=60)
        self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         [f"record {i}" for i in range(1000)] + ["the end"])

    def test_fatal_from_a_python_sink_callback_aborts_and_every_other_sink_receives_it(self):
        # fatal() runs on the dispatch thread here, inside a callback, with
        # the GIL. The expected lines follow lumen/core.hpp ("Fatal
        # records"): the sink whose callback is running receives nothing
        # more, and the other sinks first receive the record that was being
        # delivered, "trigger", and then the fatal record.
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "fatal.log")
            result = run_child(self, """
                import lumen_bindings as lumen
                def calling(record):
                    print(f"calling: {record.level.name} {record.message}", flush=True)
                    if record.message == "trigger":
                        lumen.fatal("from a sink")
                def other(record):
                    print(f"other: {record.level.name} {record.message}", flush=True)
                lumen.core().add_sink(lumen.CallbackSink(on_log=calling))
                lumen.core().add_sink(lumen.CallbackSink(on_log=other))
                config = lumen.FileConfig(PATH, format=lumen.FileFormat.KeyValue)
                lumen.core().add_sink(lumen.FileSink(config))
                lumen.info("trigger")
                lumen.core().flush()
                print("not reached", flush=True)
            """.replace("PATH", repr(path)), timeout=60)
            with open(path, encoding="utf-8") as f:
                written = f.read().splitlines()
        self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         ["calling: INFO trigger", "other: INFO trigger",
                          "other: FATAL from a sink"])
        self.assertEqual(len(written), 2, written)
        self.assertIn('msg=trigger', written[0])
        self.assertIn('msg="from a sink"', written[1])
        self.assertIn("level=FATAL", written[1])


if __name__ == "__main__":
    unittest.main()
