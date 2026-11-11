// Defined before Lumen is included, as -DDEBUG builds and <windows.h> do.
// Macros, not constants, are what this check is about.
// NOLINTBEGIN(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum,cppcoreguidelines-macro-usage)
#define DEBUG 1
#define ERROR 0
#define INFO 2
// NOLINTEND(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum,cppcoreguidelines-macro-usage)

#include "lumen/lumen.hpp"

int main() {
    LUMEN_LOG_ERROR("error from a consumer that defines ERROR");
    LUMEN_LOG_DEBUG("debug from a consumer that defines DEBUG");
    LUMEN_LOG_INFO("info from a consumer that defines INFO");
    lumen::core().flush();
    return 0;
}
