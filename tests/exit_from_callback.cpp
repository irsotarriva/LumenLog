// exit() called from inside a sink callback of the global core, on the
// dispatch thread (argument "on_log") or on a thread inside flush()
// (argument "flush"). Run by ctest as its own process: exit status 0 means
// the process neither aborted nor hung, the record in flight and the records
// still queued reached the other sink, that sink was flushed, and no sink was
// destroyed (they may be in a call on the exiting thread's stack).
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lumen/lumen.hpp"

namespace {

// What the sinks saw, read by the verdict during static destruction, hence
// global. Each is written by the delivering thread and read after it exited.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::mutex seen_mutex;
std::vector<std::string> seen;  // guarded by seen_mutex
std::atomic<int> checker_flushes{0};
std::atomic<int> sinks_destroyed{0};
std::atomic<bool> all_submitted{false};
std::atomic<bool> never_set{false};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Calls exit(0) on the record "exit now" (on_log) or on its first flush.
class ExitingSink : public lumen::Sink {
public:
    explicit ExitingSink(bool exit_on_flush) : exit_on_flush_(exit_on_flush) {}
    ~ExitingSink() override { sinks_destroyed.fetch_add(1); }
    ExitingSink(const ExitingSink&) = delete;
    ExitingSink& operator=(const ExitingSink&) = delete;
    ExitingSink(ExitingSink&&) = delete;
    ExitingSink& operator=(ExitingSink&&) = delete;

    void on_log(const lumen::LogRecord& r) override {
        if (!exit_on_flush_ && r.message == "exit now") {
            // Exit only once main has queued everything it logs.
            while (!all_submitted.load()) {
                std::this_thread::yield();
            }
            // Calling exit() from a callback is what this test is about.
            std::exit(0);  // NOLINT(concurrency-mt-unsafe)
        }
    }
    void flush() override {
        if (exit_on_flush_) {
            LUMEN_LOG_INFO("logged by the exiting flush");
            std::exit(0);  // NOLINT(concurrency-mt-unsafe): as above
        }
    }

private:
    bool exit_on_flush_;
};

// Registered after ExitingSink, so it is offered each record later.
class CheckingSink : public lumen::Sink {
public:
    CheckingSink() = default;
    ~CheckingSink() override { sinks_destroyed.fetch_add(1); }
    CheckingSink(const CheckingSink&) = delete;
    CheckingSink& operator=(const CheckingSink&) = delete;
    CheckingSink(CheckingSink&&) = delete;
    CheckingSink& operator=(CheckingSink&&) = delete;

    void on_log(const lumen::LogRecord& r) override {
        const std::scoped_lock lock(seen_mutex);
        seen.emplace_back(r.message);
    }
    void flush() override { checker_flushes.fetch_add(1); }
};

[[nodiscard]] std::vector<std::string> expected_records(std::string_view mode) {
    if (mode == "flush") {
        return {"exit now", "queued after", "logged by the exiting flush"};
    }
    return {"exit now", "queued after"};
}

// Constructed before main, so before the first use of lumen::core(), and
// therefore destroyed after the core has shut down: gives the verdict.
class Verdict {
public:
    Verdict() = default;
    ~Verdict() {
        std::vector<std::string> received;
        {
            const std::scoped_lock lock(seen_mutex);
            received = seen;
        }
        const bool ok = received == expected_records(mode) && checker_flushes.load() >= 1 &&
                        sinks_destroyed.load() == 0;
        if (!ok) {
            std::cerr << "exit from callback (" << mode << "): received";
            for (const std::string& message : received) {
                std::cerr << " [" << message << ']';
            }
            std::cerr << " flushes=" << checker_flushes.load()
                      << " sinks destroyed=" << sinks_destroyed.load() << '\n';
        }
        // The process is already exiting; _Exit reports the verdict as the status.
        std::_Exit(ok ? 0 : 1);
    }
    Verdict(const Verdict&) = delete;
    Verdict& operator=(const Verdict&) = delete;
    Verdict(Verdict&&) = delete;
    Verdict& operator=(Verdict&&) = delete;

    std::string mode;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
Verdict verdict;

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> args(arguments.begin(), arguments.end());
    if (args.size() != 2 || (args[1] != "on_log" && args[1] != "flush")) {
        std::cerr << "usage: lumen_test_exit_from_callback on_log|flush\n";
        std::_Exit(2);
    }
    verdict.mode = args[1];
    const bool exit_on_flush = args[1] == "flush";
    if (!lumen::core().add_sink(std::make_unique<ExitingSink>(exit_on_flush)) ||
        !lumen::core().add_sink(std::make_unique<CheckingSink>())) {
        std::_Exit(2);
    }
    LUMEN_LOG_INFO("exit now");
    LUMEN_LOG_INFO("queued after");
    all_submitted.store(true);
    if (exit_on_flush) {
        lumen::core().flush();
    } else {
        // The dispatch thread exits the process while this thread waits.
        while (!never_set.load()) {
            std::this_thread::yield();
        }
    }
    std::_Exit(3);  // flush() returned: exit() was never called
}
