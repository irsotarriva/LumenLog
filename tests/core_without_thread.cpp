// A Core whose dispatch thread cannot be started. The failure is injected by
// replacing pthread_create in this executable (Linux only: the dynamic
// linker resolves the call made by the C++ runtime to this definition), so
// the library needs no test hook. Every other thread creation is passed on
// to the real function.
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <pthread.h>

#include <atomic>
#include <cerrno>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lumen/core.hpp"
#include "lumen/thread_clock.hpp"
#include "next_pthread_create.hpp"
#include "test_support.hpp"

namespace {

// Read by pthread_create below, which any thread may call.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> refuse_threads{false};

}  // namespace

// The C library's function, so its declaration's parameter names are not ours.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                              void* (*start)(void*), void* argument) {
    if (refuse_threads.load()) {
        return EAGAIN;
    }
    static const lumen::test::PthreadCreate next = lumen::test::next_pthread_create();
    return next(thread, attributes, start, argument);
}

namespace lumen {
namespace {

using test::Captured;
using test::log_to;

// A Core built while thread creation fails, with its diagnostics and a
// capture sink.
class CoreWithoutThreadTest : public ::testing::Test {
protected:
    [[nodiscard]] Core& _core() { return *core_; }
    [[nodiscard]] Captured& _captured() { return *captured_; }
    void _destroy_core() { core_.reset(); }
    [[nodiscard]] std::vector<std::string> _diagnostic_lines() const {
        const std::scoped_lock lock(diagnostics_->mutex);
        return diagnostics_->lines;
    }

private:
    struct Diagnostics {
        std::mutex mutex;
        std::vector<std::string> lines;  // guarded by mutex
    };

    static std::unique_ptr<Core> make_core_(const std::shared_ptr<Diagnostics>& diagnostics) {
        refuse_threads.store(true);
        auto core = std::make_unique<Core>([diagnostics](std::string_view line) {
            const std::scoped_lock lock(diagnostics->mutex);
            diagnostics->lines.emplace_back(line);
        });
        refuse_threads.store(false);
        return core;
    }

    test::CleanContext context_;
    std::shared_ptr<Diagnostics> diagnostics_ = std::make_shared<Diagnostics>();
    std::unique_ptr<Core> core_ = make_core_(diagnostics_);
    std::shared_ptr<Captured> captured_ = test::capture(*core_);
};

TEST_F(CoreWithoutThreadTest, ConstructionSucceedsAndReportsTheFailureOnce) {
    const auto lines = _diagnostic_lines();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("dispatch thread could not be started"), std::string::npos);
}

TEST_F(CoreWithoutThreadTest, RecordIsDeliveredOnTheSubmittingThreadBeforeSubmissionReturns) {
    log_to(_core(), "on this thread");
    uint32_t other_thread_id = 0;
    std::thread other([&] {
        other_thread_id = this_thread_id();
        log_to(_core(), "on another thread");
    });
    other.join();

    const auto logs = _captured().logs();
    ASSERT_EQ(logs.size(), 2U);
    EXPECT_EQ(logs[0].message, "on this thread");
    EXPECT_EQ(logs[1].message, "on another thread");
    EXPECT_EQ(logs[1].thread_id, other_thread_id);
}

TEST_F(CoreWithoutThreadTest, RecordSubmittedFromACallbackIsDeliveredByTheNextFlush) {
    Core& core = _core();
    struct LogsOnce : Sink {
        explicit LogsOnce(Core& target) : core(target) {}
        void on_log(const LogRecord& r) override {
            if (r.message == "first") {
                log_to(core, "from the callback");
            }
        }
        void flush() override {}
        Core& core;
    };
    ASSERT_TRUE(core.add_sink(std::make_unique<LogsOnce>(core)));
    log_to(core, "first");
    EXPECT_EQ(_captured().messages(), (std::vector<std::string>{"first"}));
    core.flush();
    EXPECT_EQ(_captured().messages(), (std::vector<std::string>{"first", "from the callback"}));
}

TEST_F(CoreWithoutThreadTest, DestroyingTheCoreFlushesItsSinksOnceAndDeliversNothingTwice) {
    const std::size_t flushes_before = _captured().flushes();
    log_to(_core(), "last");
    _destroy_core();
    EXPECT_EQ(_captured().messages(), (std::vector<std::string>{"last"}));
    EXPECT_EQ(_captured().flushes(), flushes_before + 1);
}

}  // namespace
}  // namespace lumen
