#ifndef LUMEN_LEVEL_HPP
#define LUMEN_LEVEL_HPP

#include <cstdint>

namespace lumen {

// Severity of a log record, in increasing order. The numeric values are part
// of the interface: LUMEN_MIN_LEVEL_VALUE (lumen/macros.hpp) is one of them.
enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

}  // namespace lumen

#endif
