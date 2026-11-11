"""Records as Python sees them, and LogLevel."""

import gc
import unittest

from support import LumenTestCase, lumen


class RecordTest(LumenTestCase):
    def test_records_cannot_be_created_from_python(self):
        for record_type in (lumen.LogRecord, lumen.MetricRecord, lumen.ProgressRecord):
            with self.subTest(record_type=record_type.__name__):
                with self.assertRaises(TypeError):
                    record_type()

    def test_record_fields_are_read_only(self):
        lumen.info("fixed")
        record = self.only_log()
        for field in ("level", "message", "tags", "timestamp_ns", "thread_id", "source"):
            with self.subTest(field=field):
                with self.assertRaises(AttributeError):
                    setattr(record, field, getattr(record, field))
        with self.assertRaises(AttributeError):
            record.source.file = "elsewhere"

    def test_tags_is_a_new_dict_each_time(self):
        lumen.info("tagged", tags={"k": "v"})
        record = self.only_log()
        record.tags["k"] = "changed"
        self.assertEqual(record.tags, {"k": "v"})

    def test_a_kept_record_stays_intact_after_its_inputs_are_freed(self):
        message = "".join(["kept ", "x" * 1000])
        key = "".join(["key", "-" * 50])
        value = "".join(["value", "+" * 500])
        lumen.info(message, tags={key: value})
        expected = (message, {key: value})
        del message, key, value
        record = self.only_log()
        # Churn the allocator so that freed memory is reused.
        for i in range(200):
            lumen.info("filler " + str(i) * 50, tags={"n": "y" * 300})
        lumen.core().flush()
        gc.collect()
        self.assertEqual((record.message, record.tags), expected)

    def test_source_of_a_record_outlives_the_code_that_logged_it(self):
        namespace = {"lumen": lumen}
        file_name = "/generated/" + "d" * 300 + ".py"
        exec(compile("def produce():\n    lumen.info('generated')\n", file_name, "exec"),
             namespace)
        namespace["produce"]()
        del namespace
        gc.collect()
        record = self.only_log()
        for i in range(200):
            lumen.info("filler " + str(i) * 50)
        lumen.core().flush()
        gc.collect()
        self.assertEqual((record.source.file, record.source.line, record.source.function),
                         (file_name, 2, "produce"))

    def test_source_object_keeps_its_record_alive(self):
        lumen.info("source only")
        source = self.only_log().source
        self.capture.logs.clear()
        gc.collect()
        self.assertEqual(source.function, "RecordTest.test_source_object_keeps_its_record_alive")


class LogLevelTest(unittest.TestCase):
    def test_log_level_is_an_int_enum_with_the_library_values(self):
        self.assertEqual([int(level) for level in lumen.LogLevel], [0, 1, 2, 3, 4, 5])
        self.assertEqual([level.name for level in lumen.LogLevel],
                         ["TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"])
        self.assertEqual(lumen.LogLevel.ERROR, 4)
        self.assertEqual(lumen.LogLevel(3), lumen.LogLevel.WARN)

    def test_levels_order_by_severity(self):
        self.assertLess(lumen.LogLevel.WARN, lumen.LogLevel.ERROR)
        self.assertGreaterEqual(lumen.LogLevel.ERROR, 4)

    def test_an_unknown_number_is_not_a_level(self):
        with self.assertRaises(ValueError):
            lumen.LogLevel(6)


if __name__ == "__main__":
    unittest.main()
