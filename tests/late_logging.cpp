// Logging during static destruction, after the global core has shut down.
// Run by ctest as its own process: exit status 0 means the late records were
// counted as dropped_after_shutdown, the record logged in main was delivered
// at shutdown, a sink added after shutdown was refused with CoreShutDown and
// destroyed, and nothing crashed (the Debug build runs under ASan).
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

#include "lumen/lumen.hpp"

namespace {

// Read during static destruction, hence global.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> delivered_in_main{0};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> late_sinks_destroyed{0};

class CountingSink : public lumen::Sink {
public:
    void on_log(const lumen::LogRecord& r) override {
        if (r.message == "from main") {
            delivered_in_main.fetch_add(1);
        }
    }
    void flush() override {}
};

class LateSink : public lumen::Sink {
public:
    LateSink() = default;
    ~LateSink() override { late_sinks_destroyed.fetch_add(1); }
    LateSink(const LateSink&) = delete;
    LateSink& operator=(const LateSink&) = delete;
    LateSink(LateSink&&) = delete;
    LateSink& operator=(LateSink&&) = delete;
    void flush() override {}
};

// Constructed before main, so before the first use of lumen::core(), and
// therefore destroyed after the core has shut down.
class LogsWhenDestroyed {
public:
    LogsWhenDestroyed() = default;
    ~LogsWhenDestroyed() {
        LUMEN_LOG_INFO("from a static destructor").tag("late", true);
        lumen::metric("late_metric", 1.0);
        lumen::progress("late_bar", 1).finish();
        lumen::core().flush();  // must do nothing, not hang
        const auto added = lumen::core().add_sink(std::make_unique<LateSink>());
        const bool refused = !added && added.error() == lumen::LumenError::CoreShutDown;
        const lumen::CoreStatistics stats = lumen::core().statistics();
        const bool ok = delivered_in_main.load() == 1 && stats.logs.dropped_after_shutdown == 1 &&
                        stats.metrics.dropped_after_shutdown == 1 &&
                        stats.progress.dropped_after_shutdown == 1 && refused &&
                        late_sinks_destroyed.load() == 1;
        if (!ok) {
            std::cerr << "late logging: delivered_in_main=" << delivered_in_main.load()
                      << " logs.dropped_after_shutdown=" << stats.logs.dropped_after_shutdown
                      << " late add_sink refused=" << refused
                      << " late sinks destroyed=" << late_sinks_destroyed.load() << '\n';
        }
        // The process is already exiting; _Exit reports the verdict as the status.
        std::_Exit(ok ? 0 : 1);
    }
    LogsWhenDestroyed(const LogsWhenDestroyed&) = delete;
    LogsWhenDestroyed& operator=(const LogsWhenDestroyed&) = delete;
    LogsWhenDestroyed(LogsWhenDestroyed&&) = delete;
    LogsWhenDestroyed& operator=(LogsWhenDestroyed&&) = delete;
};

const LogsWhenDestroyed logs_when_destroyed;

}  // namespace

int main() {
    if (!lumen::core().add_sink(std::make_unique<CountingSink>())) {
        return 2;
    }
    // Queued, not flushed: shutdown must deliver it.
    LUMEN_LOG_INFO("from main");
    // Not 0: the status must come from the destructor above, which replaces
    // it. If that destructor never ran, the test fails with this status.
    return 3;
}
