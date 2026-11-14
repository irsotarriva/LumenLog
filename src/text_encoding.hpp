#ifndef LUMEN_SRC_TEXT_ENCODING_HPP
#define LUMEN_SRC_TEXT_ENCODING_HPP

#include <string>
#include <string_view>

#include "lumen/level.hpp"

// The pieces every line format is built from. Each function appends to `out`;
// the format rules they implement are documented with FileFormat
// (lumen/file_sink.hpp) and TerminalSink (lumen/terminal_sink.hpp).

namespace lumen::detail {

// "TRACE" ... "FATAL"; "UNKNOWN" for a value outside the enum.
[[nodiscard]] std::string_view level_name(LogLevel level) noexcept;

// Text formats: what could break the one-record-per-line layout or drive a
// terminal is escaped, everything else is appended as it is:
//   - bytes below 0x20 and 0x7F as \n, \r, \t or \xHH;
//   - each byte that is not part of a valid UTF-8 sequence as \xHH;
//   - the C1 controls U+0080-U+009F and the bidirectional overrides and
//     isolates U+202A-U+202E, U+2066-U+2069 as \uHHHH.
// Hex digits are lower case. '\' itself is not escaped.
void append_text_escaped(std::string& out, std::string_view text);
[[nodiscard]] std::string text_escaped(std::string_view text);

// A JSON string literal, quotes included. Each byte that is not part of a
// valid UTF-8 sequence becomes U+FFFD, written as the escape \ufffd; a U+FFFD
// in the text is written the same way. Every other character has exactly one
// written form, so two strings are written alike exactly when they read back
// alike.
void append_json_string(std::string& out, std::string_view text);

// logfmt: `text` as it is, or as a JSON string literal when it is empty or
// contains a space, '=', '"', '\', a byte below 0x20 or 0x7F.
void append_logfmt_value(std::string& out, std::string_view text);

// The shortest decimal that reads back as `value`; for a non-finite value
// "NaN", "Infinity" or "-Infinity".
void append_double(std::string& out, double value);

// A finite `value` as a JSON number: append_double's text, except that -0.0
// is written "-0.0" so that the sign survives a parser that reads "-0" as an
// integer.
void append_json_number(std::string& out, double value);

// Whether `value` has a JSON number (it is finite).
[[nodiscard]] bool has_json_number(double value) noexcept;

}  // namespace lumen::detail

#endif
