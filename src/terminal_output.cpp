#include "terminal_output.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef PLATFORM_WINDOWS
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace lumen::detail {

namespace {

[[nodiscard]] bool standard_error_is_terminal() noexcept {
#ifdef PLATFORM_WINDOWS
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) == 1;
#endif
}

// The window size of the terminal on standard error, if it is one.
[[nodiscard]] std::optional<TerminalSize> standard_error_size() noexcept {
#ifdef PLATFORM_WINDOWS
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    if (console == INVALID_HANDLE_VALUE || console == nullptr ||
        GetConsoleScreenBufferInfo(console, &info) == 0) {
        return std::nullopt;
    }
    const int32_t columns = int32_t{info.srWindow.Right} - int32_t{info.srWindow.Left} + 1;
    const int32_t rows = int32_t{info.srWindow.Bottom} - int32_t{info.srWindow.Top} + 1;
    if (columns <= 0 || rows <= 0 || columns > UINT16_MAX || rows > UINT16_MAX) {
        return std::nullopt;
    }
    return TerminalSize{.columns = static_cast<uint16_t>(columns),
                        .rows = static_cast<uint16_t>(rows)};
#else
    winsize size{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): ioctl(2) is variadic
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col == 0 || size.ws_row == 0) {
        return std::nullopt;
    }
    return TerminalSize{.columns = size.ws_col, .rows = size.ws_row};
#endif
}

// Clears the stream's error state. With exceptions() enabled, clear() to
// goodbit cannot throw; the guard is for a stream type that would.
void clear_state(std::ostream& stream) noexcept {
    try {
        stream.clear();
    } catch (...) {
        // Nothing else can be done; the next write reports the state again.
        return;
    }
}

// Not on Windows, where Auto never colours (see ColorMode::Auto).
#ifndef PLATFORM_WINDOWS
// Read once per sink, at creation, from the thread creating it; nothing in
// Lumen writes the environment.
[[nodiscard]] std::string_view environment(const char* name) noexcept {
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): see above
    return value == nullptr ? std::string_view{} : std::string_view{value};
}

[[nodiscard]] bool environment_disables_color() noexcept {
    // https://no-color.org: set and not empty.
    return !environment("NO_COLOR").empty() || environment("TERM") == "dumb";
}
#endif

}  // namespace

OutputStream::OutputStream(std::shared_ptr<std::ostream> stream) noexcept
    : owned_(std::move(stream))
    , stream_(owned_ ? owned_.get() : &std::cerr)
    , is_terminal_(!owned_ && standard_error_is_terminal()) {}

bool resolve_color(ColorMode mode, const OutputStream& output) noexcept {
    switch (mode) {
        case ColorMode::Always: return true;
        case ColorMode::Never:  return false;
        case ColorMode::Auto:   break;
    }
#ifdef PLATFORM_WINDOWS
    static_cast<void>(output);
    return false;
#else
    return output.is_terminal() && !environment_disables_color();
#endif
}

std::optional<std::string> write_text(std::ostream& stream,
                                      std::span<const std::string_view> pieces) noexcept {
    std::optional<std::string> failure;
    try {
        for (const std::string_view piece : pieces) {
            stream.write(piece.data(), static_cast<std::streamsize>(piece.size()));
        }
        stream.flush();
        if (!stream.good()) {
            failure = "the stream reported an error";
        }
    } catch (const std::exception& e) {
        // A stream with exceptions() enabled throws instead of only setting
        // its state; the state still has to be cleared below.
        failure = std::string("the stream threw: ") + e.what();
    } catch (...) {
        failure = "the stream threw an exception";
    }
    if (failure) {
        clear_state(stream);
    }
    return failure;
}

BatchOutcome write_and_flush(std::ostream& stream, std::span<const std::string> lines) {
    std::vector<std::string_view> pieces(lines.begin(), lines.end());
    std::optional<std::string> failure = write_text(stream, pieces);
    if (!failure) {
        return {};
    }
    return {.failed_lines = lines.size(), .failure = std::move(*failure)};
}

std::optional<TerminalSize> OutputStream::terminal_size() const noexcept {
    if (owned_) {
        return std::nullopt;  // a caller's stream: no descriptor to ask
    }
    return standard_error_size();
}

}  // namespace lumen::detail
