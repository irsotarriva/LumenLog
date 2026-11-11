// Logging while a thread's context is being destroyed, with the global core
// still running. Run by ctest as its own process, once per mode:
//
//   static        the destructor of a static constructed after the first use
//                 of lumen::core() logs; it runs after the main thread's
//                 thread_local objects (its context included) are destroyed.
//   thread_local  a worker thread's thread_local objects log in their
//                 destructors, one destroyed before the thread's context and
//                 one after it.
//
// Exit status 0 means every record arrived with exactly the expected tags
// and nothing crashed; the Debug build runs under ASan, which turns a read
// of a destroyed context into a failure.
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "lumen/lumen.hpp"

namespace {

using Tags = std::map<std::string, std::string>;

// What the sink received, checked when this object is destroyed: after the
// core has shut down, because it was constructed before the core.
class Verdict {
public:
    Verdict() = default;
    ~Verdict() {
        const bool ok = check_();
        // The process is already exiting; _Exit reports the verdict as the status.
        std::_Exit(ok ? 0 : 1);
    }
    Verdict(const Verdict&) = delete;
    Verdict& operator=(const Verdict&) = delete;
    Verdict(Verdict&&) = delete;
    Verdict& operator=(Verdict&&) = delete;

    void expect(std::string message, Tags tags) {
        const std::scoped_lock lock(mutex_);
        expected_.emplace(std::move(message), std::move(tags));
    }
    void receive(const lumen::LogRecord& record) {
        Tags tags;
        for (const auto& tag : record.tags) {
            tags.emplace(tag.key, tag.value);
        }
        const std::scoped_lock lock(mutex_);
        received_.emplace(std::string(record.message), std::move(tags));
    }
    void fail(std::string_view why) {
        const std::scoped_lock lock(mutex_);
        failures_.emplace_back(why);
    }

private:
    [[nodiscard]] bool check_() {
        const std::scoped_lock lock(mutex_);
        for (const std::string& failure : failures_) {
            std::cerr << "teardown logging: " << failure << '\n';
        }
        if (received_ != expected_) {
            std::cerr << "teardown logging: received records differ from the expected ones\n";
            for (const auto& [message, tags] : received_) {
                std::cerr << "  received '" << message << "' with " << tags.size() << " tags\n";
            }
        }
        return failures_.empty() && received_ == expected_;
    }

    std::mutex mutex_;
    std::multimap<std::string, Tags> expected_;  // guarded by mutex_
    std::multimap<std::string, Tags> received_;  // guarded by mutex_
    std::vector<std::string> failures_;          // guarded by mutex_
};

// Constructed before main, so before the first use of lumen::core(), and
// therefore destroyed after the core has shut down and delivered everything.
Verdict verdict;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

class VerdictSink : public lumen::Sink {
public:
    void on_log(const lumen::LogRecord& record) override { verdict.receive(record); }
    void flush() override {}
};

class LogsWhenDestroyed {
public:
    explicit LogsWhenDestroyed(std::string message) : message_(std::move(message)) {}
    ~LogsWhenDestroyed() {
        LUMEN_SCOPE("scope", "inner");  // pushes nothing once the context is gone
        LUMEN_LOG_INFO("{}", message_).tag("explicit", "kept");
    }
    LogsWhenDestroyed(const LogsWhenDestroyed&) = delete;
    LogsWhenDestroyed& operator=(const LogsWhenDestroyed&) = delete;
    LogsWhenDestroyed(LogsWhenDestroyed&&) = delete;
    LogsWhenDestroyed& operator=(LogsWhenDestroyed&&) = delete;

private:
    std::string message_;
};

// Checks, while the thread's context is gone, the documented answers of the
// thread-tag functions.
class SetsThreadTagWhenDestroyed {
public:
    SetsThreadTagWhenDestroyed() = default;
    ~SetsThreadTagWhenDestroyed() {
        const auto set = lumen::set_thread_tag("thread", "again");
        if (set || set.error() != std::errc::operation_not_permitted) {
            verdict.fail("set_thread_tag after the context was destroyed did not fail");
        }
        if (lumen::remove_thread_tag("thread")) {
            verdict.fail("remove_thread_tag after the context was destroyed returned true");
        }
    }
    SetsThreadTagWhenDestroyed(const SetsThreadTagWhenDestroyed&) = delete;
    SetsThreadTagWhenDestroyed& operator=(const SetsThreadTagWhenDestroyed&) = delete;
    SetsThreadTagWhenDestroyed(SetsThreadTagWhenDestroyed&&) = delete;
    SetsThreadTagWhenDestroyed& operator=(SetsThreadTagWhenDestroyed&&) = delete;
};

void set_tags(std::string_view thread_value) {
    if (!lumen::set_process_tag("process", "long-enough-to-live-on-the-heap-0123456789") ||
        !lumen::set_thread_tag("thread", thread_value)) {
        verdict.fail("could not set the tags");
    }
}

void run_static_mode() {
    set_tags("main-thread-value-long-enough-to-live-on-the-heap");
    LUMEN_LOG_INFO("from main");
    verdict.expect("from main", {{"process", "long-enough-to-live-on-the-heap-0123456789"},
                                 {"thread", "main-thread-value-long-enough-to-live-on-the-heap"}});
    // Constructed after the first use of core(), so destroyed before the
    // core shuts down, and after the main thread's context.
    static const LogsWhenDestroyed late("from a static destructor");
    static const SetsThreadTagWhenDestroyed checks;
    verdict.expect(
        "from a static destructor",
        {{"explicit", "kept"}, {"process", "long-enough-to-live-on-the-heap-0123456789"}});
}

void run_thread_local_mode() {
    std::thread worker([] {
        // Constructed before the thread's context, so destroyed after it.
        thread_local const LogsWhenDestroyed after_context("after the context");
        thread_local const SetsThreadTagWhenDestroyed checks;
        set_tags("worker-value-long-enough-to-live-on-the-heap");
        // Constructed after the context, so destroyed while it is alive.
        thread_local const LogsWhenDestroyed before_context("before the context");
    });
    worker.join();
    verdict.expect("before the context",
                   {{"explicit", "kept"},
                    {"scope", "inner"},
                    {"process", "long-enough-to-live-on-the-heap-0123456789"},
                    {"thread", "worker-value-long-enough-to-live-on-the-heap"}});
    verdict.expect(
        "after the context",
        {{"explicit", "kept"}, {"process", "long-enough-to-live-on-the-heap-0123456789"}});
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !lumen::core().add_sink(std::make_unique<VerdictSink>())) {
        return 2;
    }
    const std::string_view mode =
        argv[1];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (mode == "static") {
        run_static_mode();
    } else if (mode == "thread_local") {
        run_thread_local_mode();
    } else {
        verdict.fail("unknown mode");
    }
    // Not 0: the status must come from Verdict's destructor, which replaces it.
    return 3;
}
