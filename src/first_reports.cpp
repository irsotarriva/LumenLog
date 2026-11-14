#include "first_reports.hpp"

#include <format>

namespace lumen::detail {

void FirstReports::report_new_losses(uint64_t refused_records, uint64_t dropped_tags) {
    if (refused_records != 0 && !reported_queue_full_.exchange(true, std::memory_order_relaxed)) {
        reporter_->report(
            "lumen: a record was dropped because its queue was full; further drops are "
            "only counted (Core::statistics())");
    }
    if (dropped_tags != 0 && !reported_tags_dropped_.exchange(true, std::memory_order_relaxed)) {
        reporter_->report(
            "lumen: a tag was dropped because its record already had the maximum number "
            "of tags; further drops are only counted (Core::statistics())");
    }
}

void FirstReports::sink_threw(SinkEntry& entry, std::string_view callback, std::string_view what) {
    if (entry.exceptions.fetch_add(1, std::memory_order_relaxed) == 0) {
        reporter_->report(
            std::format("lumen: sink {} threw from {}: {} (further exceptions from this "
                        "sink are only counted)",
                        entry.id, callback, what));
    }
}

}  // namespace lumen::detail
