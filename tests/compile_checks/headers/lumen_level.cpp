// lumen/level.hpp alone declares what it documents.
#include "lumen/level.hpp"

#include <array>

namespace lumen {

inline constexpr std::array every_level{LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                                        LogLevel::Warn,  LogLevel::Error, LogLevel::Fatal};

}  // namespace lumen
