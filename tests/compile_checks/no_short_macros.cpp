#include <string_view>

#define LUMEN_NO_SHORT_MACROS
// A consumer's own LOG_INFO, as another logging library would define it. A
// second definition with a different body would be diagnosed by the compiler.
#define LOG_INFO(message) consumer_log(message)

#include "lumen/lumen.hpp"

#if defined(LOG_TRACE) || defined(LOG_DEBUG) || defined(LOG_WARN) || defined(LOG_ERROR) || \
    defined(LOG_FATAL)
#error "LUMEN_NO_SHORT_MACROS must suppress every LOG_* short name"
#endif

namespace {
int consumer_calls = 0;
void consumer_log(std::string_view /*message*/) {
    ++consumer_calls;
}
}  // namespace

int main() {
    LOG_INFO("through the consumer's macro");
    LUMEN_LOG_INFO("through Lumen's prefixed macro");
    lumen::core().flush();
    return consumer_calls == 1 ? 0 : 1;
}
