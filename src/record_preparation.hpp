#ifndef LUMEN_SRC_RECORD_PREPARATION_HPP
#define LUMEN_SRC_RECORD_PREPARATION_HPP

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "lumen/detail/context.hpp"
#include "lumen/record.hpp"
#include "lumen/thread_clock.hpp"

namespace lumen::detail {

[[nodiscard]] inline std::string_view& text_of(LogRecord& r) noexcept {
    return r.message;
}
[[nodiscard]] inline std::string_view& text_of(MetricRecord& r) noexcept {
    return r.name;
}
[[nodiscard]] inline std::string_view& text_of(ProgressRecord& r) noexcept {
    return r.label;
}

// Copies `texts` (the record's text, and for some log records its source
// strings) and every tag string into one NUL-terminated block and re-points
// the views at it.
template <RecordType R>
void own_strings(R& record, std::span<std::string_view* const> texts) {
    std::size_t total = 0;
    for (const std::string_view* const text : texts) {
        total += text->size() + 1;
    }
    for (const auto& e : record.tags) {
        total += e.key.size() + 1 + e.value.size() + 1;
    }
    // The array form of make_shared: one allocation for all the strings.
    // NOLINTNEXTLINE(*-avoid-c-arrays)
    auto block = std::make_shared_for_overwrite<char[]>(total);
    std::span<char> out(block.get(), total);
    auto copy = [&out](std::string_view s) {
        std::ranges::copy(s, out.begin());
        out[s.size()] = '\0';
        const std::string_view owned(out.data(), s.size());
        out = out.subspan(s.size() + 1);
        return owned;
    };
    for (std::string_view* const text : texts) {
        *text = copy(*text);
    }
    record.tags.remap(copy);
    record.storage = std::move(block);
}

// Makes a submitted record complete, on the producing thread: stamps it
// (now_ns(), this_thread_id()), merges the ambient context after its
// explicit tags, and copies its strings into its own storage; a log record's
// source strings too when `copy_source` is set (they were given as text).
// Returns how many ambient tags did not fit.
template <RecordType R>
[[nodiscard]] uint32_t prepare_record(R& record, bool copy_source) {
    record.timestamp_ns = now_ns();
    record.thread_id = this_thread_id();
    const uint32_t dropped = merge_context(record.tags);
    if constexpr (std::same_as<R, LogRecord>) {
        if (copy_source) {
            const std::array texts{&record.message, &record.source.file, &record.source.function,
                                   &record.source.class_name};
            own_strings(record, texts);
            return dropped;
        }
    }
    const std::array texts{&text_of(record)};
    own_strings(record, texts);
    return dropped;
}

}  // namespace lumen::detail

#endif
