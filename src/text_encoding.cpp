#include "text_encoding.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace lumen::detail {

namespace {

constexpr std::string_view hex_digits = "0123456789abcdef";
constexpr std::string_view replacement_escape = "\\ufffd";
constexpr uint32_t replacement_character = 0xFFFD;

[[nodiscard]] constexpr bool is_control(uint8_t byte) noexcept {
    return byte < 0x20 || byte == 0x7F;
}

void append_hex_byte(std::string& out, uint8_t byte) {
    out += hex_digits[byte >> 4U];
    out += hex_digits[byte & 0x0FU];
}

// The first byte of a UTF-8 sequence: how long the sequence is, the payload
// bits it carries and the smallest code point that needs this length (a
// smaller one would be an overlong form). length 0: not a lead byte.
struct LeadByte {
    std::size_t length = 0;
    uint32_t bits = 0;
    uint32_t min_code_point = 0;
};

[[nodiscard]] constexpr LeadByte decode_lead(uint8_t byte) noexcept {
    if ((byte & 0xE0U) == 0xC0U) {
        return {2, byte & 0x1FU, 0x80};
    }
    if ((byte & 0xF0U) == 0xE0U) {
        return {3, byte & 0x0FU, 0x800};
    }
    if ((byte & 0xF8U) == 0xF0U) {
        return {4, byte & 0x07U, 0x10000};
    }
    return {};
}

[[nodiscard]] constexpr bool is_scalar_value(uint32_t code_point) noexcept {
    return code_point <= 0x10FFFF && (code_point < 0xD800 || code_point > 0xDFFF);
}

// A valid multi-byte UTF-8 sequence: its length and the code point it
// encodes. length 0: there is none.
struct Utf8Sequence {
    std::size_t length = 0;
    uint32_t code_point = 0;
};

// The valid multi-byte UTF-8 sequence at text[pos], if there is one. Reads
// nothing past the end of `text`, which need not be NUL-terminated.
[[nodiscard]] Utf8Sequence decode_utf8(std::string_view text, std::size_t pos) noexcept {
    const LeadByte lead = decode_lead(static_cast<uint8_t>(text[pos]));
    if (lead.length == 0 || text.size() - pos < lead.length) {
        return {};
    }
    uint32_t code_point = lead.bits;
    for (std::size_t k = 1; k < lead.length; ++k) {
        const auto byte = static_cast<uint8_t>(text[pos + k]);
        if ((byte & 0xC0U) != 0x80U) {
            return {};
        }
        code_point = (code_point << 6U) | (byte & 0x3FU);
    }
    if (code_point < lead.min_code_point || !is_scalar_value(code_point)) {
        return {};
    }
    return {.length = lead.length, .code_point = code_point};
}

// Code points that a terminal may act on rather than show: the C1 controls
// (U+009B is CSI, like ESC '[') and the bidirectional overrides and isolates,
// which reorder the text shown after them.
[[nodiscard]] constexpr bool drives_terminal(uint32_t code_point) noexcept {
    return (code_point >= 0x80 && code_point <= 0x9F) ||
           (code_point >= 0x202A && code_point <= 0x202E) ||
           (code_point >= 0x2066 && code_point <= 0x2069);
}

void append_text_ascii(std::string& out, uint8_t byte) {
    switch (byte) {
        case '\n': out += "\\n"; return;
        case '\r': out += "\\r"; return;
        case '\t': out += "\\t"; return;
        default:   break;
    }
    if (is_control(byte)) {
        out += "\\x";
        append_hex_byte(out, byte);
        return;
    }
    out += static_cast<char>(byte);
}

void append_json_ascii(std::string& out, uint8_t byte) {
    switch (byte) {
        case '"':  out += "\\\""; return;
        case '\\': out += "\\\\"; return;
        case '\b': out += "\\b"; return;
        case '\f': out += "\\f"; return;
        case '\n': out += "\\n"; return;
        case '\r': out += "\\r"; return;
        case '\t': out += "\\t"; return;
        default:   break;
    }
    if (byte < 0x20) {
        out += "\\u00";
        append_hex_byte(out, byte);
        return;
    }
    out += static_cast<char>(byte);
}

[[nodiscard]] bool needs_logfmt_quotes(std::string_view text) noexcept {
    return text.empty() || std::ranges::any_of(text, [](char c) {
               return c == ' ' || c == '=' || c == '"' || c == '\\' ||
                      is_control(static_cast<uint8_t>(c));
           });
}

}  // namespace

std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "UNKNOWN";
}

void append_text_escaped(std::string& out, std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto byte = static_cast<uint8_t>(text[pos]);
        if (byte < 0x80) {
            append_text_ascii(out, byte);
            ++pos;
            continue;
        }
        const Utf8Sequence sequence = decode_utf8(text, pos);
        if (sequence.length == 0) {
            out += "\\x";
            append_hex_byte(out, byte);
            ++pos;
            continue;
        }
        if (drives_terminal(sequence.code_point)) {
            std::format_to(std::back_inserter(out), "\\u{:04x}", sequence.code_point);
        } else {
            out += text.substr(pos, sequence.length);
        }
        pos += sequence.length;
    }
}

std::string text_escaped(std::string_view text) {
    std::string out;
    append_text_escaped(out, text);
    return out;
}

void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto byte = static_cast<uint8_t>(text[pos]);
        if (byte < 0x80) {
            append_json_ascii(out, byte);
            ++pos;
            continue;
        }
        const Utf8Sequence sequence = decode_utf8(text, pos);
        if (sequence.length == 0) {
            out += replacement_escape;
            ++pos;
            continue;
        }
        if (sequence.code_point == replacement_character) {
            // Written like a replaced byte, so that two strings that read
            // back the same are always written alike.
            out += replacement_escape;
        } else {
            out += text.substr(pos, sequence.length);
        }
        pos += sequence.length;
    }
    out += '"';
}

void append_logfmt_value(std::string& out, std::string_view text) {
    if (needs_logfmt_quotes(text)) {
        append_json_string(out, text);
    } else {
        out += text;
    }
}

void append_json_number(std::string& out, double value) {
    // "-0" is a JSON number, but a parser may read it as the integer 0 and
    // lose the sign; "-0.0" is read as the double -0.0.
    if (value == 0.0 && std::signbit(value)) {
        out += "-0.0";
        return;
    }
    append_double(out, value);
}

bool has_json_number(double value) noexcept {
    return std::isfinite(value);
}

void append_double(std::string& out, double value) {
    if (std::isnan(value)) {
        out += "NaN";
        return;
    }
    if (std::isinf(value)) {
        out += value > 0 ? "Infinity" : "-Infinity";
        return;
    }
    // The shortest round-trip form of a double has at most 24 characters
    // ("-2.2250738585072014e-308"), so this buffer cannot be too small.
    std::array<char, 32> buffer{};
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        out += "NaN";  // unreachable: see the buffer size above
        return;
    }
    out.append(buffer.data(), result.ptr);
}

}  // namespace lumen::detail
