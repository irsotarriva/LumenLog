"""Unit tests of check_docs.py, run by ctest (Docs.Tools.CheckDocs).

The name checks run against the real source tree (the repository this file
is in), on document text written here.
"""

import unittest
from pathlib import Path

import check_docs

ROOT = Path(__file__).resolve().parents[2]
TREE = check_docs.Tree(ROOT)


def name_errors(text: str, doc: str = "docs/page.md") -> list[str]:
    errors: list[str] = []
    check_docs.check_names(TREE, Path(doc), text, errors)
    return errors


class QualifiedNamesTest(unittest.TestCase):
    def assert_rejected(self, text: str, doc: str = "docs/page.md"):
        errors = name_errors(text, doc)
        self.assertEqual(len(errors), 1, errors)
        return errors[0]

    def test_a_free_function_named_as_a_member_of_core_is_rejected(self):
        self.assertIn("lumen::Core::set_process_tag", self.assert_rejected("`lumen::Core::set_process_tag`"))
        self.assertEqual(name_errors("`lumen::set_process_tag`"), [])

    def test_a_member_named_on_the_wrong_class_is_rejected(self):
        self.assertIn("TerminalSink::max_files", self.assert_rejected("`TerminalSink::max_files`"))
        self.assertEqual(name_errors("`FileSink::Config` has `Config::max_files`"), [])

    def test_a_detail_name_is_rejected_even_when_it_exists(self):
        self.assertIn("internal", self.assert_rejected("`lumen::detail::merge_context`"))
        self.assertIn("internal", self.assert_rejected("`detail::merge_context`"))

    def test_the_internals_document_may_name_existing_detail_names_only(self):
        self.assertEqual(name_errors("`detail::merge_context`", "docs/design.md"), [])
        error = self.assert_rejected("`detail::no_such_function`", "docs/design.md")
        self.assertIn("does not exist", error)

    def test_a_name_that_exists_only_in_a_comment_is_rejected(self):
        # "fallbacks" is a word of the header comments, not a declaration.
        self.assert_rejected("`lumen::fallbacks`")

    def test_names_that_exist_are_accepted(self):
        text = ("`lumen::lumen` `lumen::Core::flush` `lumen::NullSink::on_log` `LogLevel::Info` "
                "`lumen::TerminalSink::Config::diagnostics` `lumen::bridges::OpenMpWorkBalance`")
        self.assertEqual(name_errors(text), [])

    def test_scopes_that_are_not_lumen_classes_are_not_checked(self):
        self.assertEqual(name_errors("`Eigen::MatrixXd` `std::vector` `Detector::create`"), [])


class OtherNamesTest(unittest.TestCase):
    def test_a_python_argument_name_is_not_a_module_name(self):
        error = name_errors("`lumen.handler`")
        self.assertEqual(len(error), 1, error)
        self.assertEqual(name_errors("`lumen.core`"), [])

    def test_a_generated_header_counts_as_existing(self):
        self.assertEqual(name_errors("`lumen/config.hpp`"), [])
        self.assertEqual(len(name_errors("`lumen/no_such_header.hpp`")), 1)

    def test_unknown_macros_presets_targets_and_paths_are_rejected(self):
        text = "`LUMEN_NO_SUCH_MACRO` `--preset nightly` `--target deploy` `src/no_such_file.cpp`"
        self.assertEqual(len(name_errors(text)), 4, name_errors(text))
        self.assertEqual(name_errors("`LUMEN_LOG_INFO` `--preset debug` `--target lint` `src/core.cpp`"), [])


class BlocksTest(unittest.TestCase):
    def blocks(self, text: str):
        errors: list[str] = []
        return check_docs.blocks_in(Path("doc.md"), text, errors), errors

    def test_a_block_takes_the_directive_on_the_line_before_its_fence(self):
        blocks, errors = self.blocks('<!-- check: prelude=common wrap=body -->\n\n```cpp\nint x;\n```\n')
        self.assertEqual(errors, [])
        self.assertEqual(len(blocks), 1)
        self.assertEqual(blocks[0].directive, {"prelude": "common", "wrap": "body"})
        self.assertEqual((blocks[0].line, blocks[0].language, blocks[0].text), (3, "cpp", "int x;\n"))

    def test_an_unterminated_block_is_an_error(self):
        _, errors = self.blocks("```cpp\nint x;\n")
        self.assertEqual(errors, ["doc.md:1: unterminated code block"])

    def classify(self, language: str, directive: dict, text: str = "x\n"):
        errors: list[str] = []
        block = check_docs.Block(Path("doc.md"), 1, language, text, directive)
        return check_docs.validate(ROOT, block, errors), errors

    def test_an_unknown_language_is_an_error(self):
        self.assertEqual(self.classify("rust", {})[0], "error")

    def test_a_text_block_needs_a_directive(self):
        kind, errors = self.classify("text", {})
        self.assertEqual(kind, "error")
        self.assertEqual(len(errors), 1)

    def test_illustrative_needs_a_reason(self):
        _, errors = self.classify("cpp", {"illustrative": " "})
        self.assertEqual(errors, ["doc.md:1: illustrative needs a reason"])

    def test_a_cpp_block_with_main_is_a_program_and_an_unknown_key_is_an_error(self):
        kind, errors = self.classify("cpp", {"run": "yes"}, "int main() {}\n")
        self.assertEqual(kind, "cpp-program")
        self.assertEqual(len(errors), 1)

    def test_an_output_block_must_equal_its_file(self):
        kind, errors = self.classify("text", {"output": "examples/expected/quick_start.stderr"}, "other\n")
        self.assertEqual(kind, "output")
        self.assertEqual(errors, ["doc.md:1: the block differs from examples/expected/quick_start.stderr"])


class OptionsTableTest(unittest.TestCase):
    def test_missing_extra_and_wrong_defaults_are_reported(self):
        rows = check_docs.options_in_table(["| Option | Default |", "|---|---|",
                                            "| `LUMEN_A` | `ON` |", "| `LUMEN_C` | `1` |"])
        problems = check_docs.option_problems(rows, {"LUMEN_A": "OFF", "LUMEN_B": "x"})
        self.assertEqual(problems, ["the options table lacks LUMEN_B",
                                    "the options table lists LUMEN_C, which the build lacks",
                                    "LUMEN_A defaults to `OFF`, the table says `ON`"])

    def test_a_computed_default_is_not_compared(self):
        self.assertEqual(check_docs.option_problems({"LUMEN_A": "on if found"}, {"LUMEN_A": "${x}"}), [])


class RepositoryTest(unittest.TestCase):
    def test_the_documents_of_this_repository_have_no_name_errors(self):
        errors: list[str] = []
        check_docs.check_documents(ROOT, errors)
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
