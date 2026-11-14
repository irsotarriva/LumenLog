"""Unit tests of ctest_without_skips.py, run by ctest (CI.CtestWithoutSkips)."""

import unittest

import ctest_without_skips

REPORT = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="(empty)" tests="4" failures="0" disabled="1" skipped="1">
  <testcase name="A.Ran" status="run"><properties/></testcase>
  <testcase name="B.Skipped" status="notrun"><skipped message="SKIP_REGULAR_EXPRESSION_MATCHED"/></testcase>
  <testcase name="C.Disabled" status="disabled"><skipped message="Disabled"/></testcase>
  <testcase name="D.Failed" status="fail"><failure message="Failed"/></testcase>
</testsuite>
"""


class TestsNotRunTest(unittest.TestCase):
    def test_every_case_whose_status_is_not_run_is_listed(self):
        self.assertEqual(ctest_without_skips.tests_not_run(REPORT), ["B.Skipped", "C.Disabled", "D.Failed"])

    def test_a_report_where_everything_ran_lists_nothing(self):
        report = '<testsuite><testcase name="A" status="run"/><testcase name="B" status="run"/></testsuite>'
        self.assertEqual(ctest_without_skips.tests_not_run(report), [])


if __name__ == "__main__":
    unittest.main()
