#ifndef LUMEN_SRC_RECORD_FORMAT_HPP
#define LUMEN_SRC_RECORD_FORMAT_HPP

#include <optional>
#include <string>
#include <vector>

#include "lumen/record.hpp"
#include "lumen/terminal_style.hpp"
#include "time_format.hpp"

// One line of text per record, in each of the three formats that the shipped
// sinks write. The formats are specified with TerminalSink
// (lumen/terminal_sink.hpp) and FileFormat (lumen/file_sink.hpp). Every
// returned line ends with '\n'.
//
// A record built by hand may hold a tag key twice (TagSet::add); every format
// writes only the first, which is the one TagSet::find and the predicates see.

namespace lumen::detail {

struct TextStyle {
    TimeFormat time;                                     // empty: no time column
    std::optional<LevelColors> colors;                   // nullopt: no escapes
    std::optional<std::vector<std::string>> shown_tags;  // nullopt: all, in record order
    bool show_thread = false;
    bool show_source = false;
};

// The time column of FileFormat::Text: 2026-11-11T10:00:00.123456789Z.
[[nodiscard]] TimeFormat iso8601_utc_format();

[[nodiscard]] std::string text_line(const LogRecord& record, const TextStyle& style);
[[nodiscard]] std::string text_line(const MetricRecord& record, const TextStyle& style);
[[nodiscard]] std::string text_line(const ProgressRecord& record, const TextStyle& style);

[[nodiscard]] std::string key_value_line(const LogRecord& record);
[[nodiscard]] std::string key_value_line(const MetricRecord& record);
[[nodiscard]] std::string key_value_line(const ProgressRecord& record);

[[nodiscard]] std::string json_line(const LogRecord& record);
[[nodiscard]] std::string json_line(const MetricRecord& record);
[[nodiscard]] std::string json_line(const ProgressRecord& record);

}  // namespace lumen::detail

#endif
