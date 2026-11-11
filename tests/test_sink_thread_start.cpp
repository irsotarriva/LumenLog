// The sinks' create() when their thread cannot be started: an error code, no
// exception. pthread_create is replaced in this executable (Linux only, where
// the executable's definition takes precedence over the C library's), so
// these tests run in an executable of their own.
#include <gtest/gtest.h>

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <cerrno>
#include <memory>
#include <sstream>
#include <system_error>

#include "lumen/error.hpp"
#include "lumen/file_sink.hpp"
#include "lumen/terminal_sink.hpp"
#include "sink_test_support.hpp"

#ifdef LUMEN_TEST_DASHBOARD
#include "lumen/dashboard_sink.hpp"
#endif

namespace {

// Set by a test while it expects thread creation to fail; read by every
// thread that creates one, hence atomic. A global because pthread_create,
// below, can reach nothing else; only NoNewThreads writes it.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> refuse_threads{false};

using PthreadCreate = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

}  // namespace

// The C library's declaration names its parameters with reserved identifiers.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                              void* (*start)(void*), void* argument) {
    if (refuse_threads.load()) {
        return EAGAIN;
    }
    // The next definition: the C library's, or a sanitizer's interceptor.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): dlsym returns void*
    static const auto next = reinterpret_cast<PthreadCreate>(dlsym(RTLD_NEXT, "pthread_create"));
    return next(thread, attributes, start, argument);
}

namespace lumen {
namespace {

// Refuses new threads for the lifetime of the object.
class NoNewThreads {
public:
    NoNewThreads() { refuse_threads.store(true); }
    ~NoNewThreads() { refuse_threads.store(false); }
    NoNewThreads(const NoNewThreads&) = delete;
    NoNewThreads& operator=(const NoNewThreads&) = delete;
    NoNewThreads(NoNewThreads&&) = delete;
    NoNewThreads& operator=(NoNewThreads&&) = delete;
};

const std::error_code out_of_threads =
    std::make_error_code(std::errc::resource_unavailable_try_again);

TEST(SinkThreadStartTest, TerminalSinkCreateReturnsTheErrorWhenItsThreadCannotStart) {
    TerminalSink::Config config;
    config.output = std::make_shared<std::ostringstream>();
    const NoNewThreads refused;
    const auto sink = TerminalSink::create(std::move(config));
    ASSERT_FALSE(sink.has_value());
    EXPECT_EQ(sink.error(), out_of_threads);
}

TEST(SinkThreadStartTest, FileSinkCreateReturnsTheErrorWhenItsThreadCannotStart) {
    const test::TempDir dir;
    FileSink::Config config;
    config.path = dir / "out.log";
    const NoNewThreads refused;
    const auto sink = FileSink::create(std::move(config));
    ASSERT_FALSE(sink.has_value());
    EXPECT_EQ(sink.error(), out_of_threads);
}

TEST(SinkThreadStartTest, SinksCanBeCreatedAgainOnceThreadsStartAgain) {
    TerminalSink::Config config;
    config.output = std::make_shared<std::ostringstream>();
    EXPECT_TRUE(TerminalSink::create(std::move(config)).has_value());
}

#ifdef LUMEN_TEST_DASHBOARD
TEST(SinkThreadStartTest, DashboardSinkCreateReturnsTheErrorAndLeavesTheTerminalAsItWas) {
    const auto text = std::make_shared<std::ostringstream>();
    DashboardSink::Config config;
    config.output = text;
    config.assume_terminal = true;
    const NoNewThreads refused;
    const auto sink = DashboardSink::create(std::move(config));
    ASSERT_FALSE(sink.has_value());
    EXPECT_EQ(sink.error(), out_of_threads);
    // Entered, then left again: the screen is as it was.
    EXPECT_EQ(text->str(), "\x1b[?1049h\x1b[?25l\x1b[?25h\x1b[?1049l");
}
#endif

}  // namespace
}  // namespace lumen
