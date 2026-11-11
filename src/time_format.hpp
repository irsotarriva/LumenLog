#ifndef LUMEN_SRC_TIME_FORMAT_HPP
#define LUMEN_SRC_TIME_FORMAT_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "lumen/terminal_style.hpp"

// Timestamps for the text formats. Record timestamps are nanoseconds since
// the Unix epoch on std::chrono::system_clock (lumen::now_ns()).

namespace lumen::detail {

// A validated TerminalSink::Config::time_format pattern: std::strftime
// conversions plus %L (milliseconds) and %N (nanoseconds). The pattern may
// be of any length.
class TimeFormat {
public:
    // Fails with LumenError::InvalidSinkConfig on a '%' that is not followed
    // by an accepted conversion character (%n and %t, which write a newline
    // and a tab, are not accepted) and on a NUL character.
    [[nodiscard]] static std::expected<TimeFormat, std::error_code> parse(std::string_view pattern,
                                                                          TimeZone zone);

    [[nodiscard]] bool empty() const noexcept { return pieces_.empty(); }

    // Appends the formatted time. If the platform cannot convert the time to
    // calendar form, appends the plain nanosecond count instead.
    void append(std::string& out, uint64_t timestamp_ns) const;

private:
    enum class PieceKind : uint8_t {
        Strftime,
        Milliseconds,
        Nanoseconds
    };
    struct Piece {
        PieceKind kind = PieceKind::Strftime;
        std::string pattern;  // for Strftime: a run of literal text and conversions
    };

    TimeFormat(std::vector<Piece> pieces, TimeZone zone)
        : pieces_(std::move(pieces)), zone_(zone) {}
    // The piece of %L or %N; nullopt for any other conversion character.
    [[nodiscard]] static std::optional<PieceKind> subsecond_kind_(char conversion) noexcept;
    [[nodiscard]] static std::expected<std::vector<Piece>, std::error_code> split_(
        std::string_view pattern);
    [[nodiscard]] static bool fits_(const std::vector<Piece>& pieces);

    std::vector<Piece> pieces_;
    TimeZone zone_ = TimeZone::Local;
};

// 2026-11-11: the UTC date of `timestamp_ns`.
[[nodiscard]] std::string utc_date(uint64_t timestamp_ns);

}  // namespace lumen::detail

#endif
