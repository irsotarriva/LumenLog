// fork() with a Core in use (lumen/core.hpp, "fork()"). Each test forks; the
// child reports through its exit status and through a pipe, and ends with
// _exit (a child must not return into the test framework).
#include <gtest/gtest.h>

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "lumen/context.hpp"
#include "lumen/core.hpp"
#include "lumen/error.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::log_to;

constexpr std::chrono::seconds generous_timeout{10};

// Child exit statuses.
constexpr int child_passed = 0;
constexpr int child_failed = 1;

// A pipe from the child to the parent.
class Pipe {
public:
    Pipe() {
        std::array<int, 2> ends{-1, -1};
        if (::pipe(ends.data()) == 0) {
            read_end_ = ends[0];
            write_end_ = ends[1];
        }
    }
    ~Pipe() {
        close_write_end();
        if (read_end_ >= 0) {
            ::close(read_end_);
        }
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&&) = delete;
    Pipe& operator=(Pipe&&) = delete;

    [[nodiscard]] bool open() const { return read_end_ >= 0; }
    [[nodiscard]] int write_end() const { return write_end_; }
    void close_write_end() {
        if (write_end_ >= 0) {
            ::close(write_end_);
            write_end_ = -1;
        }
    }
    // Everything written until every write end is closed.
    [[nodiscard]] std::string read_all() {
        close_write_end();
        std::string text;
        std::array<char, 256> buffer{};
        while (true) {
            const ssize_t got = ::read(read_end_, buffer.data(), buffer.size());
            if (got <= 0) {
                return text;
            }
            text.append(buffer.data(), static_cast<std::size_t>(got));
        }
    }

private:
    int read_end_ = -1;
    int write_end_ = -1;
};

// Runs `child` in a child process and returns its exit status, or nullopt if
// it did not exit within generous_timeout (it is then killed).
[[nodiscard]] std::optional<int> in_child(const std::function<int()>& child) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::_exit(child());
    }
    if (pid < 0) {
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + generous_timeout;
    int status = 0;
    while (::waitpid(pid, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            static_cast<void>(::waitpid(pid, &status, 0));
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (!WIFEXITED(status)) {
        return -WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

// Writes each message it receives, one per line, to a pipe, but only in a
// process other than the one that created it (the child); blocks on the
// message "blocks" until released, in the creating process only.
class ChildPipeSink : public Sink {
public:
    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;  // waits on mutex
        bool entered = false;             // guarded by mutex
        bool released = false;            // guarded by mutex
    };

    ChildPipeSink(int fd, std::shared_ptr<Shared> shared)
        : fd_(fd), creator_(::getpid()), shared_(std::move(shared)) {}

    void on_log(const LogRecord& r) override {
        if (::getpid() != creator_) {
            const std::string line = std::string(r.message) + '\n';
            // A failed write shows as a missing line, which the test reports.
            const auto written = ::write(fd_, line.data(), line.size());
            static_cast<void>(written);
            return;
        }
        if (r.message == "blocks") {
            std::unique_lock lock(shared_->mutex);
            shared_->entered = true;
            shared_->changed.notify_all();
            shared_->changed.wait(lock, [this] { return shared_->released; });
        }
    }
    void flush() override {}

private:
    int fd_;
    pid_t creator_;
    std::shared_ptr<Shared> shared_;
};

// Calls `action` from inside on_log.
template <std::invocable<const LogRecord&> F>
class CallbackSink : public Sink {
public:
    explicit CallbackSink(F action) : action_(std::move(action)) {}
    void on_log(const LogRecord& r) override { action_(r); }
    void flush() override {}

private:
    F action_;
};

template <std::invocable<const LogRecord&> F>
[[nodiscard]] std::unique_ptr<Sink> callback_sink(F action) {
    return std::make_unique<CallbackSink<F>>(std::move(action));
}

class ForkTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(pipe_.open()); }

    // Registers a ChildPipeSink and has the dispatch thread block in it, with
    // records queued behind it, as at a fork in the middle of delivery.
    void _block_the_dispatcher_with_records_queued() {
        const auto id = core_.add_sink(std::make_unique<ChildPipeSink>(pipe_.write_end(), shared_));
        ASSERT_TRUE(id);
        sink_id_ = *id;
        log_to(core_, "blocks");
        std::unique_lock lock(shared_->mutex);
        ASSERT_TRUE(
            shared_->changed.wait_for(lock, generous_timeout, [this] { return shared_->entered; }));
        lock.unlock();
        for (int i = 0; i < 3; ++i) {
            log_to(core_, "queued in the parent");
        }
    }

    void _release_the_dispatcher() {
        {
            const std::scoped_lock lock(shared_->mutex);
            shared_->released = true;
        }
        shared_->changed.notify_all();
        core_.flush();
    }

    [[nodiscard]] Core& _core() { return core_; }
    [[nodiscard]] Pipe& _pipe() { return pipe_; }
    [[nodiscard]] std::shared_ptr<ChildPipeSink::Shared> _shared() const { return shared_; }
    [[nodiscard]] SinkId _sink_id() const { return sink_id_; }

private:
    const test::CleanContext context_;
    Pipe pipe_;
    std::shared_ptr<ChildPipeSink::Shared> shared_ = std::make_shared<ChildPipeSink::Shared>();
    Core core_{[](std::string_view /*line*/) {}};
    SinkId sink_id_ = 0;
};

TEST_F(ForkTest, ChildDeliversItsOwnRecordsButNotThoseQueuedInTheParent) {
    _block_the_dispatcher_with_records_queued();

    const std::optional<int> status = in_child([this] {
        log_to(_core(), "from the child");
        return _core().flush_for(generous_timeout) ? child_passed : child_failed;
    });
    _release_the_dispatcher();

    EXPECT_EQ(status, child_passed);
    EXPECT_EQ(_pipe().read_all(), "from the child\n");
    EXPECT_EQ(_core().statistics().logs.queued, 0U);
}

TEST_F(ForkTest, ChildFlushOnItsOwnThreadDeliversWhileTheParentsDispatcherIsInACallback) {
    _block_the_dispatcher_with_records_queued();

    const std::optional<int> status = in_child([this] {
        log_to(_core(), "flushed on the child's thread");
        _core().flush();
        return child_passed;
    });
    _release_the_dispatcher();

    EXPECT_EQ(status, child_passed);
    EXPECT_EQ(_pipe().read_all(), "flushed on the child's thread\n");
}

TEST_F(ForkTest, ChildRemovesASinkWhoseCallbackWasRunningInTheParent) {
    _block_the_dispatcher_with_records_queued();

    const std::optional<int> status = in_child([this] {
        const auto removed = _core().remove_sink(_sink_id());
        return removed && *removed != nullptr ? child_passed : child_failed;
    });
    _release_the_dispatcher();

    EXPECT_EQ(status, child_passed);
}

TEST_F(ForkTest, ForkFromInsideACallbackLeavesTheForkingThreadDeliveringInTheChild) {
    ASSERT_TRUE(_core().add_sink(std::make_unique<ChildPipeSink>(_pipe().write_end(), _shared())));
    auto forked = std::make_shared<std::promise<pid_t>>();
    std::future<pid_t> child = forked->get_future();
    Core& core = _core();
    ASSERT_TRUE(_core().add_sink(callback_sink([&core, forked](const LogRecord& r) {
        if (r.message != "fork here") {
            return;
        }
        const pid_t pid = ::fork();
        if (pid != 0) {
            forked->set_value(pid);
            return;
        }
        // In the child, this thread goes on as the dispatch thread once the
        // callback returns; another thread logs, has it flush, and exits.
        std::thread([&core] {
            log_to(core, "from the child after the fork");
            ::_exit(core.flush_for(generous_timeout) ? child_passed : child_failed);
        }).detach();
    })));
    // Not flush(): it would deliver, and so fork, on this thread. The
    // dispatch thread must be the one in the callback.
    log_to(_core(), "fork here");
    ASSERT_EQ(child.wait_for(generous_timeout), std::future_status::ready);
    const pid_t pid = child.get();

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), child_passed);
    EXPECT_EQ(_pipe().read_all(), "from the child after the fork\n");
}

TEST(ForkProcessTagsTest, ChildSetsProcessTagsWhileAParentThreadKeepsChangingThem) {
    const test::CleanContext context;
    std::atomic<bool> stop{false};
    std::thread changer([&stop] {
        while (!stop.load()) {
            // A full table only makes this iteration change nothing.
            [[maybe_unused]] const auto set = set_process_tag("changing", "yes");
            static_cast<void>(remove_process_tag("changing"));
        }
    });
    uint32_t passed = 0;
    for (uint32_t i = 0; i < 50; ++i) {
        const std::optional<int> status = in_child(
            [] { return set_process_tag("in the child", "1") ? child_passed : child_failed; });
        passed += status == child_passed ? 1 : 0;
    }
    stop.store(true);
    changer.join();

    EXPECT_EQ(passed, 50U);
}

TEST(ForkExitTest, ChildThatOnlyCallsUnderscoreExitEndsWithItsStatus) {
    const test::CleanContext context;
    Core core([](std::string_view /*line*/) {});
    log_to(core, "in the parent");

    EXPECT_EQ(in_child([] { return 7; }), 7);
}

}  // namespace
}  // namespace lumen

#endif
