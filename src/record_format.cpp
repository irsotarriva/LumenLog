#include "record_format.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "text_encoding.hpp"

namespace lumen::detail {

namespace {

constexpr std::string_view ansi_reset = "\x1b[0m";
// Level names are padded to the longest of the six (UNKNOWN is not cut).
constexpr std::size_t level_width = 5;

[[nodiscard]] std::string_view ansi_code(AnsiColor color) noexcept {
    switch (color) {
        case AnsiColor::Default: return "";
        case AnsiColor::Black:   return "\x1b[30m";
        case AnsiColor::Red:     return "\x1b[31m";
        case AnsiColor::Green:   return "\x1b[32m";
        case AnsiColor::Yellow:  return "\x1b[33m";
        case AnsiColor::Blue:    return "\x1b[34m";
        case AnsiColor::Magenta: return "\x1b[35m";
        case AnsiColor::Cyan:    return "\x1b[36m";
        case AnsiColor::White:   return "\x1b[37m";
    }
    return "";
}

// Calls `visit(key, value)` for each tag whose key did not occur earlier in
// the set. This is the one loop over a record's tags.
template <std::invocable<std::string_view, std::string_view> Visit>
void for_each_unique_tag(const RecordTags& tags, Visit visit) {
    const std::span<const RecordTags::Entry> entries(tags.begin(), tags.count());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const RecordTags::Entry& entry = entries[i];
        const bool seen = std::ranges::any_of(
            entries.first(i),
            [&](const RecordTags::Entry& earlier) { return earlier.key == entry.key; });
        if (!seen) {
            visit(entry.key, entry.value);
        }
    }
}

template <typename... Args>
void append_format(std::string& out, std::format_string<Args...> fmt, Args&&... args) {
    std::format_to(std::back_inserter(out), fmt, std::forward<Args>(args)...);
}

// Text format.

void append_time_column(std::string& out, const TextStyle& style, uint64_t timestamp_ns) {
    if (!style.time.empty()) {
        style.time.append(out, timestamp_ns);
        out += ' ';
    }
}

void append_level_column(std::string& out, const TextStyle& style, LogLevel level) {
    const std::string_view name = level_name(level);
    const std::string_view code =
        style.colors ? ansi_code(color_for(*style.colors, level)) : std::string_view{};
    out += code;
    out += name;
    if (!code.empty()) {
        out += ansi_reset;
    }
    if (name.size() < level_width) {
        out.append(level_width - name.size(), ' ');
    }
}

void append_text_tag(std::string& out, std::string_view key, std::string_view value) {
    out += ' ';
    append_text_escaped(out, key);
    out += '=';
    append_text_escaped(out, value);
}

void append_text_tags(std::string& out, const TextStyle& style, const RecordTags& tags) {
    if (!style.shown_tags) {
        for_each_unique_tag(tags, [&](std::string_view key, std::string_view value) {
            append_text_tag(out, key, value);
        });
        return;
    }
    for (const std::string& key : *style.shown_tags) {
        if (tags.contains(key)) {
            append_text_tag(out, key, tags.find(key));
        }
    }
}

void append_text_tail(std::string& out, const TextStyle& style, const RecordTags& tags,
                      uint32_t thread_id) {
    append_text_tags(out, style, tags);
    if (style.show_thread) {
        append_format(out, " [t{}]", thread_id);
    }
}

// KeyValue format.

void append_logfmt_pair(std::string& out, std::string_view key, std::string_view value) {
    out += ' ';
    out += key;
    out += '=';
    append_logfmt_value(out, value);
}

void append_logfmt_head(std::string& out, uint64_t timestamp_ns, std::string_view kind) {
    out += "time=";
    iso8601_utc_format().append(out, timestamp_ns);
    out += " kind=";
    out += kind;
}

void append_logfmt_tail(std::string& out, const RecordTags& tags, uint32_t thread_id) {
    append_format(out, " thread={}", thread_id);
    for_each_unique_tag(tags, [&](std::string_view key, std::string_view value) {
        out += ' ';
        append_logfmt_value(out, std::string("tags.") += key);
        out += '=';
        append_logfmt_value(out, value);
    });
}

// Json format.

void append_json_head(std::string& out, std::string_view type, uint64_t timestamp_ns) {
    append_format(out, R"({{"type":"{}","ts":{})", type, timestamp_ns);
}

void append_json_key(std::string& out, std::string_view key) {
    out += ',';
    append_json_string(out, key);
    out += ':';
}

void append_json_tail(std::string& out, const RecordTags& tags, uint32_t thread_id) {
    append_format(out, R"(,"thread":{},"tags":{{)", thread_id);
    bool first = true;
    for_each_unique_tag(tags, [&](std::string_view key, std::string_view value) {
        if (!first) {
            out += ',';
        }
        first = false;
        append_json_string(out, key);
        out += ':';
        append_json_string(out, value);
    });
    out += "}}\n";
}

}  // namespace

TimeFormat iso8601_utc_format() {
    // A constant pattern of accepted conversions: parse() cannot fail on it.
    return *TimeFormat::parse("%Y-%m-%dT%H:%M:%S.%NZ", TimeZone::Utc);
}

std::string text_line(const LogRecord& record, const TextStyle& style) {
    std::string out;
    append_time_column(out, style, record.timestamp_ns);
    append_level_column(out, style, record.level);
    out += ' ';
    append_text_escaped(out, record.message);
    append_text_tail(out, style, record.tags, record.thread_id);
    if (style.show_source) {
        out += " (";
        append_text_escaped(out, record.source.file);
        append_format(out, ":{})", record.source.line);
    }
    out += '\n';
    return out;
}

std::string text_line(const MetricRecord& record, const TextStyle& style) {
    std::string out;
    append_time_column(out, style, record.timestamp_ns);
    out += "METRIC ";
    append_text_escaped(out, record.name);
    out += '=';
    append_double(out, record.value);
    append_text_tail(out, style, record.tags, record.thread_id);
    out += '\n';
    return out;
}

std::string text_line(const ProgressRecord& record, const TextStyle& style) {
    std::string out;
    append_time_column(out, style, record.timestamp_ns);
    out += "PROGRESS ";
    append_text_escaped(out, record.label);
    append_format(out, " {}/{}", record.current, record.total);
    append_text_tail(out, style, record.tags, record.thread_id);
    out += '\n';
    return out;
}

std::string key_value_line(const LogRecord& record) {
    std::string out;
    append_logfmt_head(out, record.timestamp_ns, "log");
    append_logfmt_pair(out, "level", level_name(record.level));
    append_logfmt_pair(out, "msg", record.message);
    append_logfmt_pair(out, "file", record.source.file);
    append_format(out, " line={}", record.source.line);
    if (!record.source.function.empty()) {
        append_logfmt_pair(out, "function", record.source.function);
    }
    if (!record.source.class_name.empty()) {
        append_logfmt_pair(out, "class", record.source.class_name);
    }
    append_logfmt_tail(out, record.tags, record.thread_id);
    out += '\n';
    return out;
}

std::string key_value_line(const MetricRecord& record) {
    std::string out;
    append_logfmt_head(out, record.timestamp_ns, "metric");
    append_logfmt_pair(out, "name", record.name);
    out += " value=";
    append_double(out, record.value);
    append_logfmt_tail(out, record.tags, record.thread_id);
    out += '\n';
    return out;
}

std::string key_value_line(const ProgressRecord& record) {
    std::string out;
    append_logfmt_head(out, record.timestamp_ns, "progress");
    append_format(out, " id={}", record.id);
    append_logfmt_pair(out, "label", record.label);
    append_format(out, " current={} total={}", record.current, record.total);
    append_logfmt_tail(out, record.tags, record.thread_id);
    out += '\n';
    return out;
}

std::string json_line(const LogRecord& record) {
    std::string out;
    append_json_head(out, "log", record.timestamp_ns);
    append_json_key(out, "level");
    append_json_string(out, level_name(record.level));
    append_json_key(out, "msg");
    append_json_string(out, record.message);
    append_json_key(out, "file");
    append_json_string(out, record.source.file);
    append_format(out, R"(,"line":{})", record.source.line);
    append_json_key(out, "function");
    append_json_string(out, record.source.function);
    append_json_key(out, "class");
    append_json_string(out, record.source.class_name);
    append_json_tail(out, record.tags, record.thread_id);
    return out;
}

std::string json_line(const MetricRecord& record) {
    std::string out;
    append_json_head(out, "metric", record.timestamp_ns);
    append_json_key(out, "name");
    append_json_string(out, record.name);
    if (has_json_number(record.value)) {
        out += R"(,"value":)";
        append_double(out, record.value);
    } else {
        out += R"(,"value":null,"value_text":")";
        append_double(out, record.value);
        out += '"';
    }
    append_json_tail(out, record.tags, record.thread_id);
    return out;
}

std::string json_line(const ProgressRecord& record) {
    std::string out;
    append_json_head(out, "progress", record.timestamp_ns);
    append_format(out, R"(,"id":{})", record.id);
    append_json_key(out, "label");
    append_json_string(out, record.label);
    append_format(out, R"(,"current":{},"total":{})", record.current, record.total);
    append_json_tail(out, record.tags, record.thread_id);
    return out;
}

}  // namespace lumen::detail
