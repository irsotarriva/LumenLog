#ifndef LUMEN_SRC_FIRST_REPORTS_HPP
#define LUMEN_SRC_FIRST_REPORTS_HPP

#include <atomic>
#include <cstdint>
#include <string_view>

#include "diagnostic_reporter.hpp"
#include "sink_registry.hpp"

namespace lumen::detail {

// "Report the first, then only count" for a Core: the first record refused
// by a full queue, the first tag left out of a full record, and the first
// exception of each sink each produce one diagnostic line; later ones are
// only counted (in Core::statistics() and sink_exception_count). This class
// decides which occurrence is the first and words the lines.
class FirstReports {
public:
    explicit FirstReports(DiagnosticReporter& reporter) : reporter_(&reporter) {}

    // Given the totals counted so far, reports a kind of loss the first time
    // its total is not zero.
    void report_new_losses(uint64_t refused_records, uint64_t dropped_tags);

    // Counts an exception that `entry`'s `callback` threw and reports it if it
    // is that sink's first.
    void sink_threw(SinkEntry& entry, std::string_view callback, std::string_view what);

private:
    DiagnosticReporter* reporter_;  // non-owning: the Core's, which outlives this
    std::atomic<bool> reported_queue_full_{false};
    std::atomic<bool> reported_tags_dropped_{false};
};

}  // namespace lumen::detail

#endif
