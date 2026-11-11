"""Unit tests of compare_output.py, run by ctest (Docs.Tools.CompareOutput)."""

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import compare_output as co

# The newline that a standard stream does not use on this platform.
OTHER_NEWLINE = "\n" if co.STREAM_NEWLINE == "\r\n" else "\r\n"


class LineEndingsTest(unittest.TestCase):
    def test_lines_that_each_end_with_the_newline_pass(self):
        self.assertEqual(co.line_ending_problems("out", "a\nb\n", "\n"), [])
        self.assertEqual(co.line_ending_problems("out", "a\r\nb\r\n", "\r\n"), [])
        self.assertEqual(co.line_ending_problems("out", "", "\n"), [])

    def test_crlf_where_lf_is_expected_fails(self):
        problems = co.line_ending_problems("out", "a\r\nb\r\n", "\n")
        self.assertEqual(len(problems), 1)
        self.assertIn("line 1", problems[0])

    def test_a_bare_lf_where_crlf_is_expected_fails(self):
        self.assertEqual(len(co.line_ending_problems("out", "a\r\nb\nc\r\n", "\r\n")), 1)

    def test_a_missing_final_newline_fails(self):
        problems = co.line_ending_problems("out", "a\nb", "\n")
        self.assertEqual(problems, ["out: the last line does not end with '\\n'"])

    def test_decoded_turns_the_expected_newline_into_lf(self):
        self.assertEqual(co.decoded("out", b"a\r\nb\r\n", "\r\n"), ("a\nb\n", []))

    def test_text_that_is_not_utf8_fails(self):
        self.assertEqual(len(co.decoded("out", b"\xff\n", "\n")[1]), 1)


class MaskTest(unittest.TestCase):
    def test_a_native_windows_path_under_the_source_dir_becomes_a_posix_path(self):
        text = 'file="C:\\\\work\\\\lumen\\\\examples\\\\routing.cpp" line=3\n'
        self.assertEqual(co.mask(text, "C:/work/lumen"), "file=<src>/examples/routing.cpp line=3\n")

    def test_the_function_spelling_of_any_compiler_is_masked(self):
        gcc = co.mask('function="int main(int, char**)"\n', "")
        clang = co.mask('function="int main(int, char **)"\n', "")
        self.assertEqual(gcc, clang)

    def test_lines_of_splits_at_lf_only(self):
        self.assertEqual(co.lines_of("a\x0bb\nc\n"), ["a\x0bb", "c"])


class EndToEndTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        self.expected = self.dir / "expected.txt"
        self.expected.write_text("one\ntwo\n", encoding="utf-8")

    def run_writing(self, data: bytes, *extra: str) -> int:
        """Runs a child that writes `data` to stdout, as raw bytes."""
        code = f"import sys; sys.stdout.buffer.write({data!r})"
        return co.main(["--expect-stdout", str(self.expected), *extra, "--", sys.executable, "-c", code])

    def test_output_with_the_platform_stream_newline_matches(self):
        newline = co.STREAM_NEWLINE.encode()
        self.assertEqual(self.run_writing(b"one" + newline + b"two" + newline), 0)

    def test_output_with_the_other_newline_fails(self):
        newline = OTHER_NEWLINE.encode()
        self.assertEqual(self.run_writing(b"one" + newline + b"two" + newline), 1)

    def test_output_without_a_final_newline_fails(self):
        self.assertEqual(self.run_writing(b"one" + co.STREAM_NEWLINE.encode() + b"two"), 1)

    def test_a_written_file_with_crlf_fails_and_with_lf_matches(self):
        produced = self.dir / "produced.txt"
        script = self.dir / "write.py"
        for content, status in ((b"one\r\ntwo\r\n", 1), (b"one\ntwo\n", 0)):
            script.write_text(f"open({str(produced)!r}, 'wb').write({content!r})\n", encoding="utf-8")
            argv = ["--expect-file", f"{produced}={self.expected}", "--", sys.executable, str(script)]
            self.assertEqual(co.main(argv), status, content)

    def test_a_nonzero_exit_status_fails(self):
        self.assertEqual(co.main(["--", sys.executable, "-c", "raise SystemExit(3)"]), 1)


if __name__ == "__main__":
    unittest.main()
