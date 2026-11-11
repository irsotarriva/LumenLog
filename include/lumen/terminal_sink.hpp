#ifndef LUMEN_TERMINAL_SINK_HPP
#define LUMEN_TERMINAL_SINK_HPP

#include <cstddef>
#include <expected>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <vector>

#include "lumen/diagnostics.hpp"
#include "lumen/sink.hpp"
#include "lumen/terminal_style.hpp"
#include "lumen/writer_statistics.hpp"

namespace lumen {

// Writes one human-readable line per record to standard error, or to any
// std::ostream you supply:
//
//   12:04:05.123 INFO  connected host=db1
//   12:04:05.124 METRIC latency_ms=3.25
//   12:04:05.125 PROGRESS files 3/10
//
// Layout, fields separated by one space:
//   <time>      Config::time_format applied to the record's timestamp; left
//               out (with its space) when the format is empty.
//   <kind>      a log record's level, padded to five characters (TRACE,
//               DEBUG, INFO, WARN, ERROR, FATAL; UNKNOWN for a value outside
//               the enum); METRIC; or PROGRESS.
//   <payload>   log: the message. metric: name=value. progress: label
//               current/total.
//   <tags>      key=value for each key in Config::inline_tags that the record
//               has, in that order.
//   <source>    (file:line), logs only, when Config::show_source is set.
// Each line ends with '\n'. With colour on, only the level name is coloured
// (ESC[<colour>m before it, ESC[0m after it; the padding is outside).
//
// A record is always one line and cannot drive the terminal. Text (the
// message, names, labels, tags, the source file and the literal text of
// time_format) is written as received except for what could break the
// one-record-per-line layout or be acted on by a terminal:
//   - bytes below 0x20 and 0x7F as \n, \r, \t or \xHH;
//   - each byte that is not part of a valid UTF-8 sequence as \xHH;
//   - the C1 controls U+0080-U+009F (U+009B is CSI) and the bidirectional
//     overrides and isolates U+202A-U+202E and U+2066-U+2069 as \uHHHH.
// Hex digits are lower case; '\' itself is not escaped, so an escape and the
// same characters typed literally look alike. Metric values are written in
// the shortest form that reads back as the same double (std::to_chars); NaN
// and infinities as NaN, Infinity and -Infinity.
//
// Threads. on_* formats the line on the calling thread and queues it; a
// thread owned by the sink writes it (see lumen/writer_statistics.hpp for the
// queue bound and what happens when it is full). on_* never waits for the
// stream or for the diagnostic handler. flush() returns when every line
// queued before it has been written and the stream flushed; after a Fatal
// record it waits at most fatal_flush_timeout. The destructor writes what is
// still queued, then stops the thread; it waits for the stream as long as the
// stream blocks.
class TerminalSink final : public Sink {
public:
    struct Config {
        // Where lines go. Null means the process's standard error (std::cerr).
        // The writer thread uses the stream until the sink is destroyed;
        // anything else that writes to it meanwhile needs its own
        // synchronisation with that thread.
        std::shared_ptr<std::ostream> output;
        ColorMode color = ColorMode::Auto;
        LevelColors colors = default_level_colors;
        // A std::strftime pattern with two additions: %L is the millisecond
        // (000-999) and %N the nanosecond (000000000-999999999) within the
        // second. Only the conversions listed in std::strftime that need no
        // modifier are accepted (no %E or %O forms), except %n and %t, which
        // would put a newline or a tab into the line. The pattern may not
        // contain a NUL character; its length is not limited. Literal text in
        // it is escaped like a message. Empty: no time column.
        std::string time_format = "%H:%M:%S.%L";
        TimeZone time_zone = TimeZone::Local;
        // Tags shown on a line, in this order. Tags not listed are not shown.
        std::vector<std::string> inline_tags;
        bool show_source = false;
        // Lines that may wait for the writer thread; at least 1.
        std::size_t max_queued_lines = 10000;
        // Receives the first queue-full drop and the first write failure
        // (one line each); called on the sink's writer thread only, never
        // concurrently with itself. Empty: discard.
        DiagnosticHandler diagnostics = stderr_diagnostics();
    };

    // Fails with LumenError::InvalidSinkConfig if time_format is not a valid
    // pattern or max_queued_lines is 0, and with the error std::thread
    // reports, as a std::generic_category code on every standard library
    // (std::errc::resource_unavailable_try_again when the system is out of
    // threads), if the writer thread cannot be started.
    [[nodiscard]] static std::expected<std::unique_ptr<TerminalSink>, std::error_code> create(
        Config config);

    // Only create() can make the key, so only create() can construct a sink.
    class ConstructionKey {
        friend class TerminalSink;
        ConstructionKey() = default;
    };
    struct Impl;
    TerminalSink(ConstructionKey key, std::unique_ptr<Impl> impl);
    ~TerminalSink() override;
    TerminalSink(const TerminalSink&) = delete;
    TerminalSink& operator=(const TerminalSink&) = delete;
    TerminalSink(TerminalSink&&) = delete;
    TerminalSink& operator=(TerminalSink&&) = delete;

    void on_log(const LogRecord& record) override;
    void on_metric(const MetricRecord& record) override;
    void on_progress(const ProgressRecord& record) override;
    void flush() override;

    // Whether this sink writes colour escapes (ColorMode resolved at creation).
    [[nodiscard]] bool colored() const noexcept;
    // Safe to call from any thread.
    [[nodiscard]] WriterStatistics statistics() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace lumen

#endif
