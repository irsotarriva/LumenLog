#include <gtest/gtest.h>

#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lumen/core.h"
#include "lumen/record.h"
#include "lumen/sink.h"

namespace lumen {
namespace {

// ── extract_class_name unit tests ─────────────────────────────────────────────

TEST(ReflectionTest, ExtractClassFromMethod) {
    EXPECT_EQ(detail::extract_class_name("void MyClass::method()"),
              std::string_view("MyClass"));
}

TEST(ReflectionTest, ExtractClassFromStaticMethod) {
    EXPECT_EQ(detail::extract_class_name("static void MyClass::static_method()"),
              std::string_view("MyClass"));
}

TEST(ReflectionTest, ExtractClassFromConstructor) {
    EXPECT_EQ(detail::extract_class_name("MyClass::MyClass(int)"),
              std::string_view("MyClass"));
}

TEST(ReflectionTest, ExtractClassFromNestedClass) {
    EXPECT_EQ(detail::extract_class_name("void Outer::Inner::deep()"),
              std::string_view("Inner"));
}

TEST(ReflectionTest, ExtractClassFromFreeFunction) {
    EXPECT_EQ(detail::extract_class_name("void free_function(int, double)"),
              std::string_view(""));
}

TEST(ReflectionTest, ExtractClassFromEmptyInput) {
    EXPECT_EQ(detail::extract_class_name(""),
              std::string_view(""));
}

TEST(ReflectionTest, ExtractClassWithoutColons) {
    EXPECT_EQ(detail::extract_class_name("void NoClass(int)"),
              std::string_view(""));
}

TEST(ReflectionTest, ExtractClassWithCrlfOrExtraSpaces) {
    EXPECT_EQ(detail::extract_class_name("void  A::b()"),
              std::string_view("A"));
}

// ── class_name() runtime helpers ──────────────────────────────────────────────

struct TestClass {
    void member_method() {
        auto loc = std::source_location::current();
        captured_class = detail::class_name(loc);
    }
    static std::string captured_class;
};

std::string TestClass::captured_class;

std::string captured_free_class;

}  // namespace
}  // namespace lumen

static std::string g_free_class_capture;

static void reflection_free_function() {
    auto loc = std::source_location::current();
    g_free_class_capture = std::string(lumen::detail::class_name(loc));
}

namespace lumen {
namespace {

TEST(ReflectionTest, ClassNameFromMemberFunctionIsNotEmpty) {
    TestClass obj;
    obj.member_method();
    EXPECT_EQ(TestClass::captured_class, "TestClass");
}

TEST(ReflectionTest, ClassNameFromFreeFunctionIsEmpty) {
    reflection_free_function();
    EXPECT_TRUE(g_free_class_capture.empty());
}

// ── Integration: RecordBuilder populates class_name ───────────────────────────

struct CaptureSink : public Sink {
    std::vector<LogRecord> logs;
    void on_log(const LogRecord& rec) override { logs.push_back(rec); }
    void on_metric(const MetricRecord&) override {}
    void on_progress(const ProgressRecord&) override {}
    void flush() override {}
};

void wait_dispatch(int ms = 100) {
    core().flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

}  // namespace lumen

static void emit_from_free_function() {
    lumen::RecordBuilder(lumen::core().log_buffer(), lumen::LogLevel::INFO, "from free function");
}

namespace lumen {
namespace {

class ReflectionMacroTest : public ::testing::Test {
protected:
    std::vector<SinkId> ids_;

    void TearDown() override {
        for (auto id : ids_) {
            (void)core().remove_sink(id);
        }
        ids_.clear();
    }

    SinkId register_sink(std::unique_ptr<Sink> sink, Predicate pred) {
        SinkId id = core().add_sink(std::move(sink), std::move(pred));
        ids_.push_back(id);
        return id;
    }
};

TEST_F(ReflectionMacroTest, LogFromClassPopulatesClassName) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        struct InnerScope {
            void emit() {
                RecordBuilder(core().log_buffer(), LogLevel::INFO, "from member");
            }
        };
        InnerScope is;
        is.emit();
    }

    wait_dispatch();

    ASSERT_EQ(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].source.class_name,
              std::string_view("InnerScope"));
}

TEST_F(ReflectionMacroTest, LogFromFreeFunctionHasEmptyClassName) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    emit_from_free_function();

    wait_dispatch();

    ASSERT_EQ(raw->logs.size(), 1);
    EXPECT_TRUE(raw->logs[0].source.class_name.empty());
}

// ── Tier 2: C++26 static reflection (class_name_of<T>) ────────────────────────

#ifdef LUMEN_HAS_REFLECTION

TEST(ReflectionTest, ClassNameOfReturnsTypeName) {
    auto name = detail::class_name_of<int>();
    EXPECT_FALSE(name.empty());
}

TEST(ReflectionTest, ClassNameOfMatchesExpected) {
    using MyTestType = int;
    auto name = detail::class_name_of<MyTestType>();
    EXPECT_EQ(name, std::string_view("int"));
}

#endif  // LUMEN_HAS_REFLECTION

}  // namespace
}  // namespace lumen
