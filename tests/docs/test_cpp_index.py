"""Unit tests of cpp_index.py, run by ctest (Docs.Tools.CppIndex)."""

import unittest

import cpp_index


def index_of(text: str) -> cpp_index.Index:
    return cpp_index.index_text(text)


class DeclarationsTest(unittest.TestCase):
    def test_functions_classes_and_members_get_their_qualified_names(self):
        index = index_of("""
            namespace lumen {
            class Core {
            public:
                explicit Core(int x = 0);
                [[nodiscard]] std::expected<SinkId, std::error_code> add_sink(std::function<void(int)> f);
            private:
                std::unique_ptr<Impl> impl_;
            };
            inline constexpr std::size_t max_tags = 16;
            [[nodiscard]] Core& core();
            }
        """)
        for name in ("lumen::Core", "lumen::Core::Core", "lumen::Core::add_sink",
                     "lumen::Core::impl_", "lumen::max_tags", "lumen::core"):
            self.assertIn(name, index.names)
        self.assertNotIn("lumen::add_sink", index.names)
        self.assertNotIn("lumen::Core::function", index.names)

    def test_a_member_initialised_by_a_call_is_named_by_the_member_not_the_call(self):
        index = index_of("struct Config { Handler diagnostics = stderr_diagnostics(); };")
        self.assertEqual(index.names, {"Config", "Config::diagnostics"})

    def test_enumerators_belong_to_their_scoped_enumeration(self):
        index = index_of("namespace n { enum class Level : uint8_t { Trace = 0, Debug, Info }; }")
        self.assertTrue({"n::Level::Trace", "n::Level::Debug", "n::Level::Info"} <= index.names)
        self.assertNotIn("n::Trace", index.names)

    def test_nested_namespace_head_is_closed_by_one_brace(self):
        index = index_of("namespace a::b { void f(); } void g();")
        self.assertIn("a::b::f", index.names)
        self.assertIn("g", index.names)
        self.assertNotIn("a::g", index.names)

    def test_names_inside_function_bodies_comments_and_strings_are_not_declarations(self):
        index = index_of("""
            namespace n {
            // void in_comment();
            inline int f() { int local = 0; const char* s = "void in_string();"; return local; }
            /* struct InBlockComment {}; */
            }
        """)
        self.assertEqual(index.names, {"n", "n::f"})

    def test_aliases_concepts_and_constrained_templates_are_declarations(self):
        index = index_of("""
            namespace n {
            using Handler = std::function<void(std::string_view)>;
            template <typename T> concept Element = std::movable<T>;
            template <Element T, std::size_t N> requires(std::has_single_bit(N)) class Buffer {
                static constexpr std::size_t capacity() noexcept { return N; }
            };
            }
        """)
        self.assertTrue({"n::Handler", "n::Element", "n::Buffer", "n::Buffer::capacity"} <= index.names)

    def test_preprocessor_lines_and_friends_declare_nothing(self):
        index = index_of("#define LUMEN_X(a) \\\n  call(a)\nclass C { friend class D; };")
        self.assertEqual(index.names, {"C"})


class ResolutionTest(unittest.TestCase):
    def setUp(self):
        self.index = index_of("""
            namespace lumen {
            class Sink { public: virtual void on_log(); virtual void flush() = 0; };
            class TerminalSink final : public Sink {
            public:
                struct Config { int max_queued_lines = 0; };
                void flush() override;
            };
            void set_process_tag();
            }
        """)

    def test_an_inherited_member_resolves_through_the_base_class(self):
        self.assertTrue(cpp_index.resolves(self.index, "lumen::TerminalSink::on_log"))

    def test_a_free_function_does_not_resolve_as_a_member(self):
        self.assertTrue(cpp_index.resolves(self.index, "lumen::set_process_tag"))
        self.assertFalse(cpp_index.resolves(self.index, "lumen::TerminalSink::set_process_tag"))

    def test_a_member_of_a_nested_class_does_not_resolve_on_the_enclosing_class(self):
        self.assertTrue(cpp_index.resolves(self.index, "lumen::TerminalSink::Config::max_queued_lines"))
        self.assertFalse(cpp_index.resolves(self.index, "lumen::TerminalSink::max_queued_lines"))

    def test_classes_named_finds_every_class_with_that_last_name(self):
        self.assertEqual(cpp_index.classes_named(self.index, "Config"), ["lumen::TerminalSink::Config"])
        self.assertEqual(cpp_index.classes_named(self.index, "set_process_tag"), [])


if __name__ == "__main__":
    unittest.main()
