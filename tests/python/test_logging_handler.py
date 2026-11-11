"""LumenHandler: records of Python's logging module forwarded to Lumen."""

import inspect
import logging
import unittest

from logging_handler import LumenHandler, lumen_level
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

    def test_a_failure_to_forward_goes_to_handle_error(self):
        handled = []
        handler = LumenHandler()
        handler.handleError = handled.append
        logger = logging.getLogger("lumen.test.handler.failing")
        logger.propagate = False
        logger.addHandler(handler)
        self.addCleanup(logger.removeHandler, handler)
        logger.warning("too big", extra={"huge": 2**70})
        self.assertEqual(len(handled), 1)
        self.assertEqual(self.flushed_logs(), [])


if __name__ == "__main__":
    unittest.main()
