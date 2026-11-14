#ifndef LUMEN_TERMINAL_STYLE_HPP
#define LUMEN_TERMINAL_STYLE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "lumen/level.hpp"

// Colour and clock settings shared by TerminalSink and DashboardSink.

namespace lumen {

// The eight basic ANSI foreground colours, plus the terminal's own default.
enum class AnsiColor : uint8_t {
    Default,
    Black,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
    White,
};

// Whether a sink writes ANSI colour escapes.
enum class ColorMode : uint8_t {
    // Colour only when all of these hold: the destination is the process's
    // standard error, standard error is a terminal, the NO_COLOR environment
    // variable is unset or empty, TERM is not "dumb", and the platform is not
    // Windows (whose console needs a mode switch that Lumen does not make).
    // Decided once, when the sink is created.
    Auto,
    Always,
    Never,
};

// One colour per level, indexed by the level's numeric value (Trace = 0 ...
// Fatal = 5). Look a colour up with color_for(), which is total over every
// value a LogLevel can hold.
using LevelColors = std::array<AnsiColor, 6>;

// The colours a sink starts with.
inline constexpr LevelColors default_level_colors = {
    AnsiColor::Default,  // Trace
    AnsiColor::Cyan,     // Debug
    AnsiColor::Green,    // Info
    AnsiColor::Yellow,   // Warn
    AnsiColor::Red,      // Error
    AnsiColor::Magenta,  // Fatal
};

// The colour for `level`; AnsiColor::Default for a value outside the enum
// (a LogLevel can hold any uint8_t, for instance one set from Python).
[[nodiscard]] constexpr AnsiColor color_for(const LevelColors& colors, LogLevel level) noexcept {
    const auto index = static_cast<std::size_t>(level);
    return index < colors.size() ? colors.at(index) : AnsiColor::Default;
}

// The clock a human-readable timestamp is shown in.
enum class TimeZone : uint8_t {
    Local,  // the process's local time zone (TZ on POSIX)
    Utc,
};

}  // namespace lumen

#endif
