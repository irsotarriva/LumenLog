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

namespace lumen::detail {

namespace {

constexpr uint64_t ns_per_second = 1'000'000'000;
constexpr uint64_t ns_per_millisecond = 1'000'000;
constexpr uint64_t seconds_per_day = 86'400;
constexpr uint64_t seconds_per_hour = 3'600;
constexpr uint64_t seconds_per_minute = 60;

// The std::strftime conversions that take no E or O modifier. Anything else
// after '%' is undefined behaviour for std::strftime, so it is refused.
constexpr std::string_view strftime_conversions = "aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%";

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

// Returns false if the result does not fit the buffer. std::strftime returns
// 0 both for that and for an empty result (%p in some locales), so a trailing
// space is added to the pattern and removed from the result.
[[nodiscard]] bool append_strftime(std::string& out, const std::string& pattern,
                                   const std::tm& calendar) {
    const std::string padded = pattern + ' ';
    std::array<char, 256> buffer{};
    const std::size_t written =
        std::strftime(buffer.data(), buffer.size(), padded.c_str(), &calendar);
    if (written == 0) {
        return false;
    }
    out.append(buffer.data(), written - 1);
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

std::expected<std::vector<TimeFormat::Piece>, std::error_code> TimeFormat::split_(
    std::string_view pattern) {
    std::vector<Piece> pieces;
    std::string run;
    const auto end_run = [&] {
        if (!run.empty()) {
            pieces.push_back({PieceKind::Strftime, std::move(run)});
            run.clear();
        }
    };
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            run += pattern[i];
            continue;
        }
        if (++i == pattern.size()) {
            return std::unexpected(make_error_code(LumenError::InvalidSinkConfig));
        }
        const char conversion = pattern[i];
        if (conversion == 'L' || conversion == 'N') {
            end_run();
            pieces.push_back(
                {conversion == 'L' ? PieceKind::Milliseconds : PieceKind::Nanoseconds, {}});
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

// A run that does not fit the buffer for a sample time is too long for any
// time: the conversions' widths do not depend on the value, except for the
// names (%Z, %B, ...), which are short.
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

void append_iso8601_utc(std::string& out, uint64_t timestamp_ns) {
    const uint64_t seconds = timestamp_ns / ns_per_second;
    const uint64_t second_of_day = seconds % seconds_per_day;
    const CivilDay day = civil_day(seconds / seconds_per_day);
    std::format_to(std::back_inserter(out), "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:09}Z", day.year,
                   day.month, day.day, second_of_day / seconds_per_hour,
                   second_of_day % seconds_per_hour / seconds_per_minute,
                   second_of_day % seconds_per_minute, timestamp_ns % ns_per_second);
}

std::string utc_date(uint64_t timestamp_ns) {
    const CivilDay day = civil_day(timestamp_ns / ns_per_second / seconds_per_day);
    return std::format("{:04}-{:02}-{:02}", day.year, day.month, day.day);
}

}  // namespace lumen::detail
