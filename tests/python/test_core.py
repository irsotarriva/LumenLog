"""Core from Python: errors, statistics, the diagnostic handler and predicates."""

import sys
import unittest

from support import LumenTestCase, lumen


class CoreErrorTest(LumenTestCase):
    def test_remove_sink_with_an_unknown_id_raises_invalid_sink_id_error(self):
        with self.assertRaises(lumen.InvalidSinkIdError) as raised:
            lumen.core().remove_sink(10**12)
        self.assertIsInstance(raised.exception, lumen.LumenError)
        self.assertIsInstance(raised.exception, LookupError)
        self.assertIsInstance(raised.exception, RuntimeError)

    def test_removing_a_sink_twice_raises_the_second_time(self):
        sink_id = lumen.core().add_sink(lumen.NullSink())
        lumen.core().remove_sink(sink_id)
        with self.assertRaises(lumen.InvalidSinkIdError):
            lumen.core().remove_sink(sink_id)

    def test_sink_exception_count_with_an_unknown_id_raises(self):
        with self.assertRaises(lumen.InvalidSinkIdError):
            lumen.core().sink_exception_count(10**12)

    def test_a_sink_removing_itself_from_its_callback_gets_sink_busy_error(self):
        errors = []
        ids = []

        def remove_self(record):
            try:
                lumen.core().remove_sink(ids[0])
            except lumen.SinkBusyError as e:
                errors.append(e)

        ids.append(self.add_sink(lumen.CallbackSink(on_log=remove_self)))
        lumen.info("trigger")
        lumen.core().flush()
        self.assertEqual(len(errors), 1)
        self.assertEqual(lumen.core().sink_exception_count(ids[0]), 0)

    def test_a_sink_can_remove_another_sink_from_its_callback(self):
        other = self.add_sink(lumen.NullSink())
        removed = []

        def remove_other(record):
            if not removed:
                removed.append(lumen.core().remove_sink(other))

        self.add_sink(lumen.CallbackSink(on_log=remove_other))
        lumen.info("trigger")
        lumen.core().flush()
        self.assertEqual(len(removed), 1)
        self.assertIsInstance(removed[0], lumen.NullSink)


class StatisticsTest(LumenTestCase):
    def test_statistics_count_accepted_records_per_type(self):
        before = lumen.core().statistics()
        lumen.info("a")
        lumen.info("b")
        lumen.metric("m", 1.0)
        lumen.progress("p", 1).finish()
        after = lumen.core().statistics()
        self.assertEqual(after.logs.accepted - before.logs.accepted, 2)
        self.assertEqual(after.metrics.accepted - before.metrics.accepted, 1)
        self.assertEqual(after.progress.accepted - before.progress.accepted, 1)
        self.assertEqual(after.logs.dropped_queue_full, before.logs.dropped_queue_full)
        self.assertEqual(after.logs.dropped_after_shutdown, 0)


class DiagnosticHandlerTest(LumenTestCase):
    def _report_one_line(self):
        sink_id = self.add_sink(lumen.CallbackSink(on_log=lambda r: 1 / 0))
        lumen.info("x")
        lumen.core().flush()
        return sink_id

    def test_python_handler_receives_the_line(self):
        lines = []
        lumen.core().set_diagnostic_handler(lines.append)
        sink_id = self._report_one_line()
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith(f"lumen: sink {sink_id} threw from on_log: "
                                            "ZeroDivisionError"), lines[0])

    def test_none_silences_diagnostics(self):
        lines = []
        lumen.core().set_diagnostic_handler(lines.append)
        lumen.core().set_diagnostic_handler(None)
        sink_id = self._report_one_line()
        self.assertEqual(lines, [])
        self.assertEqual(lumen.core().sink_exception_count(sink_id), 1)

    def test_exception_in_the_handler_goes_to_the_unraisable_hook(self):
        seen = []
        previous = sys.unraisablehook
        sys.unraisablehook = lambda info: seen.append(info.exc_type)
        self.addCleanup(setattr, sys, "unraisablehook", previous)

        def broken(line):
            raise KeyError(line)

        lumen.core().set_diagnostic_handler(broken)
        self._report_one_line()
        self.assertEqual(seen, [KeyError])

    def test_handler_must_be_callable(self):
        with self.assertRaises(TypeError):
            lumen.core().set_diagnostic_handler("not callable")


class PredicateTest(unittest.TestCase):
    def test_tag_predicates_evaluate_against_a_dict(self):
        p = lumen.tag_equals("run", 42) & lumen.tag_greater("energy", 1.5)
        self.assertTrue(p.evaluate({"run": 42, "energy": 2.0}))
        self.assertFalse(p.evaluate({"run": "42", "energy": 1.0}))
        self.assertFalse(p.evaluate({"energy": 2.0}))

    def test_level_conditions_need_a_level_and_are_ignored_without_one(self):
        p = lumen.level_at_least(lumen.LogLevel.WARN)
        self.assertTrue(p.evaluate({}, lumen.LogLevel.ERROR))
        self.assertFalse(p.evaluate({}, lumen.LogLevel.INFO))
        self.assertTrue(p.evaluate({}))

    def test_operators_combine_predicates(self):
        a = lumen.tag_exists("a")
        b = lumen.tag_exists("b")
        self.assertEqual([(a | b).evaluate(t) for t in ({}, {"a": 1}, {"b": 1})],
                         [False, True, True])
        self.assertEqual([(a & ~b).evaluate(t) for t in ({"a": 1}, {"a": 1, "b": 1})],
                         [True, False])
        self.assertTrue(lumen.always().evaluate({}))
        self.assertFalse(lumen.never().evaluate({}))
        self.assertTrue(lumen.Predicate().evaluate({}))

    def test_parse_predicate_builds_the_same_predicate(self):
        p = lumen.parse_predicate("level >= WARN && run == 42")
        self.assertTrue(p.evaluate({"run": 42}, lumen.LogLevel.WARN))
        self.assertFalse(p.evaluate({"run": 41}, lumen.LogLevel.WARN))
        self.assertFalse(p.evaluate({"run": 42}, lumen.LogLevel.INFO))

    def test_parse_error_raises_invalid_predicate_error_with_the_offset(self):
        with self.assertRaises(lumen.InvalidPredicateError) as raised:
            lumen.parse_predicate("run == ")
        self.assertIsInstance(raised.exception, ValueError)
        self.assertIsInstance(raised.exception, lumen.LumenError)
        self.assertIn("offset 7", str(raised.exception))

    def test_more_tags_than_a_record_holds_raise_value_error(self):
        with self.assertRaises(ValueError):
            lumen.always().evaluate({f"k{i}": i for i in range(17)})

    def test_tags_dict_emptied_by_a_values_index_raises_runtime_error(self):
        class Clearing:
            def __index__(self):
                tags.clear()
                return 1

        tags = {"".join(["key_", "a" * 40]): "".join(["value_", "b" * 40]), "n": Clearing()}
        with self.assertRaisesRegex(RuntimeError, "changed while it was read"):
            lumen.tag_exists("n").evaluate(tags)


if __name__ == "__main__":
    unittest.main()
