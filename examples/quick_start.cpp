// Quick start: a terminal sink, process tags, log statements, a metric and a
// progress bar. Every line goes to standard error.
#include <cstdint>
#include <iostream>
#include <utility>

#include "lumen/lumen.hpp"

int main() {
    lumen::TerminalSink::Config config;
    // Tags are shown only when listed here, in this order.
    config.inline_tags = {"app", "detector", "run"};
    auto terminal = lumen::TerminalSink::create(std::move(config));
    if (!terminal) {
        std::cerr << "cannot create the terminal sink: " << terminal.error().message() << '\n';
        return 1;
    }
    if (const auto id = lumen::core().add_sink(std::move(*terminal)); !id) {
        std::cerr << "cannot register the terminal sink: " << id.error().message() << '\n';
        return 1;
    }

    // Every record from now on carries app=quick_start and run=42.
    for (const auto& [key, value] : {std::pair{"app", "quick_start"}, std::pair{"run", "42"}}) {
        if (const auto set = lumen::set_process_tag(key, value); !set) {
            std::cerr << "cannot set the process tag " << key << ": " << set.error().message()
                      << '\n';
            return 1;
        }
    }

    LUMEN_LOG_INFO("starting");
    const double energy = 41.53;
    LUMEN_LOG_WARN("energy {:.1f} GeV above threshold", energy).tag("detector", "calorimeter");
    LUMEN_LOG_DEBUG("{} channels masked", 3).tag("detector", "tracker");
    LUMEN_LOG_ERROR("explicit tags win").tag("run", 43);

    lumen::metric("loss", 0.25).tag("epoch", 3);

    auto bar = lumen::progress("events", 3);
    for (uint32_t event = 0; event < 3; ++event) {
        bar.tick();
    }
    bar.finish();  // 3/3 was already sent by the last tick(), so nothing more is sent

    // Delivers everything logged so far and waits until the sink has written it.
    lumen::core().flush();
    return 0;
}
