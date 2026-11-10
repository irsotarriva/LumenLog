#include <iostream>

#include "lumen/lumen.hpp"

int main() {
    auto& core = lumen::core();

    const auto id = core.add_sink(
        std::make_unique<lumen::TerminalSink>(lumen::TerminalSink::default_config()),
        lumen::always()
    );
    (void)id;  // retained for program lifetime

    (void)lumen::set_process_tag("app", "example");
    (void)lumen::set_process_tag("version", "0.1.0");

    std::cout << "Lumen example running. Library is a stub; no output yet.\n";
    return 0;
}
