#ifndef LUMEN_FILE_SINK_HPP
#define LUMEN_FILE_SINK_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <system_error>

#include "lumen/diagnostics.hpp"
#include "lumen/sink.hpp"
#include "lumen/writer_statistics.hpp"

namespace lumen {

// The line format of a FileSink. Every format writes one line per record,
// ending in '\n', and every line of every format carries the same facts: time,
// record kind, the kind's fields, the producing thread and all tags. The
// three record kinds are laid out alike within a format.
enum class FileFormat : uint8_t {
    // For people. Timestamps are UTC, ISO 8601 with nanoseconds.
    //   2026-11-11T10:00:00.123456789Z INFO  connected host=db1 [t3] (main.cpp:42)
    //   2026-11-11T10:00:00.123456789Z METRIC loss=0.25 epoch=3 [t3]
    //   2026-11-11T10:00:00.123456789Z PROGRESS files 3/10 [t3]
    // Fields as in TerminalSink (lumen/terminal_sink.hpp), with every tag in
    // record order, then [t<thread id>], then the source for logs. Text is
    // escaped as on the terminal (control bytes, bytes that are not UTF-8,
    // C1 controls and bidirectional overrides; see TerminalSink); nothing
    // else is, so a tag value containing a space or '=' is ambiguous. Use
    // KeyValue or Json for files a program reads.
    Text,
    // logfmt: space-separated key=value pairs, fixed keys first.
    //   time=2026-11-11T10:00:00.123456789Z kind=log level=INFO msg=connected
    //     file=main.cpp line=42 function=main thread=3 tags.host=db1
    //   time=... kind=metric name=loss value=0.25 thread=3 tags.epoch=3
    //   time=... kind=progress id=7 label=files current=3 total=10 thread=3
    // (one line each; wrapped here). `function` and `class` are left out when
    // empty. Tags follow as tags.<key>=<value>, so a tag can never collide
    // with a fixed key. A key or value that is empty or contains a space,
    // '=', '"', '\', a byte below 0x20 or 0x7F is written as a JSON string
    // literal (quoted and escaped exactly as in Json); everything else is
    // written as it is.
    KeyValue,
    // One JSON object per line (NDJSON):
    //   {"type":"log","ts":1762855200123456789,"level":"INFO","msg":"connected",
    //    "file":"main.cpp","line":42,"function":"main","class":"","thread":3,
    //    "tags":{"host":"db1"}}
    //   {"type":"metric","ts":...,"name":"loss","value":0.25,"thread":3,"tags":{}}
    //   {"type":"progress","ts":...,"id":7,"label":"files","current":3,
    //    "total":10,"thread":3,"tags":{}}
    // (one line each; wrapped here.)
    //   ts      integer nanoseconds since the Unix epoch (system_clock, UTC).
    //   level   the plain level name; "UNKNOWN" for a value outside the enum.
    //   value   the shortest decimal that reads back as the same double
    //           (std::to_chars); -0.0 is written "-0.0", so that a parser
    //           that reads "-0" as an integer keeps the sign. NaN and
    //           infinities have no JSON number: for
    //           them "value" is null and "value_text" is "NaN", "Infinity"
    //           or "-Infinity". value_text is present only then.
    //   tags    an object nested under "tags", so tag keys never collide with
    //           the record's own keys. Every tag value is a JSON string, as
    //           in the record: no type is guessed (the tag 3 is "3"). Two
    //           different keys that are written alike (each invalid byte
    //           becomes U+FFFD, so "\xff" and "\xfe" both read "\ufffd")
    //           would make a duplicate key: the later one gets the suffix
    //           "#2", or "#3" and so on, the first that is not taken
    //           ("\ufffd#2"). KeyValue does the same for its keys.
    // Strings: '"' and '\' are escaped, \b \f \n \r \t use their short
    // escapes and every other byte below 0x20 is written as \u00XX. Valid
    // UTF-8 is written as it is; each byte that is not part of a valid UTF-8
    // sequence (including overlong forms and surrogates) is replaced by
    // U+FFFD, written as the escape \ufffd (as is a U+FFFD in the record), so
    // every line is valid JSON whatever the record holds.
    Json,
};

// Appends records to a file, one line per record (see FileFormat), with
// optional size-based rotation.
//
// Threads. As TerminalSink: on_* formats on the calling thread and queues the
// line; the sink's writer thread writes it and reports diagnostics
// (lumen/writer_statistics.hpp). flush() returns when every line queued
// before it is written and the stream flushed to the operating system (no
// fsync); after a Fatal record it waits at most fatal_flush_timeout.
//
// Rotation. With max_file_bytes > 0, a line that would take the active file
// past max_file_bytes starts a new file first (unless the active file is
// empty: a line longer than the limit is written whole). The active file is
// renamed to <path>.1, an existing <path>.1 to <path>.2 and so on, and the
// oldest, <path>.<max_files - 1>, is deleted; so at most max_files files
// exist: the active one and max_files - 1 old ones. With max_files == 1 the
// active file is deleted and started again. The size of a file that already
// exists when the sink is created counts towards the limit, since the sink
// appends to it. A rotation step that fails is reported as a write failure
// (no line is lost by it) and writing continues in the file that is open.
// If the active file could not be moved away, the next attempt is made only
// once it has grown by another max_file_bytes: a file whose rotation keeps
// failing grows without limit, with one rename attempt per max_file_bytes
// written, and only the first failure is reported. A path that is not a
// regular file when the sink is created (/dev/null, a named pipe) is never
// rotated.
//
// Named pipes. A FIFO is opened only if a process has it open for reading
// when create() is called; otherwise create() fails at once with
// LumenError::FileOpenFailed instead of waiting for a reader (POSIX; on
// Windows a path is opened as it is). Once open, writes wait while the pipe
// is full; a reader that goes away makes the next write fail (with SIGPIPE
// ignored) or ends the process (SIGPIPE's default action), as for any pipe.
class FileSink final : public Sink {
public:
    struct Config {
        // "{date}" anywhere in the path is replaced, once at creation, by the
        // current UTC date (system_clock) as YYYY-MM-DD. No other placeholder
        // exists. The directory must exist.
        std::filesystem::path path = "run_{date}.log";
        FileFormat format = FileFormat::Text;
        // 0: never rotate.
        uint64_t max_file_bytes = uint64_t{256} * 1024 * 1024;
        // Files kept, the active one included; at least 1.
        uint32_t max_files = 5;
        // Lines that may wait for the writer thread; at least 1.
        std::size_t max_queued_lines = 65536;
        // As TerminalSink::Config::diagnostics; also receives the cause of a
        // failed open, on the thread that calls create().
        DiagnosticHandler diagnostics = stderr_diagnostics();
    };

    // Opens (creates or appends to) the file. Fails with
    // LumenError::InvalidSinkConfig if max_files or max_queued_lines is 0 or
    // the path is empty; with LumenError::FileOpenFailed if the file cannot
    // be opened for appending, after passing one line with the cause, as the
    // platform words it, to config.diagnostics on the calling thread
    // ("lumen: FileSink: cannot open <path>: No such file or directory");
    // and with the error std::thread reports
    // (std::errc::resource_unavailable_try_again when the system is out of
    // threads) if the writer thread cannot be started.
    [[nodiscard]] static std::expected<std::unique_ptr<FileSink>, std::error_code> create(
        Config config);

    // Only create() can make the key, so only create() can construct a sink.
    class ConstructionKey {
        friend class FileSink;
        ConstructionKey() = default;
    };
    struct Impl;
    FileSink(ConstructionKey key, std::unique_ptr<Impl> impl);
    ~FileSink() override;
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;
    FileSink(FileSink&&) = delete;
    FileSink& operator=(FileSink&&) = delete;

    void on_log(const LogRecord& record) override;
    void on_metric(const MetricRecord& record) override;
    void on_progress(const ProgressRecord& record) override;
    void flush() override;

    // The active file's path, with {date} substituted.
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    // Safe to call from any thread.
    [[nodiscard]] WriterStatistics statistics() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace lumen

#endif
