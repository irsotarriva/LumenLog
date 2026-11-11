"""Runs a program and compares what it writes with expected files.

Used by ctest for the examples (examples/CMakeLists.txt). The expected files
are also quoted in the documentation, where tests/docs/check_docs.py checks
that the quoted text equals them.

Before comparing, the parts of a line that change from run to run are
replaced by placeholders:

    <src>           the source directory (--source-dir), in file paths
    <time>          a clock time: HH:MM:SS.mmm at the start of a line, or
                    an ISO 8601 UTC time with nanoseconds
    "ts":<ns>       a JSON timestamp in nanoseconds
    <id>            a thread id: [t<id>], thread=<id>, "thread":<id>
    <function>      a function signature: function="..." and "function":"...",
                    whose spelling depends on the compiler

Paths under the source directory are compared with forward slashes whatever
the platform writes.

Records of different types are not delivered in time order with each other
(lumen/core.hpp, "Order"), so the lines are compared per record type: the
metric lines in order, the progress lines in order, and all other lines in
order. Within a type the comparison is exact.

Exit status 0 when everything matches; otherwise 1, with a report.
"""

import argparse
import difflib
import re
import subprocess
import sys
from pathlib import Path

_MASKS = [
    (re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}Z"), "<time>"),
    (re.compile(r"^\d{2}:\d{2}:\d{2}\.\d{3}(?= )", re.MULTILINE), "<time>"),
    (re.compile(r'"ts":\d+'), '"ts":<ns>'),
    (re.compile(r'"thread":\d+'), '"thread":<id>'),
    (re.compile(r"\[t\d+\]"), "[t<id>]"),
    (re.compile(r"(?<=[ \t])thread=\d+"), "thread=<id>"),
]


_FUNCTION_MASKS = [
    (re.compile(r'function="(?:[^"\\]|\\.)*"'), 'function="<function>"'),
    (re.compile(r'"function":"(?:[^"\\]|\\.)*"'), '"function":"<function>"'),
]

# A path below <src>, with either separator, the backslash possibly doubled
# by JSON or logfmt escaping.
_SOURCE_PATH = re.compile(r"<src>(?:(?:/|\\{1,2})[\w.\-]+)+")
_QUOTED_SOURCE_PATH = re.compile(r'file="(<src>[^"]*)"')


def mask_functions(text: str) -> str:
    for pattern, replacement in _FUNCTION_MASKS:
        text = pattern.sub(replacement, text)
    return text


def mask_source_dir(text: str, source_dir: str) -> str:
    """Replaces the source directory by <src> and normalises the path after it."""
    root = source_dir.rstrip("/\\")
    if not root:
        return text
    native = root.replace("/", "\\")
    for spelling in (root, native, native.replace("\\", "\\\\")):
        text = text.replace(spelling, "<src>")
    text = _SOURCE_PATH.sub(lambda m: re.sub(r"\\+", "/", m.group(0)), text)
    # logfmt quotes a value only when it has to: a backslash forces quotes
    # that the same path with forward slashes does not need.
    return _QUOTED_SOURCE_PATH.sub(r"file=\1", text)


def mask(text: str, source_dir: str) -> str:
    text = mask_source_dir(text, source_dir)
    for pattern, replacement in _MASKS:
        text = pattern.sub(replacement, text)
    return mask_functions(text)


def kind(line: str) -> str:
    for k in ("metric", "progress"):
        if re.search(rf'(^| ){k.upper()} |^{k},|"type":"{k}"|(^| )kind={k} ', line):
            return k
    return "other"


def compare(what: str, actual: str, expected: str) -> list[str]:
    """Problems found comparing `actual` (already masked) with `expected`."""
    problems = []
    actual_lines = actual.splitlines()
    expected_lines = expected.splitlines()
    for k in ("other", "metric", "progress"):
        a = [line for line in actual_lines if kind(line) == k]
        e = [line for line in expected_lines if kind(line) == k]
        if a != e:
            diff = "\n".join(
                difflib.unified_diff(e, a, f"expected ({k} lines)", f"actual ({k} lines)", lineterm="")
            )
            problems.append(f"{what}: the {k} lines differ:\n{diff}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source-dir", default="")
    parser.add_argument("--expect-stdout", type=Path)
    parser.add_argument("--expect-stderr", type=Path)
    parser.add_argument(
        "--expect-file",
        action="append",
        default=[],
        metavar="PRODUCED=EXPECTED",
        help="a file the program writes, and the file it must match",
    )
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- program [arguments]")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no program to run")

    produced_files = []
    for pair in args.expect_file:
        produced, _, expected = pair.partition("=")
        produced_files.append((Path(produced), Path(expected)))
        Path(produced).unlink(missing_ok=True)

    run = subprocess.run(command, capture_output=True, text=True, timeout=120)
    problems = []
    if run.returncode != 0:
        problems.append(f"exit status {run.returncode}\nstdout:\n{run.stdout}\nstderr:\n{run.stderr}")

    streams = [("stdout", run.stdout, args.expect_stdout), ("stderr", run.stderr, args.expect_stderr)]
    for name, text, expected in streams:
        actual = mask(text, args.source_dir)
        if expected is None:
            if actual:
                problems.append(f"{name}: expected nothing, got:\n{actual}")
            continue
        problems += compare(name, actual, mask_functions(expected.read_text(encoding="utf-8")))

    for produced, expected in produced_files:
        if not produced.exists():
            problems.append(f"{produced}: the program did not write this file")
            continue
        actual = mask(produced.read_text(encoding="utf-8"), args.source_dir)
        problems += compare(str(produced), actual, mask_functions(expected.read_text(encoding="utf-8")))

    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print("output matches: " + " ".join(command))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
