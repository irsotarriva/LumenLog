// Context layers: which tags a record carries without the log statement
// naming them. Each line on standard error shows the tags app, worker,
// detector, run and event, in that order, for the keys the record has.
//
// Priority, highest first: explicit .tag(), the Tagged object of the
// innermost LUMEN_MEMBER_SCOPE, LUMEN_SCOPE / LUMEN_LOOP (innermost first),
// the thread's tags, the process's tags. The first layer to name a key wins.
#include <cstdint>
#include <expected>
#include <iostream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "lumen/lumen.hpp"

namespace {

// An object whose own tags are carried by the records logged in its member
// scopes.
class Detector : public lumen::Tagged {
public:
    // A factory, because tagging can fail and a constructor cannot report it.
    [[nodiscard]] static std::expected<Detector, std::error_code> create(std::string_view name) {
        Detector detector;
        if (auto tagged = detector.lumen_tag("detector", name); !tagged) {
            return std::unexpected(tagged.error());
        }
        return detector;
    }

    void calibrate(uint32_t channels) const {
        LUMEN_MEMBER_SCOPE;
        LUMEN_LOG_INFO("calibrating {} channels", channels);
    }

private:
    Detector() = default;
};

// Runs on its own thread, so that its thread tag is its own.
[[nodiscard]] std::expected<void, std::error_code> run_worker(const Detector& detector) {
    if (auto set = lumen::set_thread_tag("worker", "1"); !set) {
        return std::unexpected(set.error());
    }
    LUMEN_LOG_INFO("worker started");

    LUMEN_SCOPE("run", "7");
    LUMEN_SCOPE("detector", "none");
    LUMEN_LOG_INFO("inside two scopes");
    detector.calibrate(128);  // the instance tag detector=tracker beats the scope's
    LUMEN_LOG_INFO("explicit tag").tag("run", "8");

    LUMEN_LOOP("event", event, uint32_t{2}) {
        LUMEN_LOG_DEBUG("processing event {}", event);
    }
    return {};
}

}  // namespace

int main() {
    lumen::TerminalSink::Config config;
    config.time_format = "";  // no time column
    config.inline_tags = {"app", "worker", "detector", "run", "event"};
    auto terminal = lumen::TerminalSink::create(std::move(config));
    if (!terminal) {
        std::cerr << "cannot create the terminal sink: " << terminal.error().message() << '\n';
        return 1;
    }
    if (const auto id = lumen::core().add_sink(std::move(*terminal)); !id) {
        std::cerr << "cannot register the terminal sink: " << id.error().message() << '\n';
        return 1;
    }
    if (const auto set = lumen::set_process_tag("app", "context_layers"); !set) {
        std::cerr << "cannot set the process tag: " << set.error().message() << '\n';
        return 1;
    }

    const auto detector = Detector::create("tracker");
    if (!detector) {
        std::cerr << "cannot tag the detector: " << detector.error().message() << '\n';
        return 1;
    }

    std::expected<void, std::error_code> worker_result;
    // std::thread rather than std::jthread: Apple's libc++ lacks jthread.
    std::thread worker([&] { worker_result = run_worker(*detector); });
    worker.join();
    if (!worker_result) {
        std::cerr << "worker failed: " << worker_result.error().message() << '\n';
        return 1;
    }

    LUMEN_LOG_INFO("main thread: no worker tag, no scope");

    lumen::core().flush();
    return 0;
}
