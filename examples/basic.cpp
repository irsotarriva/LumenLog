#include <iostream>

#include "lumen/lumen.h"

int main() {
    auto& core = lumen::core();

    core.add_sink(
        std::make_unique<lumen::TerminalSink>(lumen::TerminalSink::default_config()),
        lumen::always()
    );

    core.set_process_tag("app", "example");
    core.set_process_tag("version", "0.1.0");

    std::cout << "Lumen example running. Library is a stub; no output yet.\n";
    return 0;
}
