#ifndef LUMEN_RECORD_BUILDER_HPP
#define LUMEN_RECORD_BUILDER_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <source_location>
#include <string_view>
#include <utility>

#include "lumen/core.hpp"
#include "lumen/detail/class_name.hpp"
#include "lumen/detail/string_scratch.hpp"
#include "lumen/record.hpp"

// Builders: a log or metric record under construction. A builder lives for
// one statement:
//
//   LUMEN_LOG_INFO("energy {:.2f} GeV", e).tag("run", run_id);
//   lumen::metric("loss", loss).tag("epoch", epoch);
//
// and submits its record to its Core when it is destroyed, at the end of the
// full expression, on the producing thread. That is when the record gets its
// timestamp and thread id and when the ambient context is merged.
//
// Tags. .tag(key, value) copies both strings at once, so temporaries are
// fine. Numbers are written as text: integers exactly, floating point in the
// shortest form that reads back as the same value; bool as "true"/"false".
// Tagging a key the record already has replaces its value (the later .tag()
// wins). A record holds at most max_record_tags tags; .tag() with a new key on
// a full record is ignored and counted in Core::statistics().tags_dropped.
//
// Fatal. A builder for a LogLevel::Fatal record does not queue it. Its
// destructor delivers, on the calling thread, every record queued before it,
// then the fatal record, then flushes every sink, and then calls
// std::terminate(). If it runs inside a sink callback of the same Core, the
// sink whose callback is running is skipped (it is in the middle of a call
// and may hold its own locks); every other sink receives the record and is
// flushed. After the Core has shut down the record is only counted, and the
// program still terminates.

namespace lumen {

template <BuiltRecord R>
class BasicRecordBuilder {
public:
    // A log record whose message is `message`, verbatim.
    BasicRecordBuilder(Core& core, LogLevel level, std::source_location loc,
                       std::string_view message)
        requires std::same_as<R, LogRecord>
        : core_(&core) {
        init_log_(level, loc);
        record_.message = scratch_.store(message);
    }

    // A log record whose message is std::format(fmt, args...), checked at
    // compile time and formatted into the builder's scratch buffer.
    template <typename... Args>
        requires std::same_as<R, LogRecord> && (sizeof...(Args) > 0)
    BasicRecordBuilder(Core& core, LogLevel level, std::source_location loc,
                       std::format_string<Args...> fmt, Args&&... args)
        : core_(&core) {
        init_log_(level, loc);
        record_.message = scratch_.store_formatted(fmt, std::forward<Args>(args)...);
    }

    // A metric record.
    BasicRecordBuilder(Core& core, std::string_view name, double value)
        requires std::same_as<R, MetricRecord>
        : core_(&core) {
        record_.name = scratch_.store(name);
        record_.value = value;
    }

    ~BasicRecordBuilder() {
        if constexpr (std::same_as<R, LogRecord>) {
            if (record_.level == LogLevel::Fatal) {
                core_->submit_fatal_(std::move(record_), tags_dropped_);
            }
        }
        core_->submit_(std::move(record_), tags_dropped_);
    }

    BasicRecordBuilder(const BasicRecordBuilder&) = delete;
    BasicRecordBuilder& operator=(const BasicRecordBuilder&) = delete;
    BasicRecordBuilder(BasicRecordBuilder&&) = delete;
    BasicRecordBuilder& operator=(BasicRecordBuilder&&) = delete;

    BasicRecordBuilder& tag(std::string_view key, std::string_view value) {
        set_tag_(key, scratch_.store(value));
        return *this;
    }

    // Any integer or floating-point type (a plain `int` would otherwise be
    // ambiguous between int64_t and double). bool has its own overload.
    template <typename T>
        requires((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>)
    BasicRecordBuilder& tag(std::string_view key, T value) {
        set_tag_(key, scratch_.store_number(value));
        return *this;
    }

    // A template so that a string literal (const char* -> bool is a standard
    // conversion) still picks the string_view overload.
    template <std::same_as<bool> T>
    BasicRecordBuilder& tag(std::string_view key, T value) {
        return tag(key, std::string_view(value ? "true" : "false"));
    }

private:
    void init_log_(LogLevel level, std::source_location loc) {
        record_.level = level;
        record_.source.file = loc.file_name();
        record_.source.function = loc.function_name();
        record_.source.class_name = detail::extract_class_name(loc.function_name());
        record_.source.line = loc.line();
    }

    void set_tag_(std::string_view key, std::string_view stored_value) {
        if (record_.tags.contains(key)) {
            static_cast<void>(record_.tags.set(key, stored_value));  // an existing key always fits
        } else if (!record_.tags.add(scratch_.store(key), stored_value)) {
            ++tags_dropped_;
        }
    }

    Core* core_;  // non-owning
    R record_{};
    uint32_t tags_dropped_ = 0;
    detail::StringScratch scratch_;
};

using RecordBuilder = BasicRecordBuilder<LogRecord>;
using MetricBuilder = BasicRecordBuilder<MetricRecord>;

// A metric record for the global core(): lumen::metric("loss", 0.25).tag("epoch", 3);
inline MetricBuilder metric(std::string_view name, double value) {
    return MetricBuilder(core(), name, value);
}

}  // namespace lumen

#endif
