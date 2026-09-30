#ifndef LUMEN_RECORD_H
#define LUMEN_RECORD_H

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <forward_list>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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
    std::string_view       file{};
    std::string_view       function{};
    std::string_view       class_name{};
    uint32_t               line{};
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
    // Replaces the value of an existing key, or adds the pair if absent.
    void set(std::string_view key, std::string_view value) {
        for (size_t i = 0; i < __count; ++i) {
            if (__entries[i].key == key) {
                __entries[i].value = value;
                return;
            }
        }
        add(key, value);
    }
    [[nodiscard]] std::string_view find(std::string_view key) const {
        for (size_t i = 0; i < __count; ++i) {
            if (__entries[i].key == key) return __entries[i].value;
        }
        return {};
    }
    [[nodiscard]] bool contains(std::string_view key) const {
        for (size_t i = 0; i < __count; ++i) {
            if (__entries[i].key == key) return true;
        }
        return false;
    }
    // Re-points every key and value through `f` (used to move them into owned storage).
    template <typename F>
    void remap(F&& f) {
        for (size_t i = 0; i < __count; ++i) {
            __entries[i].key = f(__entries[i].key);
            __entries[i].value = f(__entries[i].value);
        }
    }
    [[nodiscard]] size_t count() const { return __count; }
    [[nodiscard]] const Entry* begin() const { return __entries.data(); }
    [[nodiscard]] const Entry* end() const { return __entries.data() + __count; }

private:
    std::array<Entry, N> __entries{};
    size_t __count = 0;
};

// ── Record types ──────────────────────────────────────────────────────────────
//
// Records are consumed on the dispatch thread, long after the producing
// statement has finished. Every string_view in a record that went through the
// broker (message/name/label and all tag keys and values) therefore points
// into `storage`: an immutable, NUL-terminated block owned by the record and
// shared by its copies. Sinks may copy a record and keep it; the views stay
// valid for as long as any copy is alive.
//
// `SourceLocation` strings are not copied: they come from
// std::source_location and have static storage duration.

namespace detail {
using RecordStorage = std::shared_ptr<const char[]>;
}  // namespace detail

struct LogRecord {
    LogLevel          level{};
    std::string_view  message{};
    TagSet<16>        tags{};
    uint64_t          timestamp_ns{};
    uint32_t          thread_id{};
    SourceLocation    source{};
    detail::RecordStorage storage{};
};

struct MetricRecord {
    std::string_view  name{};
    double            value{};
    TagSet<16>        tags{};
    uint64_t          timestamp_ns{};
    detail::RecordStorage storage{};
};

struct ProgressRecord {
    uint64_t          id{};
    std::string_view  label{};
    uint64_t          current{};
    uint64_t          total{};
    TagSet<16>        tags{};
    uint64_t          timestamp_ns{};
    detail::RecordStorage storage{};
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
const TagSet<8>* current_instance_tags();

// Copies the record's message/name/label and all tag strings into a freshly
// allocated `storage` block and re-points the views at it.
void own_strings(LogRecord& record);
void own_strings(MetricRecord& record);
void own_strings(ProgressRecord& record);

// Merges ambient context (instance > scope > thread > process; tags already on
// the record win) and then calls own_strings(). Must run on the producing thread.
void finalize(LogRecord& record);
void finalize(MetricRecord& record);
void finalize(ProgressRecord& record);

// Holds copies of strings for the lifetime of a builder. Tag arguments are
// often temporaries that die before the builder's destructor runs, so they are
// copied here as soon as they are passed in. Short strings live inline; longer
// ones go to list nodes, whose addresses never change.
class StringScratch {
public:
    static constexpr size_t INLINE_CAPACITY = 512;

    StringScratch() = default;
    StringScratch(const StringScratch&) = delete;
    StringScratch& operator=(const StringScratch&) = delete;
    StringScratch(StringScratch&&) = delete;
    StringScratch& operator=(StringScratch&&) = delete;

    [[nodiscard]] std::string_view store(std::string_view s) {
        if (s.size() <= INLINE_CAPACITY - __used) {
            char* dst = __inline_buf.data() + __used;
            if (!s.empty()) std::memcpy(dst, s.data(), s.size());
            __used += s.size();
            return {dst, s.size()};
        }
        __overflow.emplace_front(s);
        return __overflow.front();
    }

    template <typename T>
    [[nodiscard]] std::string_view store_number(const char* fmt, T value) {
        std::array<char, 32> buf{};
        const int n = std::snprintf(buf.data(), buf.size(), fmt, value);
        if (n <= 0 || static_cast<size_t>(n) >= buf.size()) return {};
        return store(std::string_view(buf.data(), static_cast<size_t>(n)));
    }

private:
    std::array<char, INLINE_CAPACITY> __inline_buf;
    size_t __used = 0;
    std::forward_list<std::string> __overflow;
};

// LOG_* with a single argument logs it verbatim (no format parsing); with more
// arguments the first is a std::format string, checked at compile time.
[[nodiscard]] inline std::string_view format_message(std::string_view message) {
    return message;
}

template <typename... Args>
    requires(sizeof...(Args) > 0)
[[nodiscard]] std::string format_message(std::format_string<Args...> fmt, Args&&... args) {
    return std::format(fmt, std::forward<Args>(args)...);
}
}  // namespace detail

// ── RecordBuilder ─────────────────────────────────────────────────────────────

class RecordBuilder {
public:
    RecordBuilder(LogBuffer& buffer, LogLevel level, std::string_view message,
                  std::source_location loc = std::source_location::current())
        : __buffer(buffer) {
        __record.level = level;
        __record.message = __scratch.store(message);
        __record.timestamp_ns = now_ns();
        __record.thread_id = this_thread_id();
        __record.source.file = loc.file_name();
        __record.source.function = loc.function_name();
        __record.source.class_name = detail::extract_class_name(loc.function_name());
        __record.source.line = loc.line();
    }

    ~RecordBuilder() {
        detail::finalize(__record);
        __buffer.push(std::move(__record));
    }

    RecordBuilder(const RecordBuilder&) = delete;
    RecordBuilder& operator=(const RecordBuilder&) = delete;

    RecordBuilder& tag(std::string_view key, std::string_view value) {
        __record.tags.add(__scratch.store(key), __scratch.store(value));
        return *this;
    }
    RecordBuilder& tag(std::string_view key, int64_t value) {
        const std::string_view text = __scratch.store_number("%lld", static_cast<long long>(value));
        if (!text.empty()) __record.tags.add(__scratch.store(key), text);
        return *this;
    }
    RecordBuilder& tag(std::string_view key, double value) {
        const std::string_view text = __scratch.store_number("%.6g", value);
        if (!text.empty()) __record.tags.add(__scratch.store(key), text);
        return *this;
    }

private:
    LogBuffer& __buffer;
    LogRecord __record{};
    detail::StringScratch __scratch;
};

// ── MetricBuilder ─────────────────────────────────────────────────────────────

class MetricBuilder {
public:
    MetricBuilder(MetricBuffer& buffer, std::string_view name, double value)
        : __buffer(buffer) {
        __record.name = __scratch.store(name);
        __record.value = value;
        __record.timestamp_ns = now_ns();
    }

    ~MetricBuilder() {
        detail::finalize(__record);
        __buffer.push(std::move(__record));
    }

    MetricBuilder(const MetricBuilder&) = delete;
    MetricBuilder& operator=(const MetricBuilder&) = delete;

    MetricBuilder& tag(std::string_view key, std::string_view value) {
        __record.tags.add(__scratch.store(key), __scratch.store(value));
        return *this;
    }
    MetricBuilder& tag(std::string_view key, int64_t value) {
        const std::string_view text = __scratch.store_number("%lld", static_cast<long long>(value));
        if (!text.empty()) __record.tags.add(__scratch.store(key), text);
        return *this;
    }
    MetricBuilder& tag(std::string_view key, double value) {
        const std::string_view text = __scratch.store_number("%.6g", value);
        if (!text.empty()) __record.tags.add(__scratch.store(key), text);
        return *this;
    }

private:
    MetricBuffer& __buffer;
    MetricRecord __record{};
    detail::StringScratch __scratch;
};

// ── ProgressHandle ────────────────────────────────────────────────────────────

class ProgressHandle {
public:
    ProgressHandle(ProgressBuffer& buffer, std::string_view label, uint64_t total)
        : __buffer(&buffer), __label(label), __total(total), __timestamp_ns(now_ns()) {
        static std::atomic<uint64_t> __next_id{1};
        __id = __next_id.fetch_add(1, std::memory_order_relaxed);
    }

    ~ProgressHandle() {
        if (!__finished) {
            __push();
        }
    }

    ProgressHandle(const ProgressHandle&) = delete;
    ProgressHandle& operator=(const ProgressHandle&) = delete;

    ProgressHandle(ProgressHandle&& other) noexcept
        : __buffer(other.__buffer)
        , __label(std::move(other.__label))
        , __id(other.__id)
        , __current(other.__current)
        , __total(other.__total)
        , __timestamp_ns(other.__timestamp_ns)
        , __finished(other.__finished) {
        other.__finished = true;
    }
    ProgressHandle& operator=(ProgressHandle&& other) noexcept {
        if (this != &other) {
            if (!__finished) {
                __push();
            }
            __buffer = other.__buffer;
            __label = std::move(other.__label);
            __id = other.__id;
            __current = other.__current;
            __total = other.__total;
            __timestamp_ns = other.__timestamp_ns;
            __finished = other.__finished;
            other.__finished = true;
        }
        return *this;
    }

    void update(uint64_t current) {
        __current = current;
        __timestamp_ns = now_ns();
        __push();
    }

    void tick() {
        update(__current + 1);
    }

    void finish() {
        __current = __total;
        __timestamp_ns = now_ns();
        if (!__finished) {
            __push();
            __finished = true;
        }
    }

private:
    void __push() {
        ProgressRecord record{};
        record.id = __id;
        record.label = __label;
        record.current = __current;
        record.total = __total;
        record.timestamp_ns = __timestamp_ns;
        detail::finalize(record);
        __buffer->push(std::move(record));
    }

    ProgressBuffer* __buffer;  // non-owning
    std::string __label;
    uint64_t __id = 0;
    uint64_t __current = 0;
    uint64_t __total = 0;
    uint64_t __timestamp_ns = 0;
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

// Written as `if constexpr (!enabled) {} else expr` so that a user's
// `if (c) LOG_INFO("x"); else ...` binds the `else` to their own `if`.
#define LUMEN_IF_ENABLED(LEVEL, expr) \
    if constexpr (!(LUMEN_LEVEL_##LEVEL >= LUMEN_MIN_LEVEL_INT)) {} else expr

#define LUMEN_LOG_AT(LEVEL, ...) \
    LUMEN_IF_ENABLED(LEVEL, \
        lumen::RecordBuilder(lumen::core().log_buffer(), lumen::LogLevel::LEVEL, \
                             lumen::detail::format_message(__VA_ARGS__)))

// ── Log macros ────────────────────────────────────────────────────────────────
//
//   LOG_INFO("plain message");                 // logged verbatim
//   LOG_INFO("energy {:.2f} GeV", energy);     // std::format, checked at compile time
//   LOG_INFO("done").tag("n", int64_t{42});    // chain tags

#define LOG_TRACE(...) LUMEN_LOG_AT(TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) LUMEN_LOG_AT(DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  LUMEN_LOG_AT(INFO, __VA_ARGS__)
#define LOG_WARN(...)  LUMEN_LOG_AT(WARN, __VA_ARGS__)
#define LOG_ERROR(...) LUMEN_LOG_AT(ERROR, __VA_ARGS__)

#define LOG_FATAL(...) do { \
    { \
        lumen::RecordBuilder LUMEN_CONCAT(__lumen_fatal_, __LINE__) \
            (lumen::core().log_buffer(), lumen::LogLevel::FATAL, \
             lumen::detail::format_message(__VA_ARGS__)); \
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
