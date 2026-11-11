#include <iostream>

#include "lumen/lumen.hpp"

int main() {
    auto& core = lumen::core();

    auto terminal = lumen::TerminalSink::create({});
    if (!terminal) {
        std::cerr << "cannot create the terminal sink: " << terminal.error().message() << '\n';
        return 1;
    }
    const auto id = core.add_sink(std::move(*terminal), lumen::always());
    (void)id;  // retained for program lifetime

    (void)lumen::set_process_tag("app", "example");
    (void)lumen::set_process_tag("version", "0.1.0");

    std::cout << "Lumen example running. Library is a stub; no output yet.\n";
    return 0;
}
