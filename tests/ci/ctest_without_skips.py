"""Runs ctest and fails if any selected test did not run.

ctest counts a skipped test (GTEST_SKIP(), a SKIP_RETURN_CODE or
SKIP_REGULAR_EXPRESSION match) and a disabled one as "passed" in its summary.
A CI job whose environment is meant to run every test uses this instead:

    python3 tests/ci/ctest_without_skips.py --preset debug --parallel 4

Every argument is passed to ctest, which also writes a JUnit report to a
temporary file. Exit status: ctest's own if it failed; otherwise 1 if the
report lists a test whose status is not "run", with their names; otherwise 0.
"""

import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
from pathlib import Path


def tests_not_run(junit_xml: str) -> list[str]:
    """Names of the test cases in a ctest JUnit report that did not run."""
    root = ElementTree.fromstring(junit_xml)
    return [case.get("name", "?") for case in root.iter("testcase") if case.get("status") != "run"]


def main(ctest_arguments: list[str]) -> int:
    with tempfile.TemporaryDirectory() as directory:
        report = Path(directory) / "ctest.xml"
        result = subprocess.run(["ctest", *ctest_arguments, "--output-junit", str(report)], check=False)
        if result.returncode != 0:
            return result.returncode
        not_run = tests_not_run(report.read_text(encoding="utf-8"))
    for name in not_run:
        print(f"NOT RUN: {name}", file=sys.stderr)
    if not_run:
        print(f"{len(not_run)} test(s) did not run; this job must run every test", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
