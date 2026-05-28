#ifndef LUMEN_RECORD_H
#define LUMEN_RECORD_H

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include <source_location>
#include <string_view>
#include <thread>

#include "lumen/detail/arena.h"
#include "lumen/detail/class_name.hpp"
#include "lumen/detail/overflow_policy.h"
#include "lumen/detail/ring_buffer.h"

namespace lumen {

enum LogLevel : uint32_t {
    TRACE  = 0,
    DEBUG  = 1,
    INFO   = 2,
    WARN   = 3,
    ERROR  = 4,
    FATAL  = 5,
};

struct SourceLocation {
    std::string_view       file;
    std::string_view       function;
    std::string_view       class_name;
    uint32_t               line;
};

template <size_t N = 16>
class TagSet {
public:
    struct Entry {
        std::string_view key;
        std::string_view value;
    };

    void add(std::string_view key, std::string_view value) {
        if (__count < N) {
            __entries[__count++] = {key, value};
        }
    }
    [[nodiscard]] std::string_view find(std::string_view key) const {
        for (size_t i = 0; i < __count; ++i) {
            if (__entries[i].key == key) return __entries[i].value;
        }
        return {};
    }
    [[nodiscard]] size_t count() const { return __count; }
    [[nodiscard]] const Entry* begin() const { return __entries.data(); }
    [[nodiscard]] const Entry* end() const { return __entries.data() + __count; }

private:
    std::array<Entry, N> __entries{};
    size_t __count = 0;
};

// ── Record types ──────────────────────────────────────────────────────────────

struct LogRecord {
    LogLevel          level;
    std::string_view  message;
    TagSet<16>        tags;
    uint64_t          timestamp_ns;
    uint32_t          thread_id;
    SourceLocation    source;
};

struct MetricRecord {
    std::string_view  name;
    double            value;
    TagSet<16>        tags;
    uint64_t          timestamp_ns;
};

struct ProgressRecord {
    uint64_t          id;
    std::string_view  label;
    uint64_t          current;
    uint64_t          total;
    TagSet<16>        tags;
    uint64_t          timestamp_ns;
};

// ── Utilities ─────────────────────────────────────────────────────────────────

inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

inline uint32_t this_thread_id() {
    static thread_local uint32_t __cached_id = 0;
    if (__cached_id == 0) {
        __cached_id = static_cast<uint32_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFFFFu);
    }
    return __cached_id;
}

// ── Buffer type aliases ───────────────────────────────────────────────────────

#ifndef LUMEN_LOG_CAPACITY
#define LUMEN_LOG_CAPACITY 65536
#endif
#ifndef LUMEN_METRIC_CAPACITY
#define LUMEN_METRIC_CAPACITY 16384
#endif
#ifndef LUMEN_PROGRESS_CAPACITY
#define LUMEN_PROGRESS_CAPACITY 4096
#endif

using LogBuffer     = RingBuffer<LogRecord, LUMEN_LOG_CAPACITY>;
using MetricBuffer  = RingBuffer<MetricRecord, LUMEN_METRIC_CAPACITY>;
using ProgressBuffer = RingBuffer<ProgressRecord, LUMEN_PROGRESS_CAPACITY>;

namespace detail {
const TagSet<8>& tls_tags();
const TagSet<16>& proc_tags();
void collect_scope_tags(TagSet<8>& out);
const TagSet<8>* current_instance_tags();
void merge_context(LogRecord&, const TagSet<16>&, const TagSet<8>&,
                   const TagSet<8>*, const TagSet<8>*);
void merge_context(MetricRecord&, const TagSet<16>&, const TagSet<8>&,
                   const TagSet<8>*, const TagSet<8>*);
void merge_context(ProgressRecord&, const TagSet<16>&, const TagSet<8>&,
                   const TagSet<8>*, const TagSet<8>*);
}  // namespace detail

// ── RecordBuilder ─────────────────────────────────────────────────────────────

class RecordBuilder {
public:
    RecordBuilder(LogBuffer& buffer, LogLevel level, std::string_view message,
                  std::source_location loc = std::source_location::current())
        : __buffer(buffer) {
        __record.level = level;
        __record.message = message;
        __record.timestamp_ns = now_ns();
        __record.thread_id = this_thread_id();
        __record.source.file = loc.file_name();
        __record.source.function = loc.function_name();
        __record.source.class_name = detail::extract_class_name(loc.function_name());
        __record.source.line = loc.line();
    }

    ~RecordBuilder() {
        TagSet<8> scope_tags;
        detail::collect_scope_tags(scope_tags);
        const TagSet<8>* inst_tags = detail::current_instance_tags();
        detail::merge_context(__record, detail::proc_tags(), detail::tls_tags(),
                              scope_tags.count() > 0 ? &scope_tags : nullptr,
                              inst_tags);
        __buffer.push(std::move(__record));
    }

    RecordBuilder(const RecordBuilder&) = delete;
    RecordBuilder& operator=(const RecordBuilder&) = delete;

    RecordBuilder& tag(std::string_view key, std::string_view value) {
        __record.tags.add(key, value);
        return *this;
    }
    RecordBuilder& tag(std::string_view key, int64_t value) {
        char* buf = __arena_buf();
        int n = std::snprintf(buf, 32, "%lld", static_cast<long long>(value));
        if (n > 0 && n < 32) __record.tags.add(key, std::string_view(buf, static_cast<size_t>(n)));
        return *this;
    }
    RecordBuilder& tag(std::string_view key, double value) {
        char* buf = __arena_buf();
        int n = std::snprintf(buf, 32, "%.6g", value);
        if (n > 0 && n < 32) __record.tags.add(key, std::string_view(buf, static_cast<size_t>(n)));
        return *this;
    }

private:
    char* __arena_buf() {
        return this_thread_arena.allocate(32);
    }

    LogBuffer& __buffer;
    LogRecord __record{};
};

// ── MetricBuilder ─────────────────────────────────────────────────────────────

class MetricBuilder {
public:
    MetricBuilder(MetricBuffer& buffer, std::string_view name, double value)
        : __buffer(buffer) {
        __record.name = name;
        __record.value = value;
        __record.timestamp_ns = now_ns();
    }

    ~MetricBuilder() {
        TagSet<8> scope_tags;
        detail::collect_scope_tags(scope_tags);
        const TagSet<8>* inst_tags = detail::current_instance_tags();
        detail::merge_context(__record, detail::proc_tags(), detail::tls_tags(),
                              scope_tags.count() > 0 ? &scope_tags : nullptr,
                              inst_tags);
        __buffer.push(std::move(__record));
    }

    MetricBuilder(const MetricBuilder&) = delete;
    MetricBuilder& operator=(const MetricBuilder&) = delete;

    MetricBuilder& tag(std::string_view key, std::string_view value) {
        __record.tags.add(key, value);
        return *this;
    }
    MetricBuilder& tag(std::string_view key, int64_t value) {
        char* buf = __arena_buf();
        int n = std::snprintf(buf, 32, "%lld", static_cast<long long>(value));
        if (n > 0 && n < 32) __record.tags.add(key, std::string_view(buf, static_cast<size_t>(n)));
        return *this;
    }
    MetricBuilder& tag(std::string_view key, double value) {
        char* buf = __arena_buf();
        int n = std::snprintf(buf, 32, "%.6g", value);
        if (n > 0 && n < 32) __record.tags.add(key, std::string_view(buf, static_cast<size_t>(n)));
        return *this;
    }

private:
    char* __arena_buf() {
        return this_thread_arena.allocate(32);
    }

    MetricBuffer& __buffer;
    MetricRecord __record{};
};

// ── ProgressHandle ────────────────────────────────────────────────────────────

class ProgressHandle {
public:
    ProgressHandle(ProgressBuffer& buffer, std::string_view label, uint64_t total)
        : __buffer(&buffer) {
        static std::atomic<uint64_t> __next_id{1};
        __record.id = __next_id.fetch_add(1, std::memory_order_relaxed);
        __record.label = label;
        __record.current = 0;
        __record.total = total;
        __record.timestamp_ns = now_ns();
    }

    ~ProgressHandle() {
        if (!__finished) {
            TagSet<8> scope_tags;
            detail::collect_scope_tags(scope_tags);
            const TagSet<8>* inst_tags = detail::current_instance_tags();
            detail::merge_context(__record, detail::proc_tags(), detail::tls_tags(),
                                  scope_tags.count() > 0 ? &scope_tags : nullptr,
                                  inst_tags);
            __buffer->push(std::move(__record));
        }
    }

    ProgressHandle(const ProgressHandle&) = delete;
    ProgressHandle& operator=(const ProgressHandle&) = delete;

    ProgressHandle(ProgressHandle&& other) noexcept
        : __buffer(other.__buffer)
        , __record(other.__record)
        , __finished(other.__finished) {
        other.__finished = true;
    }
    ProgressHandle& operator=(ProgressHandle&& other) noexcept {
        if (this != &other) {
            __buffer = other.__buffer;
            __record = other.__record;
            __finished = other.__finished;
            other.__finished = true;
        }
        return *this;
    }

    void update(uint64_t current) {
        __record.current = current;
        __record.timestamp_ns = now_ns();
        __push_current();
    }

    void tick() {
        update(__record.current + 1);
    }

    void finish() {
        __record.current = __record.total;
        __record.timestamp_ns = now_ns();
        if (!__finished) {
            TagSet<8> scope_tags;
            detail::collect_scope_tags(scope_tags);
            const TagSet<8>* inst_tags = detail::current_instance_tags();
            detail::merge_context(__record, detail::proc_tags(), detail::tls_tags(),
                                  scope_tags.count() > 0 ? &scope_tags : nullptr,
                                  inst_tags);
            __buffer->push(ProgressRecord{__record});
            __finished = true;
        }
    }

private:
    void __push_current() {
        TagSet<8> scope_tags;
        detail::collect_scope_tags(scope_tags);
        const TagSet<8>* inst_tags = detail::current_instance_tags();
        detail::merge_context(__record, detail::proc_tags(), detail::tls_tags(),
                              scope_tags.count() > 0 ? &scope_tags : nullptr,
                              inst_tags);
        __buffer->push(ProgressRecord{__record});
    }

    ProgressBuffer* __buffer;  // non-owning
    ProgressRecord __record{};
    bool __finished = false;
};

}  // namespace lumen

#include "lumen/detail/scope_stack.h"

// ── Level integer constants for compile-time macro elision ────────────────────

#define LUMEN_LEVEL_TRACE 0
#define LUMEN_LEVEL_DEBUG 1
#define LUMEN_LEVEL_INFO  2
#define LUMEN_LEVEL_WARN  3
#define LUMEN_LEVEL_ERROR 4
#define LUMEN_LEVEL_FATAL 5

#define LUMEN_LEVEL_INT_TRACE  0
#define LUMEN_LEVEL_INT_DEBUG  1
#define LUMEN_LEVEL_INT_INFO   2
#define LUMEN_LEVEL_INT_WARN   3
#define LUMEN_LEVEL_INT_ERROR  4
#define LUMEN_LEVEL_INT_FATAL  5

#define LUMEN_LEVEL_CAT_IMPL(x) LUMEN_LEVEL_INT_##x
#define LUMEN_LEVEL_CAT(x) LUMEN_LEVEL_CAT_IMPL(x)
#define LUMEN_MIN_LEVEL_INT LUMEN_LEVEL_CAT(LUMEN_MIN_LEVEL)

#define LUMEN_CONCAT_IMPL(a, b) a##b
#define LUMEN_CONCAT(a, b) LUMEN_CONCAT_IMPL(a, b)

// ── Macro helpers ─────────────────────────────────────────────────────────────

#ifndef __has_cpp_attribute
#define __has_cpp_attribute(x) 0
#endif

#define LUMEN_IF_ENABLED(LEVEL, expr) \
    if constexpr (LUMEN_LEVEL_##LEVEL >= LUMEN_MIN_LEVEL_INT) expr

// ── Log macros ────────────────────────────────────────────────────────────────

#define LOG_TRACE(msg) \
    LUMEN_IF_ENABLED(TRACE, \
        lumen::RecordBuilder(lumen::core().log_buffer(), lumen::LogLevel::TRACE, msg))

#define LOG_DEBUG(msg) \
    LUMEN_IF_ENABLED(DEBUG, \
        lumen::RecordBuilder(lumen::core().log_buffer(), lumen::LogLevel::DEBUG, msg))

#define LOG_INFO(msg) \
    LUMEN_IF_ENABLED(INFO, \
        lumen::RecordBuilder(lumen::core().log_buffer(), lumen::LogLevel::INFO, msg))

#define LOG_WARN(msg) \
    LUMEN_IF_ENABLED(WARN, \
        lumen::RecordBuilder(lumen::core().log_buffer(), lumen::LogLevel::WARN, msg))

#define LOG_ERROR(msg) \
    LUMEN_IF_ENABLED(ERROR, \
        lumen::RecordBuilder(lumen::core().log_buffer(), lumen::LogLevel::ERROR, msg))

#define LOG_FATAL(msg) do { \
    { \
        lumen::RecordBuilder LUMEN_CONCAT(__lumen_fatal_, __LINE__) \
            (lumen::core().log_buffer(), lumen::LogLevel::FATAL, msg); \
    } \
    lumen::core().flush(); \
    std::terminate(); \
} while(0)

// ── Scope macros ──────────────────────────────────────────────────────────────

#define LUMEN_SCOPE(key, value) \
    lumen::detail::ScopeGuard LUMEN_CONCAT(__lumen_scope_, __LINE__)(key, value)

#define LUMEN_FRAME_SCOPE(key, value) \
    lumen::detail::ScopeGuard LUMEN_CONCAT(__lumen_frame_, __LINE__)(key, value)

#define LUMEN_MEMBER_SCOPE \
    auto LUMEN_CONCAT(__lumen_member_, __LINE__) = lumen_scope()

#define LUMEN_LOOP(label, var, total) \
    for (lumen::detail::LoopScope LUMEN_CONCAT(__lumen_loop_, __LINE__)(label, lumen::core().progress_buffer(), static_cast<uint64_t>(total)); \
         LUMEN_CONCAT(__lumen_loop_, __LINE__).advance(); ) \
        if (uint64_t var = LUMEN_CONCAT(__lumen_loop_, __LINE__).current(); true)

#endif
