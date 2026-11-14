"""The exception each binding raises for a wrong argument (docs/python.md, Errors).

The rule: an argument of the wrong type raises TypeError, and so does an int
that does not fit its parameter (a negative count, a line number of 2**32).
A tag value is the exception: an int outside both int64 and uint64 raises
OverflowError. A value of the right type that the call does not accept
raises ValueError (or a library error that is one).
"""

import unittest

from support import LumenTestCase, lumen


def _terminal_config(**fields):
    config = lumen.TerminalConfig()
    for name, value in fields.items():
        setattr(config, name, value)
    return config


INFO = lumen.LogLevel.INFO
CORE = lumen.core()
NOT_A_TAG_VALUE = object()

# (what is wrong, call, expected exception)
CASES = [
    # Producers.
    ("message not a str", lambda: lumen.info(1), TypeError),
    ("message as bytes", lambda: lumen.warn(b"text"), TypeError),
    ("tags not a dict", lambda: lumen.error("m", tags=[("k", "v")]), TypeError),
    ("tag key not a str", lambda: lumen.debug("m", tags={1: "v"}), TypeError),
    ("tag value of another type", lambda: lumen.trace("m", tags={"k": NOT_A_TAG_VALUE}),
     TypeError),
    ("tag value beyond uint64", lambda: lumen.info("m", tags={"k": 2**64}), OverflowError),
    ("tag value below int64", lambda: lumen.info("m", tags={"k": -(2**63) - 1}),
     OverflowError),
    ("tags passed by position", lambda: lumen.info("m", {"k": "v"}), TypeError),
    ("level as a str", lambda: lumen.log("INFO", "m"), TypeError),
    ("level as a plain int", lambda: lumen.log(2, "m"), TypeError),
    ("level FATAL for log()", lambda: lumen.log(lumen.LogLevel.FATAL, "m"), ValueError),
    ("source not a tuple", lambda: lumen.log(INFO, "m", source=["f", 1, "g"]), TypeError),
    ("source of two items", lambda: lumen.log(INFO, "m", source=("f", 1)), TypeError),
    ("source file not a str", lambda: lumen.log(INFO, "m", source=(1, 1, "g")), TypeError),
    ("source line a str", lambda: lumen.log(INFO, "m", source=("f", "1", "g")), TypeError),
    ("source line a float", lambda: lumen.log(INFO, "m", source=("f", 1.5, "g")), TypeError),
    ("source line negative", lambda: lumen.log(INFO, "m", source=("f", -1, "g")), TypeError),
    ("source line 2**32", lambda: lumen.log(INFO, "m", source=("f", 2**32, "g")), TypeError),
    ("source function not a str", lambda: lumen.log(INFO, "m", source=("f", 1, None)),
     TypeError),
    ("fatal message not a str", lambda: lumen.fatal(1), TypeError),
    ("fatal tag value of another type", lambda: lumen.fatal("m", tags={"k": NOT_A_TAG_VALUE}),
     TypeError),
    ("metric name not a str", lambda: lumen.metric(1, 1.0), TypeError),
    ("metric value a str", lambda: lumen.metric("m", "1"), TypeError),
    ("metric value too large for a float", lambda: lumen.metric("m", 2**2000), TypeError),
    ("progress label not a str", lambda: lumen.progress(1, 2), TypeError),
    ("progress total negative", lambda: lumen.progress("p", -1), TypeError),
    ("progress total 2**64", lambda: lumen.progress("p", 2**64), TypeError),
    ("update with a negative count", lambda: lumen.progress("p", 3).update(-1), TypeError),
    ("update with a str", lambda: lumen.progress("p", 3).update("1"), TypeError),
    ("set_source_capture with a str", lambda: lumen.set_source_capture("yes"), TypeError),
    # Context.
    ("thread tag key not a str", lambda: lumen.set_thread_tag(1, "v"), TypeError),
    ("thread tag value of another type", lambda: lumen.set_thread_tag("k", NOT_A_TAG_VALUE),
     TypeError),
    ("thread tag value beyond uint64", lambda: lumen.set_thread_tag("k", 2**64), OverflowError),
    ("process tag value a list", lambda: lumen.set_process_tag("k", [1]), TypeError),
    ("remove_thread_tag key not a str", lambda: lumen.remove_thread_tag(1), TypeError),
    ("remove_process_tag key not a str", lambda: lumen.remove_process_tag(1), TypeError),
    ("scope key not a str", lambda: lumen.scope(1, "v"), TypeError),
    ("scope value of another type", lambda: lumen.scope("k", NOT_A_TAG_VALUE), TypeError),
    # Predicates.
    ("level_at_least with a plain int", lambda: lumen.level_at_least(3), TypeError),
    ("level_equals with a str", lambda: lumen.level_equals("INFO"), TypeError),
    ("tag_equals key not a str", lambda: lumen.tag_equals(1, "v"), TypeError),
    ("tag_equals value of another type", lambda: lumen.tag_equals("k", NOT_A_TAG_VALUE),
     TypeError),
    ("tag_exists key not a str", lambda: lumen.tag_exists(1), TypeError),
    ("tag_less value a str", lambda: lumen.tag_less("k", "1"), TypeError),
    ("tag_greater key not a str", lambda: lumen.tag_greater(1, 1.0), TypeError),
    ("parse_predicate query not a str", lambda: lumen.parse_predicate(1), TypeError),
    ("parse_predicate invalid query", lambda: lumen.parse_predicate("level >="), ValueError),
    ("evaluate tags not a dict", lambda: lumen.always().evaluate([("k", "v")]), TypeError),
    ("evaluate level a str", lambda: lumen.always().evaluate({}, "INFO"), TypeError),
    ("evaluate with more tags than a record holds",
     lambda: lumen.always().evaluate({f"k{i}": i for i in range(17)}), ValueError),
    ("predicate combined with an int", lambda: lumen.always() & 3, TypeError),
    # Sinks and their configurations.
    ("CallbackSink callback not callable", lambda: lumen.CallbackSink(on_log=3), TypeError),
    ("TerminalSink config of another type", lambda: lumen.TerminalSink(3), TypeError),
    ("TerminalConfig count negative",
     lambda: setattr(lumen.TerminalConfig(), "max_queued_lines", -1), TypeError),
    ("TerminalConfig color a str", lambda: setattr(lumen.TerminalConfig(), "color", "x"),
     TypeError),
    ("TerminalSink with no room for a line",
     lambda: lumen.TerminalSink(_terminal_config(max_queued_lines=0)), ValueError),
    ("TerminalSink with an invalid time format",
     lambda: lumen.TerminalSink(_terminal_config(time_format="%Q")), ValueError),
    ("FileConfig path an int", lambda: lumen.FileConfig(3), TypeError),
    ("FileConfig count negative", lambda: lumen.FileConfig("x.log", max_files=-1), TypeError),
    ("FileSink config of another type", lambda: lumen.FileSink(3), TypeError),
    ("FileSink with an empty path", lambda: lumen.FileSink(lumen.FileConfig("")), ValueError),
    # The Core.
    ("add_sink with an int", lambda: CORE.add_sink(3), TypeError),
    ("add_sink predicate an int", lambda: CORE.add_sink(lumen.NullSink(), 3), TypeError),
    ("remove_sink id a str", lambda: CORE.remove_sink("1"), TypeError),
    ("remove_sink id negative", lambda: CORE.remove_sink(-1), TypeError),
    ("sink_exception_count id negative", lambda: CORE.sink_exception_count(-1), TypeError),
    ("diagnostic handler not callable", lambda: CORE.set_diagnostic_handler(3), TypeError),
]


class ArgumentErrorTest(LumenTestCase):
    def test_each_wrong_argument_raises_the_documented_exception_and_logs_nothing(self):
        for what, call, expected in CASES:
            with self.subTest(what):
                with self.assertRaises(expected):
                    call()
        lumen.core().flush()
        self.assertEqual((self.capture.logs, self.capture.metrics, self.capture.progress),
                         ([], [], []))


class ExceptionHierarchyTest(unittest.TestCase):
    def test_library_errors_derive_from_lumen_error_and_the_documented_builtins(self):
        expected = {
            lumen.LumenError: (RuntimeError,),
            lumen.InvalidSinkIdError: (lumen.LumenError, LookupError),
            lumen.SinkBusyError: (lumen.LumenError,),
            lumen.TagTableFullError: (lumen.LumenError,),
            lumen.InvalidPredicateError: (lumen.LumenError, ValueError),
            lumen.InvalidSinkConfigError: (lumen.LumenError, ValueError),
            lumen.FileOpenError: (lumen.LumenError, OSError),
        }
        for cls, bases in expected.items():
            for base in bases:
                with self.subTest(cls=cls.__name__, base=base.__name__):
                    self.assertTrue(issubclass(cls, base))


if __name__ == "__main__":
    unittest.main()
