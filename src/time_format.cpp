#include "time_format.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "lumen/error.hpp"
#include "text_encoding.hpp"

namespace lumen::detail {

namespace {

constexpr uint64_t ns_per_second = 1'000'000'000;
constexpr uint64_t ns_per_millisecond = 1'000'000;
constexpr uint64_t seconds_per_day = 86'400;

// The std::strftime conversions that take no E or O modifier. Anything else
// after '%' is undefined behaviour for std::strftime, so it is refused. %n
// and %t are left out too: they write a newline and a tab, and a record is
// one line whose control characters are escaped.
constexpr std::string_view strftime_conversions = "aAbBcCdDeFgGhHIjmMprRSTuUVwWxXyYzZ%";

// A buffer of this size holds what std::strftime writes for a run of
// ordinary length; a longer run is retried in a buffer of the run's own size.
constexpr std::size_t short_result = 256;
// The most one character of a pattern can produce: %c, the longest
// conversion, writes about 25 characters per two pattern characters in the
// "C" locale; locale names (%B, %Z) stay well below this per character.
constexpr std::size_t max_result_per_pattern_char = 64;

[[nodiscard]] std::optional<std::tm> calendar_time(uint64_t timestamp_ns, TimeZone zone) noexcept {
    const auto seconds = static_cast<std::time_t>(timestamp_ns / ns_per_second);
    std::tm calendar{};
#ifdef PLATFORM_WINDOWS
    const bool ok = zone == TimeZone::Utc ? gmtime_s(&calendar, &seconds) == 0
                                          : localtime_s(&calendar, &seconds) == 0;
#else
    const bool ok = zone == TimeZone::Utc ? gmtime_r(&seconds, &calendar) != nullptr
                                          : localtime_r(&seconds, &calendar) != nullptr;
#endif
    if (!ok) {
        return std::nullopt;
    }
    return calendar;
}

// Returns false if the result does not fit even the large buffer, which a
// run of accepted conversions cannot fill. std::strftime returns 0 both for
// a result that does not fit and for an empty one (%p in some locales), so a
// trailing space is added to the pattern and removed from the result.
[[nodiscard]] bool append_strftime(std::string& out, const std::string& pattern,
                                   const std::tm& calendar) {
    const std::string padded = pattern + ' ';
    std::array<char, short_result> buffer{};
    std::size_t written = std::strftime(buffer.data(), buffer.size(), padded.c_str(), &calendar);
    if (written > 0) {
        out.append(buffer.data(), written - 1);
        return true;
    }
    // Rare: a run of more than about 250 characters.
    std::string large(padded.size() * max_result_per_pattern_char, '\0');
    written = std::strftime(large.data(), large.size(), padded.c_str(), &calendar);
    if (written == 0) {
        return false;
    }
    out.append(large.data(), written - 1);
    return true;
}

struct CivilDay {
    int32_t year = 0;
    uint32_t month = 0;
    uint32_t day = 0;
};

[[nodiscard]] CivilDay civil_day(uint64_t days_since_epoch) {
    const std::chrono::sys_days day{std::chrono::days{static_cast<int64_t>(days_since_epoch)}};
    const std::chrono::year_month_day ymd{day};
    return {static_cast<int32_t>(ymd.year()), static_cast<uint32_t>(ymd.month()),
            static_cast<uint32_t>(ymd.day())};
}

}  // namespace

std::expected<TimeFormat, std::error_code> TimeFormat::parse(std::string_view pattern,
                                                             TimeZone zone) {
    std::expected<std::vector<Piece>, std::error_code> pieces = split_(pattern);
    if (!pieces) {
        return std::unexpected(pieces.error());
    }
    if (!fits_(*pieces)) {
        return std::unexpected(make_error_code(LumenError::InvalidSinkConfig));
    }
    return TimeFormat(std::move(*pieces), zone);
}

std::optional<TimeFormat::PieceKind> TimeFormat::subsecond_kind_(char conversion) noexcept {
    switch (conversion) {
        case 'L': return PieceKind::Milliseconds;
        case 'N': return PieceKind::Nanoseconds;
        default:  return std::nullopt;
    }
}

std::expected<std::vector<TimeFormat::Piece>, std::error_code> TimeFormat::split_(
    std::string_view pattern) {
    if (pattern.find('\0') != std::string_view::npos) {
        // std::strftime would stop at it and silently drop the rest.
        return std::unexpected(make_error_code(LumenError::InvalidSinkConfig));
    }
    std::vector<Piece> pieces;
    std::string run;
    const auto end_run = [&] {
        if (!run.empty()) {
            pieces.push_back({PieceKind::Strftime, std::move(run)});
            run.clear();
        }
    };
    std::size_t next = 0;
    while (next < pattern.size()) {
        const std::size_t percent = std::min(pattern.find('%', next), pattern.size());
        // Literal text is escaped once, here, as a message is, so that the
        // time column can neither break the line nor drive a terminal. The
        // escapes contain no '%'.
        append_text_escaped(run, pattern.substr(next, percent - next));
        if (percent == pattern.size()) {
            break;
        }
        if (percent + 1 == pattern.size()) {
            return std::unexpected(make_error_code(LumenError::InvalidSinkConfig));
        }
        const char conversion = pattern[percent + 1];
        next = percent + 2;
        if (const std::optional<PieceKind> subsecond = subsecond_kind_(conversion)) {
            end_run();
            pieces.push_back({*subsecond, {}});
            continue;
        }
        if (strftime_conversions.find(conversion) == std::string_view::npos) {
            return std::unexpected(make_error_code(LumenError::InvalidSinkConfig));
        }
        run += '%';
        run += conversion;
    }
    end_run();
    return pieces;
}

// A run whose result does not fit for a sample time would not fit for any
// time: the conversions' widths do not depend on the value, except for the
// names (%Z, %B, ...), which are short. With the large buffer of
// append_strftime this does not happen for accepted conversions; the check
// keeps a platform's surprise from reaching append().
bool TimeFormat::fits_(const std::vector<Piece>& pieces) {
    // 1970-01-01: a zero-filled std::tm has day-of-month 0, which is not a
    // date, and std::strftime's behaviour is undefined for it.
    std::tm sample{};
    sample.tm_mday = 1;
    sample.tm_year = 70;
    sample.tm_wday = 4;
    std::string scratch;
    return std::ranges::all_of(pieces, [&](const Piece& piece) {
        return piece.kind != PieceKind::Strftime || append_strftime(scratch, piece.pattern, sample);
    });
}

void TimeFormat::append(std::string& out, uint64_t timestamp_ns) const {
    const std::optional<std::tm> calendar = calendar_time(timestamp_ns, zone_);
    if (!calendar) {
        out += std::to_string(timestamp_ns);
        return;
    }
    const uint64_t subsecond_ns = timestamp_ns % ns_per_second;
    for (const Piece& piece : pieces_) {
        switch (piece.kind) {
            case PieceKind::Strftime:
                // parse() checked that every run fits; a failure here would
                // need a name (%Z, %B) far longer than any platform uses.
                if (!append_strftime(out, piece.pattern, *calendar)) {
                    out += '?';
                }
                break;
            case PieceKind::Milliseconds:
                std::format_to(std::back_inserter(out), "{:03}", subsecond_ns / ns_per_millisecond);
                break;
            case PieceKind::Nanoseconds:
                std::format_to(std::back_inserter(out), "{:09}", subsecond_ns);
                break;
        }
    }
}

std::string utc_date(uint64_t timestamp_ns) {
    const CivilDay day = civil_day(timestamp_ns / ns_per_second / seconds_per_day);
    return std::format("{:04}-{:02}-{:02}", day.year, day.month, day.day);
}

}  // namespace lumen::detail
