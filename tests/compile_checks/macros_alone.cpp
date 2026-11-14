// The only include: every macro must compile from what macros.hpp brings in.
// It also pins the documented statement form of the log macros: each is a
// complete statement that takes .tag() and may be the unbraced branch of an
// `if` with an `else`.
#include "lumen/macros.hpp"

int main(int argc, char** /*argv*/) {
    LUMEN_LOG_TRACE("trace");
    LUMEN_LOG_DEBUG("debug {}", 1);
    LUMEN_LOG_INFO("info").tag("key", "value");
    LUMEN_LOG_WARN("warn");
    LUMEN_LOG_ERROR("error");
    LOG_INFO("short name");
    // The braces are what this check is about.
    // NOLINTBEGIN(readability-braces-around-statements)
    if (argc > 1)
        LUMEN_LOG_INFO("with arguments").tag("count", argc);
    else
        LUMEN_LOG_WARN("without arguments");
    if (argc > 1)
        LOG_DEBUG("one line {}", argc);
    else
        LOG_ERROR("one line");
    // NOLINTEND(readability-braces-around-statements)
    {
        LUMEN_SCOPE("scope_key", "scope_value");
        LUMEN_LOOP("loop", i, 2) {
            LUMEN_LOG_DEBUG("iteration {}", i);
        }
    }
    // Compiled but not executed: LUMEN_LOG_FATAL terminates the process.
    if (argc > 100) {
        LUMEN_LOG_FATAL("fatal");
    }
    lumen::core().flush();
    return 0;
}
