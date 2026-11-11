"""LumenHandler: records of Python's logging module forwarded to Lumen."""

import inspect
import logging
import unittest

from logging_handler import LumenHandler, lumen_level, lumen_tags
from support import LumenTestCase, lumen


class LumenHandlerTest(LumenTestCase):
    def setUp(self):
        super().setUp()
        self.logger = logging.getLogger("lumen.test.handler")
        self.logger.setLevel(1)
        self.logger.propagate = False
        handler = LumenHandler()
        self.logger.addHandler(handler)
        self.addCleanup(self.logger.removeHandler, handler)

    def test_standard_levels_map_to_lumen_levels(self):
        self.logger.debug("d")
        self.logger.info("i")
        self.logger.warning("w")
        self.logger.error("e")
        logs = self.flushed_logs()
        self.assertEqual([(r.level, r.message) for r in logs],
                         [(lumen.LogLevel.DEBUG, "d"), (lumen.LogLevel.INFO, "i"),
                          (lumen.LogLevel.WARN, "w"), (lumen.LogLevel.ERROR, "e")])
        self.assertTrue(all("logging.level" not in r.tags for r in logs))

    def test_critical_maps_to_error_tagged_with_its_level_name_and_does_not_terminate(self):
        self.logger.critical("c")
        record = self.only_log()
        self.assertEqual(record.level, lumen.LogLevel.ERROR)
        self.assertEqual(record.tags["logging.level"], "CRITICAL")

    def test_custom_levels_map_to_the_highest_lumen_level_at_or_below(self):
        self.assertEqual([lumen_level(n) for n in (1, 9, 10, 25, 39, 45, 60)],
                         [lumen.LogLevel.TRACE, lumen.LogLevel.TRACE, lumen.LogLevel.DEBUG,
                          lumen.LogLevel.INFO, lumen.LogLevel.WARN, lumen.LogLevel.ERROR,
                          lumen.LogLevel.ERROR])
        self.logger.log(5, "fine")
        record = self.only_log()
        self.assertEqual(record.level, lumen.LogLevel.TRACE)
        self.assertEqual(record.tags["logging.level"], "Level 5")

    def test_logger_name_call_site_and_formatted_message_are_forwarded(self):
        line = inspect.currentframe().f_lineno + 1
        self.logger.info("value %d of %s", 3, "x")
        record = self.only_log()
        self.assertEqual(record.message, "value 3 of x")
        self.assertEqual(record.tags, {"logger": "lumen.test.handler"})
        self.assertEqual((record.source.file, record.source.line, record.source.function),
                         (__file__, line,
                          "test_logger_name_call_site_and_formatted_message_are_forwarded"))

    def test_extra_fields_become_tags(self):
        self.logger.info("with extra", extra={"epoch": 3, "lr": 0.5, "ok": True,
                                              "path": "/data", "shape": (2, 3)})
        self.assertEqual(self.only_log().tags,
                         {"epoch": "3", "lr": "0.5", "ok": "true", "path": "/data",
                          "shape": "(2, 3)", "logger": "lumen.test.handler"})

    def test_exception_text_is_part_of_the_message(self):
        try:
            raise ValueError("bad input")
        except ValueError:
            self.logger.exception("failed")
        message = self.only_log().message
        self.assertTrue(message.startswith("failed\nTraceback"), message)
        self.assertIn("ValueError: bad input", message)

    def test_a_record_with_ints_outside_64_bits_is_forwarded_with_their_decimal_text(self):
        self.logger.info("ranges", extra={"huge": 2**70, "below": -(2**63) - 1,
                                          "lowest": -(2**63), "highest": 2**64 - 1})
        self.assertEqual(self.only_log().tags,
                         {"logger": "lumen.test.handler", "huge": str(2**70),
                          "below": str(-(2**63) - 1), "lowest": str(-(2**63)),
                          "highest": str(2**64 - 1)})

    def test_lumen_tags_passes_ints_within_int64_or_uint64_as_ints_and_others_as_str(self):
        record = logging.LogRecord("n", logging.INFO, "p", 1, "m", None, None)
        record.__dict__.update({"huge": 2**70, "below": -(2**63) - 1,
                                "lowest": -(2**63), "highest": 2**64 - 1, "flag": True})
        self.assertEqual(lumen_tags(record),
                         {"logger": "n", "huge": str(2**70), "below": str(-(2**63) - 1),
                          "lowest": -(2**63), "highest": 2**64 - 1, "flag": True})
        self.assertIs(type(lumen_tags(record)["lowest"]), int)

    def test_logger_tag_is_kept_and_later_extra_fields_dropped_past_the_record_limit(self):
        dropped_before = lumen.core().statistics().tags_dropped
        self.logger.info("many", extra={f"k{i}": i for i in range(20)})
        tags = self.only_log().tags
        self.assertEqual(tags, {"logger": "lumen.test.handler",
                                **{f"k{i}": str(i) for i in range(15)}})
        self.assertEqual(lumen.core().statistics().tags_dropped - dropped_before, 5)

    def test_extra_fields_named_like_the_handlers_tags_are_renamed_not_overwritten(self):
        self.logger.critical("clash", extra={"logger": "other", "logging.level": "custom",
                                             "extra.logger": "taken"})
        self.assertEqual(self.only_log().tags,
                         {"logger": "lumen.test.handler", "logging.level": "CRITICAL",
                          "extra.logger": "other", "extra.logging.level": "custom",
                          "extra.extra.logger": "taken"})

    def test_a_failure_to_forward_goes_to_handle_error_and_logs_nothing(self):
        class FailingFormatter(logging.Formatter):
            def format(self, record):
                raise ValueError("cannot format")

        handled = []
        handler = LumenHandler()
        handler.setFormatter(FailingFormatter())
        handler.handleError = handled.append
        logger = logging.getLogger("lumen.test.handler.failing")
        logger.propagate = False
        logger.addHandler(handler)
        self.addCleanup(logger.removeHandler, handler)
        logger.warning("unformattable")
        self.assertEqual([r.getMessage() for r in handled], ["unformattable"])
        self.assertEqual(self.flushed_logs(), [])


if __name__ == "__main__":
    unittest.main()
