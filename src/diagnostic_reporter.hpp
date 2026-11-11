#ifndef LUMEN_SRC_DIAGNOSTIC_REPORTER_HPP
#define LUMEN_SRC_DIAGNOSTIC_REPORTER_HPP

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "lumen/core.hpp"

namespace lumen::detail {

// Passes a Core's diagnostic lines to its DiagnosticHandler, with the
// guarantees core.hpp states for the handler.
//
// Lines reach the handler one at a time, in the order they were reported.
// The thread that finds nobody draining becomes the drainer and passes on
// every pending line, its own and those other threads (or the handler
// itself, re-entering) add meanwhile; the others only queue their line. No
// lock is held while the handler runs, so a handler may replace itself, log,
// or flush.
class DiagnosticReporter {
public:
    explicit DiagnosticReporter(DiagnosticHandler handler);

    void report(std::string_view line);

    // An empty handler discards the lines.
    void set_handler(DiagnosticHandler handler);

private:
    void drain_();

    // Shared, so that the drainer copies a pointer, which cannot throw, and a
    // replaced handler lives until its last call returns.
    std::mutex mutex_;
    std::shared_ptr<const DiagnosticHandler> handler_;  // guarded by mutex_
    std::deque<std::string> pending_;                   // guarded by mutex_
    bool draining_ = false;                             // guarded by mutex_
};

}  // namespace lumen::detail

#endif
