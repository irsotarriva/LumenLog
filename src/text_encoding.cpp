#include "text_encoding.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace lumen::detail {

namespace {

constexpr std::string_view hex_digits = "0123456789abcdef";
constexpr std::string_view replacement_escape = "\\ufffd";

[[nodiscard]] constexpr bool is_control(unsigned char byte) noexcept {
    return byte < 0x20 || byte == 0x7F;
}

void append_hex_byte(std::string& out, unsigned char byte) {
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

[[nodiscard]] constexpr LeadByte decode_lead(unsigned char byte) noexcept {
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

// Length of the valid multi-byte UTF-8 sequence at text[pos], or 0 if there
// is none there.
[[nodiscard]] std::size_t utf8_sequence_length(std::string_view text, std::size_t pos) noexcept {
    const LeadByte lead = decode_lead(static_cast<unsigned char>(text[pos]));
    if (lead.length == 0 || text.size() - pos < lead.length) {
        return 0;
    }
    uint32_t code_point = lead.bits;
    for (std::size_t k = 1; k < lead.length; ++k) {
        const auto byte = static_cast<unsigned char>(text[pos + k]);
        if ((byte & 0xC0U) != 0x80U) {
            return 0;
        }
        code_point = (code_point << 6U) | (byte & 0x3FU);
    }
    if (code_point < lead.min_code_point || !is_scalar_value(code_point)) {
        return 0;
    }
    return lead.length;
}

void append_json_ascii(std::string& out, unsigned char byte) {
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
                      is_control(static_cast<unsigned char>(c));
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
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (!is_control(byte)) {
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += "\\x";
            append_hex_byte(out, byte);
        }
    }
}

void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto byte = static_cast<unsigned char>(text[pos]);
        if (byte < 0x80) {
            append_json_ascii(out, byte);
            ++pos;
            continue;
        }
        const std::size_t length = utf8_sequence_length(text, pos);
        if (length == 0) {
            out += replacement_escape;
            ++pos;
            continue;
        }
        out += text.substr(pos, length);
        pos += length;
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
