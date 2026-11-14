// lumen/terminal_style.hpp alone declares what it documents.
#include "lumen/terminal_style.hpp"

#include <array>
#include <type_traits>

namespace lumen {

inline constexpr std::array every_color{AnsiColor::Default, AnsiColor::Black,  AnsiColor::Red,
                                        AnsiColor::Green,   AnsiColor::Yellow, AnsiColor::Blue,
                                        AnsiColor::Magenta, AnsiColor::Cyan,   AnsiColor::White};
inline constexpr std::array every_mode{ColorMode::Auto, ColorMode::Always, ColorMode::Never};
inline constexpr std::array every_zone{TimeZone::Utc, TimeZone::Local};
static_assert(std::is_same_v<LevelColors, std::array<AnsiColor, 6>>);
static_assert(std::is_same_v<decltype(color_for(default_level_colors, LogLevel::Info)), AnsiColor>);

}  // namespace lumen
