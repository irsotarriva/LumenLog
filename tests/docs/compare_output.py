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

Line endings are compared exactly, before any masking. Every line, the last
one included, must end with one newline: "\n" in the files the program
writes (Lumen opens them in binary mode), and on standard output and
standard error the newline of the platform's C runtime in text mode, "\n"
except on Windows, where it is "\r\n". So a stray "\r", a CRLF line on
Linux, an LF line on Windows and a missing final newline all fail.

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


def lines_of(text: str) -> list[str]:
    """The lines of `text`, split at "\n" only (the line endings were checked
    before, so no other character counts as one)."""
    lines = text.split("\n")
    return lines[:-1] if lines[-1] == "" else lines


def compare(what: str, actual: str, expected: str) -> list[str]:
    """Problems found comparing `actual` (already masked) with `expected`."""
    problems = []
    actual_lines = lines_of(actual)
    expected_lines = lines_of(expected)
    for k in ("other", "metric", "progress"):
        a = [line for line in actual_lines if kind(line) == k]
        e = [line for line in expected_lines if kind(line) == k]
        if a != e:
            diff = "\n".join(
                difflib.unified_diff(e, a, f"expected ({k} lines)", f"actual ({k} lines)", lineterm="")
            )
            problems.append(f"{what}: the {k} lines differ:\n{diff}")
    return problems


# The newline the C runtime writes to a standard stream in text mode.
STREAM_NEWLINE = "\r\n" if sys.platform == "win32" else "\n"
FILE_NEWLINE = "\n"


def line_ending_problems(what: str, text: str, newline: str) -> list[str]:
    """Problems with the line endings of `text`, whose lines must each end
    with `newline` and contain no other carriage return or line feed."""
    if not text:
        return []
    problems = []
    if not text.endswith(newline):
        problems.append(f"{what}: the last line does not end with {newline!r}")
    for number, line in enumerate(text.split(newline), start=1):
        if "\r" in line or "\n" in line:
            problems.append(f"{what}: line {number} has a line ending other than {newline!r}: {line!r}")
            break
    return problems


def decoded(what: str, data: bytes, newline: str) -> tuple[str, list[str]]:
    """`data` as text with "\n" line endings, and the problems found in its
    encoding and line endings."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as e:
        return data.decode("utf-8", errors="replace"), [f"{what}: not UTF-8 ({e})"]
    problems = line_ending_problems(what, text, newline)
    return (text.replace(newline, "\n") if not problems else text), problems


def expected_text(path: Path) -> str:
    """An expected file, with its line endings read as "\n" whatever the
    checkout made of them."""
    return mask_functions(path.read_text(encoding="utf-8"))


def stream_problems(name: str, data: bytes, expected: Path | None, source_dir: str) -> list[str]:
    text, problems = decoded(name, data, STREAM_NEWLINE)
    actual = mask(text, source_dir)
    if expected is None:
        return problems + ([f"{name}: expected nothing, got:\n{actual}"] if actual else [])
    return problems + compare(name, actual, expected_text(expected))


def file_problems(produced: Path, expected: Path, source_dir: str) -> list[str]:
    if not produced.exists():
        return [f"{produced}: the program did not write this file"]
    text, problems = decoded(str(produced), produced.read_bytes(), FILE_NEWLINE)
    return problems + compare(str(produced), mask(text, source_dir), expected_text(expected))


def parse_arguments(argv: list[str]) -> argparse.Namespace:
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
    args = parser.parse_args(argv)
    args.command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not args.command:
        parser.error("no program to run")
    args.expect_file = [tuple(Path(p) for p in pair.split("=", 1)) for pair in args.expect_file]
    return args


def run_and_compare(args: argparse.Namespace) -> list[str]:
    for produced, _ in args.expect_file:
        produced.unlink(missing_ok=True)
    run = subprocess.run(args.command, capture_output=True, timeout=120)
    problems = []
    if run.returncode != 0:
        problems.append(f"exit status {run.returncode}\nstdout:\n{run.stdout!r}\nstderr:\n{run.stderr!r}")
    problems += stream_problems("stdout", run.stdout, args.expect_stdout, args.source_dir)
    problems += stream_problems("stderr", run.stderr, args.expect_stderr, args.source_dir)
    for produced, expected in args.expect_file:
        problems += file_problems(produced, expected, args.source_dir)
    return problems


def main(argv: list[str]) -> int:
    args = parse_arguments(argv)
    problems = run_and_compare(args)
    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print("output matches: " + " ".join(args.command))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
