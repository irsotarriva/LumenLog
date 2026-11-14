#ifndef LUMEN_ERROR_HPP
#define LUMEN_ERROR_HPP

#include <cstdint>
#include <string>
#include <system_error>

namespace lumen {

// The errors Lumen returns, as std::error_code values in the "lumen"
// category. Compare a returned code with the enumerator directly:
//
//   if (result.error() == lumen::LumenError::TagTableFull) { ... }
enum class LumenError : uint8_t {
    None = 0,
    // remove_sink, sink_exception_count: no sink is registered under this id.
    InvalidSinkId,
    // add_sink: the sink pointer is null.
    NullSink,
    // remove_sink: a callback of the sink being removed is running further up
    // the calling thread's stack (the sink removes itself, or another sink
    // removes the one that raised the fatal record it is receiving).
    // Core::flush_for: called from inside a callback of the same Core, or
    // waiting would never end (a cycle of flushes between Cores).
    SinkBusy,
    // set_process_tag, set_thread_tag, Tagged::lumen_tag: the key is new and
    // the table already holds its maximum number of keys (lumen/limits.hpp).
    TagTableFull,
    // parse_predicate: the query is not valid.
    InvalidPredicate,
    // A sink factory (TerminalSink::create, FileSink::create, ...): a Config
    // field is out of its documented range.
    InvalidSinkConfig,
    // FileSink::create: the file could not be opened for appending.
    FileOpenFailed,
    // A sink's destination refused a write (reported through the sink's
    // diagnostics and counted in its statistics, never returned by on_*).
    IoError,
    // DashboardSink::create: the destination is not a terminal.
    NotATerminal,
    // add_sink: the Core has begun shutting down (for the global core(), at
    // static destruction), so a new sink would never receive a record.
    // Core::flush_for: the Core has shut down, or gave up its shutdown.
    CoreShutDown,
    // Core::flush_for: the time limit passed before the flush was done.
    FlushTimedOut,
};

class LumenErrorCategory : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::string message(int ev) const override;
};

[[nodiscard]] const LumenErrorCategory& lumen_error_category();
[[nodiscard]] std::error_code make_error_code(LumenError e);

}  // namespace lumen

template <>
struct std::is_error_code_enum<lumen::LumenError> : std::true_type {};

#endif
