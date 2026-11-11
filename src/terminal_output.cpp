#include "terminal_output.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <streambuf>
#include <string_view>
#include <utility>

#ifdef PLATFORM_WINDOWS
#include <io.h>
#else
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

BatchOutcome write_and_flush(std::ostream& stream, std::span<const std::string> lines) {
    for (const std::string& line : lines) {
        stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    }
    stream.flush();
    if (stream.good()) {
        return {};
    }
    stream.clear();
    return {.failed_lines = lines.size(), .failure = "the stream reported an error"};
}

}  // namespace lumen::detail
