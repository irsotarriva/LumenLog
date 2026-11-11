# Sinks

A sink receives the records that its predicate accepts. Lumen ships four:
`TerminalSink` (`lumen/terminal_sink.hpp`), `FileSink`
(`lumen/file_sink.hpp`), `DashboardSink` (`lumen/dashboard_sink.hpp`) and
`NullSink` (`lumen/sink.hpp`). You can write your own.

## Registering and removing

<!-- check: prelude=common wrap=body -->
```cpp
auto terminal = lumen::TerminalSink::create({});
if (!terminal) {
    std::cerr << "terminal sink: " << terminal.error().message() << '\n';
    return;
}
const auto id = lumen::core().add_sink(std::move(*terminal),
                                       lumen::level_at_least(lumen::LogLevel::Info));
if (!id) {
    std::cerr << "add_sink: " << id.error().message() << '\n';
    return;
}
// ... later
if (auto removed = lumen::core().remove_sink(*id); !removed) {
    std::cerr << "remove_sink: " << removed.error().message() << '\n';
}
```

- `add_sink` takes ownership. The sink receives every record dispatched after
  the call returns that its predicate accepts. It fails with
  `LumenError::NullSink` for a null pointer.
- `remove_sink` gives the sink back. When it returns, the sink is not inside
  a callback and is never called again; records still queued are not
  delivered to it. If one of its callbacks is running on another thread,
  `remove_sink` waits for it. Called from inside a callback of the same Core
  it never waits, and removing the calling sink itself fails with
  `LumenError::SinkBusy`. An unknown id fails with `LumenError::InvalidSinkId`.
- `add_sink`, `remove_sink`, `sink_exception_count` and `statistics` may be
  called from any thread, also from inside a sink callback.
- Sinks still registered when the Core shuts down are flushed and destroyed
  then.

## The contract

`lumen::Sink` has four virtual functions: `on_log`, `on_metric` and
`on_progress`, which do nothing unless overridden, and `flush`, which every
sink defines.

- **One thread at a time.** The Core calls a sink's functions on one thread
  at a time and never concurrently with another sink of the same Core:
  usually the Core's dispatch thread, but `Core::flush()` delivers on the
  thread that calls it.
- **`on_*` must not block.** The dispatch thread serves every sink. A
  callback that waits for a disk, a terminal or a lock held by a slow thread
  delays every other sink and, once the queues are full, makes the Core drop
  records. A sink that does I/O hands the work to a thread of its own. The
  shipped sinks do, and they report their diagnostics from that thread too:
  a diagnostic handler that writes to a blocked standard error does not
  block the dispatch thread.
- **`flush()` may wait.** It returns when what the sink received before the
  call has reached its destination. After a fatal record the Core flushes
  and terminates; the shipped sinks then wait at most a bounded time
  (`lumen::fatal_flush_timeout`, 2 s, for `TerminalSink` and `FileSink`;
  about one second for `DashboardSink`).
- **Records own their strings.** Every `std::string_view` in a record points
  into the record's own storage, shared by its copies. A sink may copy a
  record and keep the copy; the views stay valid while a copy lives.
- **Exceptions.** A sink should not throw. An exception that escapes a
  callback is caught by the Core, counted (`Core::sink_exception_count(id)`),
  and the first one per sink is reported through the diagnostic handler. The
  record is lost for that sink only.

## The writer queue

`TerminalSink` and `FileSink` format each record into one line on the calling
thread and append it to a bounded queue; a writer thread owned by the sink
writes batches of lines and flushes the stream (`lumen/writer_statistics.hpp`).

- `on_*` never waits for the stream or for the diagnostic handler. When the
  queue holds `max_queued_lines` lines, the new line is dropped and counted
  in `lines_dropped`. The first drop is reported once through the sink's
  `Config::diagnostics`, by the writer thread as soon as it is free.
- The diagnostic handler is called on the writer thread only, never
  concurrently with itself.
- A write that fails (a closed pipe, a full disk) is not retried: its lines
  are counted in `lines_failed`, and the first failure is reported once.
  Writing continues with the next lines.
- `statistics()` returns a `WriterStatistics` with `lines_queued`,
  `lines_written`, `lines_dropped` and `lines_failed`; it may be called from
  any thread. After the sink's `flush()` returns, and while nothing else is
  delivered to it, `lines_queued == lines_written + lines_failed`.
- `flush()` waits as long as the stream blocks, except after the sink
  received a fatal record: then it gives up after `lumen::fatal_flush_timeout`
  (2 s), and the lines not written by then are lost with the process. With
  standard error on a pipe nobody reads, the fatal path still ends the
  process: unless the application installed a terminate handler of its own,
  Lumen calls `std::abort()` rather than the C++ library's default terminate
  handler, whose message to standard error would block.
- The destructor writes what is still queued, then stops the thread. It
  waits for the stream as long as the stream blocks.
- `create` fails with the error `std::thread` reports
  (`std::errc::resource_unavailable_try_again` when the system is out of
  threads) if the writer thread cannot be started; it does not throw.

## TerminalSink

One human-readable line per record, on standard error or on any
`std::ostream`. The quick start prints:

<!-- check: output=examples/expected/quick_start.stderr -->
```text
<time> INFO  starting app=quick_start run=42
<time> WARN  energy 41.5 GeV above threshold app=quick_start detector=calorimeter run=42
<time> DEBUG 3 channels masked app=quick_start detector=tracker run=42
<time> ERROR explicit tags win app=quick_start run=43
<time> METRIC loss=0.25 app=quick_start run=42
<time> PROGRESS events 1/3 app=quick_start run=42
<time> PROGRESS events 2/3 app=quick_start run=42
<time> PROGRESS events 3/3 app=quick_start run=42
```

(`<time>` is the local time as `HH:MM:SS.mmm`, the default `time_format`.)
Fields, separated by one space:

| Field | Content |
|---|---|
| time | `Config::time_format` applied to the record's timestamp; left out, with its space, when the format is empty |
| kind | a log record's level padded to five characters (`TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`, or `UNKNOWN` for a value outside the enum); `METRIC`; `PROGRESS` |
| payload | log: the message; metric: `name=value`; progress: `label current/total` |
| tags | `key=value` for each key of `Config::inline_tags` that the record has, in that order; other tags are not shown |
| source | `(file:line)`, log records only, when `Config::show_source` is set |

A record is always one line and cannot drive the terminal. In the message,
names, labels, tags, the source file and the literal text of `time_format`:

- bytes below 0x20 and the byte 0x7F are written as `\n`, `\r`, `\t` or
  `\xHH`;
- each byte that is not part of a valid UTF-8 sequence is written as `\xHH`;
- the C1 controls U+0080 to U+009F (U+009B is the one-character form of
  `ESC [`) and the bidirectional overrides and isolates U+202A to U+202E and
  U+2066 to U+2069 are written as `\uHHHH`.

Everything else, valid UTF-8 included, is written as it is; `\` itself is
not escaped. Metric values are written in the shortest form that reads back
as the same `double`; NaN and infinities as `NaN`, `Infinity` and
`-Infinity`.

`TerminalSink::Config`:

| Field | Default | Meaning |
|---|---|---|
| `output` | null: standard error | The stream. The writer thread uses it until the sink is destroyed. |
| `color` | `ColorMode::Auto` | `Auto`, `Always` or `Never` |
| `colors` | `default_level_colors` | One `AnsiColor` per level: Trace default, Debug cyan, Info green, Warn yellow, Error red, Fatal magenta |
| `time_format` | `"%H:%M:%S.%L"` | A `std::strftime` pattern plus `%L` (milliseconds, 000–999) and `%N` (nanoseconds, 000000000–999999999). Only conversions without a modifier (no `%E`, `%O` forms), and not `%n` or `%t` (a newline, a tab). No NUL character; any length. Empty: no time column. |
| `time_zone` | `TimeZone::Local` | `Local` (the process's time zone, `TZ` on POSIX) or `Utc` |
| `inline_tags` | empty | Tags shown on a line, in this order |
| `show_source` | `false` | Append `(file:line)` to log lines |
| `max_queued_lines` | `10000` | Writer queue bound, at least 1 |
| `diagnostics` | `stderr_diagnostics()` | Receives the first queue-full drop and the first write failure, on the writer thread |

`create` fails with `LumenError::InvalidSinkConfig` for an invalid
`time_format` or `max_queued_lines == 0`, and with the `std::thread` error
when the writer thread cannot start.

**Colour.** With `ColorMode::Auto` the sink writes colour escapes only when
all of these hold, decided once at creation: the output is the process's
standard error, standard error is a terminal, `NO_COLOR` is unset or empty,
`TERM` is not `dumb`, and the platform is not Windows. Only the level name is
coloured: `ESC[<colour>m` before it, `ESC[0m` after it, the padding outside.
`colored()` tells what was decided.

To write to a stream of your own (ctest runs this program):

<!-- check: prelude=common -->
```cpp
int main() {
    auto stream = std::make_shared<std::ostringstream>();
    lumen::TerminalSink::Config config;
    config.output = stream;
    config.time_format = "%Y-%m-%dT%H:%M:%S.%NZ";
    config.time_zone = lumen::TimeZone::Utc;
    config.show_source = true;
    auto sink = lumen::TerminalSink::create(std::move(config));
    if (!sink) {
        std::cerr << "terminal sink: " << sink.error().message() << '\n';
        return 1;
    }
    lumen::Core core;
    if (const auto id = core.add_sink(std::move(*sink)); !id) {
        return 1;
    }
    lumen::RecordBuilder(core, lumen::LogLevel::Info, std::source_location::current(), "hello");
    core.flush();
    // The line is "<date>T<time>.<nanoseconds>Z INFO  hello (<file>:<line>)".
    return stream->str().find("Z INFO  hello (") != std::string::npos ? 0 : 1;
}
```

## FileSink

Appends one line per record to a file, in one of three formats, with
optional rotation by size. The three files of `examples/routing.cpp`, run by
ctest as `Example.Routing`, show them. In these samples `<time>` is an ISO 8601
UTC time with nanoseconds, `<id>` the thread number and `<src>` the source
directory.

`FileFormat::Text`, for people (`routing.log`):

<!-- check: output=examples/expected/routing.log -->
```text
<time> INFO  tracker aligned detector=tracker [t<id>] (<src>/examples/routing.cpp:140)
<time> WARN  channel noisy detector=tracker channel=17 [t<id>] (<src>/examples/routing.cpp:141)
<time> METRIC hits=1024 detector=tracker [t<id>]
```

The fields are those of `TerminalSink`, with every tag in record order, then
`[t<thread>]`, then the source for log records. Text is escaped as on the
terminal and nothing else is, so a tag value containing a space or `=` is
ambiguous: use `KeyValue` or `Json` for files a program reads.

`FileFormat::KeyValue`, logfmt (`routing.logfmt`):

<!-- check: output=examples/expected/routing.logfmt -->
```text
time=<time> kind=log level=INFO msg="tracker aligned" file=<src>/examples/routing.cpp line=140 function="int main(int, char**)" thread=<id> tags.detector=tracker
time=<time> kind=log level=WARN msg="channel noisy" file=<src>/examples/routing.cpp line=141 function="int main(int, char**)" thread=<id> tags.detector=tracker tags.channel=17
time=<time> kind=metric name=hits value=1024 thread=<id> tags.detector=tracker
```

Space-separated `key=value` pairs, fixed keys first; `function` and `class`
are left out when empty; tags follow as `tags.<key>=<value>`, so a tag never
collides with a fixed key. A progress record has `kind=progress id label
current total thread`. A key or value that is empty or contains a space, `=`,
`"`, `\`, a byte below 0x20 or 0x7F is written as a JSON string literal.

`FileFormat::Json`, one JSON object per line (`routing.jsonl`):

<!-- check: output=examples/expected/routing.jsonl -->
```json
{"type":"log","ts":<ns>,"level":"INFO","msg":"channel hot","file":"<src>/examples/routing.cpp","line":142,"function":"int main(int, char**)","class":"","thread":<id>,"tags":{"detector":"calorimeter","temperature":"312.5"}}
{"type":"log","ts":<ns>,"level":"ERROR","msg":"readout lost","file":"<src>/examples/routing.cpp","line":143,"function":"int main(int, char**)","class":"","thread":<id>,"tags":{"detector":"calorimeter"}}
```

| Key | Records | Value |
|---|---|---|
| `type` | all | `"log"`, `"metric"` or `"progress"` |
| `ts` | all | integer, nanoseconds since the Unix epoch (`system_clock`) |
| `level` | log | `"TRACE"` … `"FATAL"`, `"UNKNOWN"` for a value outside the enum |
| `msg`, `file`, `line`, `function`, `class` | log | the message and the source location; `class` is `""` for a free function |
| `name`, `value` | metric | `value` is the shortest decimal that reads back as the same `double`, and `-0.0` for negative zero (a parser that reads `-0` as an integer would lose the sign); for NaN and infinities it is `null` and `"value_text"` is `"NaN"`, `"Infinity"` or `"-Infinity"` (present only then) |
| `id`, `label`, `current`, `total` | progress | integers and the label |
| `thread` | all | the producing thread's number |
| `tags` | all | an object of strings; no type is guessed (the tag `3` is `"3"`); never a key twice (below) |

Strings escape `"` and `\`, use `\b \f \n \r \t`, and write every other byte
below 0x20 as `\u00XX`. Valid UTF-8 is written as it is; each byte that is not
part of a valid UTF-8 sequence is replaced by `�`, written as the escape
`\ufffd` (as is a `�` in the record). Two different tag keys can then
be written alike (`"\xff"` and `"\xfe"` both become `"\ufffd"`): the later one
gets the suffix `#2`, or `#3` and so on, the first that no earlier key on the
line has (`"\ufffd#2"`), so an object never holds a key twice. `KeyValue`
does the same for its keys. Every line is valid JSON whatever the record
holds; `JsonOutput.EveryLineParsesStrictlyWithPythonAndHoldsTheExpectedValues`
parses the output with Python's `json` module.

`FileSink::Config`:

| Field | Default | Meaning |
|---|---|---|
| `path` | `"run_{date}.log"` | Every `{date}` is replaced, once at creation, by the UTC date as `YYYY-MM-DD`. No other placeholder exists. The directory must exist. |
| `format` | `FileFormat::Text` | `Text`, `KeyValue` or `Json` |
| `max_file_bytes` | 256 MiB | Rotate before a line would take the file past this size; 0: never rotate |
| `max_files` | `5` | Files kept, the active one included; at least 1 |
| `max_queued_lines` | `65536` | Writer queue bound, at least 1 |
| `diagnostics` | `stderr_diagnostics()` | As for `TerminalSink` |

`create` opens the file for appending. It fails with
`LumenError::InvalidSinkConfig` for an empty path or a zero `max_files` or
`max_queued_lines`; with `LumenError::FileOpenFailed` when the file cannot be
opened, after passing one line with the cause, as the platform words it, to
`Config::diagnostics` on the calling thread (`lumen: FileSink: cannot open
<path>: No such file or directory`); and with the `std::thread` error when
the writer thread cannot start. `path()` returns the path with `{date}`
substituted.

**Named pipes.** On POSIX a FIFO is opened only if a process has it open for
reading when `create` is called; otherwise `create` fails at once with
`LumenError::FileOpenFailed` ("a named pipe without a reader") instead of
waiting for one. Once open, a full pipe blocks the writer thread only. A
reader that goes away makes later writes fail, or ends the process through
`SIGPIPE` unless the application ignores that signal, as for any pipe.

**Rotation.** With `max_file_bytes > 0`, a line that would take the active
file past the limit first starts a new file, unless the active file is empty
(a line longer than the limit is written whole). The active file is renamed
to `<path>.1`, an existing `<path>.1` to `<path>.2`, and so on; the oldest,
`<path>.<max_files - 1>`, is deleted. So at most `max_files` files exist. With
`max_files == 1` the active file is deleted and started again. A file that
exists when the sink is created counts towards the limit. A failed rotation
step is reported as a write failure (no line is lost by it), and writing
continues in the open file. If the active file could not be moved away, the
next attempt is made once it has grown by another `max_file_bytes`: a file
whose rotation keeps failing grows without limit, with one attempt per
`max_file_bytes` written and only the first failure reported. A path that is
not a regular file at creation (`/dev/null`, a named pipe) is never rotated.

## DashboardSink

A full-screen live view on a terminal: the chosen metrics (latest value,
range and a sparkline of the recent history) and the progress bars in
flight, above the most recent log lines in their level's colour.

- It exists only in a library built with `LUMEN_ENABLE_DASHBOARD=ON`, which
  needs FTXUI. Without it, `lumen/dashboard_sink.hpp` still declares the
  class, but a program that uses it does not link. FTXUI is not available on
  every machine that builds Lumen; the CI jobs that use vcpkg compile it,
  except the Windows job, which turns the dashboard off.
- The destination must be a terminal: `create` fails with
  `LumenError::NotATerminal` otherwise, unless `Config::assume_terminal` is
  set. It also fails with `LumenError::InvalidSinkConfig` for an invalid
  `time_format` or a `refresh_interval` below 1 ms (above 24 h is taken as
  24 h), and with the `std::thread` error, the terminal left as it was, when
  the drawing thread cannot start.
- `create` switches the terminal to its alternate screen and hides the
  cursor; the destructor restores both, and what was on the screen before
  comes back. While the sink exists nothing else may write to that terminal.
- Without `width` and `height`, each frame measures the terminal on standard
  error, the stream the dashboard draws to (not standard output); a caller's
  stream cannot be measured and gets 80 x 24.
- `on_*` only updates the model under a mutex; a thread owned by the sink
  draws at most one frame per `refresh_interval` (default 100 ms), and only
  when something changed or a progress bar may have expired. `flush()` asks for a frame and returns when it has
  been written. `frames_written()` and `frames_failed()` count frames.
- After a fatal record, `flush()` stops drawing, restores the terminal and
  writes the log lines it holds, the fatal one last, as plain text to the
  normal screen, where they stay after the process ends.
- The other ways out that Lumen can see restore the terminal too. While a
  dashboard exists, Lumen's terminate handler restores the terminal of every
  live dashboard and writes their log lines (this covers a fatal record that
  the dashboard's predicate filtered out, and an uncaught exception), then
  calls the handler installed before it (with no exception active and the
  C++ library's default handler before it, `std::abort()` instead, for the
  reason above); an application that calls
  `std::set_terminate` after creating a dashboard replaces it. A function
  registered with `std::atexit` restores the terminal when the process exits
  with the sink still alive (`std::exit`, or a Core that is never
  destroyed), and one registered with `std::at_quick_exit` does the same for
  `std::quick_exit` (not on macOS, which has no `at_quick_exit`).
- **Signals.** Lumen installs no signal handler; that is the application's
  decision. A signal that ends the process (Ctrl-C's `SIGINT`, `SIGTERM`, a
  crash) leaves the terminal in the alternate screen with the cursor hidden
  (`reset` in the shell repairs it). To restore it, install a handler and
  call `lumen::DashboardSink::restore_terminal_after_signal()` from it, then
  end the process. That function is async-signal-safe: it writes the two
  escapes to standard error with one `write` call, and only while a
  dashboard drawing to standard error exists. It does not stop the drawing
  thread, so the process must end right after:
- What is kept is `DashboardModel`'s business (`lumen/dashboard_model.hpp`,
  built in every configuration): 512 log lines, 128 values per metric, 8
  metrics when none are named, 16 progress bars. A finished bar stays 2 s after
  its last record; any bar idle for 60 s is removed.

<!-- check: prelude=common -->
```cpp
#include <csignal>

#include "lumen/dashboard_sink.hpp"  // not included by lumen/lumen.hpp

extern "C" void restore_terminal_and_end(int signal_number) {
    lumen::DashboardSink::restore_terminal_after_signal();
    // The default action then ends the process as the signal would have.
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}

void install_terminal_restoring_handlers() {
    for (const int signal_number : {SIGINT, SIGTERM}) {
        std::signal(signal_number, restore_terminal_and_end);
    }
}
```

<!-- check: prelude=common wrap=body -->
```cpp
#include "lumen/dashboard_sink.hpp"  // not included by lumen/lumen.hpp

lumen::DashboardSink::Config config;
config.metrics = {"loss", "accuracy"};
config.refresh_interval = std::chrono::milliseconds{200};
auto dashboard = lumen::DashboardSink::create(std::move(config));
if (!dashboard) {
    // LumenError::NotATerminal when standard error is redirected
    std::cerr << "no dashboard: " << dashboard.error().message() << '\n';
}
```

## NullSink

Accepts every record and does nothing with it; useful to measure the cost of
the Core alone.

## Writing a sink

Derive from `lumen::Sink`, override the callbacks you need and `flush()`, and
keep to [the contract](#the-contract). This complete example
(`examples/custom_sink.cpp`, run by ctest as `Example.CustomSink`) writes
comma-separated lines from a thread of its own:

<!-- check: from=examples/custom_sink.cpp -->
```cpp
// A sink of your own: comma-separated lines for log and metric records,
// written to a stream by a thread the sink owns.
//
// The contract (lumen/sink.hpp): on_log, on_metric and on_progress run on the
// Core's dispatch thread, which serves every sink, so they must not wait for
// I/O. This sink formats the line, appends it to a bounded queue under a
// mutex that the writer thread holds only to take the whole queue, and
// returns. flush() is where waiting is allowed: it returns once every line
// queued before it has been written and the stream flushed.
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "lumen/lumen.hpp"

namespace {

[[nodiscard]] std::string_view level_name(lumen::LogLevel level) {
    switch (level) {
        case lumen::LogLevel::Trace: return "TRACE";
        case lumen::LogLevel::Debug: return "DEBUG";
        case lumen::LogLevel::Info:  return "INFO";
        case lumen::LogLevel::Warn:  return "WARN";
        case lumen::LogLevel::Error: return "ERROR";
        case lumen::LogLevel::Fatal: return "FATAL";
    }
    return "UNKNOWN";
}

// A CSV field: quoted, with quotes doubled (RFC 4180).
[[nodiscard]] std::string csv_field(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        out += c;
        if (c == '"') {
            out += '"';
        }
    }
    out += '"';
    return out;
}

[[nodiscard]] std::string tags_field(const lumen::RecordTags& tags) {
    std::string out;
    for (const auto& [key, value] : tags) {
        out += std::format("{}{}={}", out.empty() ? "" : ";", key, value);
    }
    return csv_field(out);
}

class CsvSink final : public lumen::Sink {
    // Only create() can make the key, so a CsvSink always has its thread.
    class Key {
        friend class CsvSink;
        Key() = default;
    };

public:
    // `out` must outlive the sink. Fails with the error of std::thread when
    // the writer thread cannot be started.
    [[nodiscard]] static std::expected<std::unique_ptr<CsvSink>, std::error_code> create(
        std::ostream& out, std::size_t max_queued_lines) {
        try {
            return std::make_unique<CsvSink>(Key{}, out, max_queued_lines);
        } catch (const std::system_error& e) {
            return std::unexpected(e.code());
        }
    }

    CsvSink(Key /*key*/, std::ostream& out, std::size_t max_queued_lines)
        : out_(out), max_queued_lines_(max_queued_lines), writer_([this] { write_lines_(); }) {}

    ~CsvSink() override {
        {
            const std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        work_.notify_one();
        writer_.join();  // the writer empties the queue before it returns
    }

    CsvSink(const CsvSink&) = delete;
    CsvSink& operator=(const CsvSink&) = delete;
    CsvSink(CsvSink&&) = delete;
    CsvSink& operator=(CsvSink&&) = delete;

    void on_log(const lumen::LogRecord& record) override {
        enqueue_(std::format("log,{},{},{}", level_name(record.level), csv_field(record.message),
                             tags_field(record.tags)));
    }

    void on_metric(const lumen::MetricRecord& record) override {
        enqueue_(std::format("metric,{},{},{}", csv_field(record.name), record.value,
                             tags_field(record.tags)));
    }

    // on_progress is not overridden: the base class ignores progress records.

    void flush() override {
        std::unique_lock lock(mutex_);
        const uint64_t target = accepted_;
        written_cv_.wait(lock, [&] { return written_ >= target; });
    }

    [[nodiscard]] uint64_t dropped() const {
        const std::scoped_lock lock(mutex_);
        return dropped_;
    }

private:
    void enqueue_(std::string line) {
        {
            const std::scoped_lock lock(mutex_);
            if (queue_.size() >= max_queued_lines_) {
                ++dropped_;  // never wait for the writer: count the line and move on
                return;
            }
            queue_.push_back(std::move(line));
            ++accepted_;
        }
        work_.notify_one();
    }

    void write_lines_() {
        std::unique_lock lock(mutex_);
        while (true) {
            work_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;  // stopping, and nothing left to write
            }
            std::deque<std::string> batch;
            batch.swap(queue_);
            lock.unlock();
            for (const std::string& line : batch) {
                out_ << line << '\n';
            }
            out_.flush();
            lock.lock();
            written_ += batch.size();
            written_cv_.notify_all();
        }
    }

    std::ostream& out_;  // written only by the writer thread
    const std::size_t max_queued_lines_;
    mutable std::mutex mutex_;
    std::condition_variable work_;        // waits on mutex_
    std::condition_variable written_cv_;  // waits on mutex_
    std::deque<std::string> queue_;       // guarded by mutex_
    uint64_t accepted_ = 0;               // guarded by mutex_
    uint64_t written_ = 0;                // guarded by mutex_
    uint64_t dropped_ = 0;                // guarded by mutex_
    bool stopping_ = false;               // guarded by mutex_
    // std::thread rather than std::jthread: Apple's libc++ lacks jthread.
    // Declared last, so that it starts after every member it uses exists.
    std::thread writer_;
};

}  // namespace

int main() {
    auto created = CsvSink::create(std::cout, 1000);
    if (!created) {
        std::cerr << "cannot create the CSV sink: " << created.error().message() << '\n';
        return 1;
    }
    const CsvSink* const csv = created->get();  // non-owning: the Core owns the sink
    if (const auto id = lumen::core().add_sink(std::move(*created)); !id) {
        std::cerr << "cannot register the CSV sink: " << id.error().message() << '\n';
        return 1;
    }

    LUMEN_LOG_INFO("run started").tag("run", 7);
    LUMEN_LOG_WARN("say \"cheese\"").tag("camera", "front");
    lumen::metric("exposure_ms", 12.5).tag("camera", "front");
    auto bar = lumen::progress("frames", 2);  // progress records: ignored by this sink
    bar.finish();

    lumen::core().flush();
    std::cout << "dropped: " << csv->dropped() << '\n';
    return 0;
}
```

It prints:

<!-- check: output=examples/expected/custom_sink.stdout -->
```text
log,INFO,"run started","run=7"
log,WARN,"say ""cheese""","camera=front"
metric,"exposure_ms",12.5,"camera=front"
dropped: 0
```

The metric line can come before the log lines: the Core does not order
records of different types with each other.
