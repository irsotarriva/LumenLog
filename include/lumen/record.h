#ifndef LUMEN_RECORD_H
#define LUMEN_RECORD_H

#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
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
        if (count_ < N) {
            entries_[count_++] = {key, value};
        }
    }
    // Replaces the value of an existing key, or adds the pair if absent.
    void set(std::string_view key, std::string_view value) {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].key == key) {
                entries_[i].value = value;
                return;
            }
        }
        add(key, value);
    }
    [[nodiscard]] std::string_view find(std::string_view key) const {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].key == key) return entries_[i].value;
        }
        return {};
    }
    [[nodiscard]] bool contains(std::string_view key) const {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].key == key) return true;
        }
        return false;
    }
    // Re-points every key and value through `f` (used to move them into owned storage).
    template <typename F>
    void remap(F&& f) {
        for (size_t i = 0; i < count_; ++i) {
            entries_[i].key = f(entries_[i].key);
            entries_[i].value = f(entries_[i].value);
        }
    }
    [[nodiscard]] size_t count() const { return count_; }
    [[nodiscard]] const Entry* begin() const { return entries_.data(); }
    [[nodiscard]] const Entry* end() const { return entries_.data() + count_; }

private:
    std::array<Entry, N> entries_{};
    size_t count_ = 0;
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
    static thread_local uint32_t cached_id_ = 0;
    if (cached_id_ == 0) {
        cached_id_ = static_cast<uint32_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFFFFu);
    }
    return cached_id_;
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
        if (s.size() <= INLINE_CAPACITY - used_) {
            char* dst = inline_buf_.data() + used_;
            if (!s.empty()) std::memcpy(dst, s.data(), s.size());
            used_ += s.size();
            return {dst, s.size()};
        }
        overflow_.emplace_front(s);
        return overflow_.front();
    }

    // Integers exactly; floating point as the shortest text that parses back
    // to the same value, so numeric predicates see the value that was logged.
    template <typename T>
        requires(std::integral<T> || std::floating_point<T>)
    [[nodiscard]] std::string_view store_number(T value) {
        std::array<char, 32> buf{};
        const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
        if (ec != std::errc{}) return {};
        return store(std::string_view(buf.data(), static_cast<size_t>(end - buf.data())));
    }

private:
    std::array<char, INLINE_CAPACITY> inline_buf_;
    size_t used_ = 0;
    std::forward_list<std::string> overflow_;
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
        : buffer_(buffer) {
        record_.level = level;
        record_.message = scratch_.store(message);
        record_.timestamp_ns = now_ns();
        record_.thread_id = this_thread_id();
        record_.source.file = loc.file_name();
        record_.source.function = loc.function_name();
        record_.source.class_name = detail::extract_class_name(loc.function_name());
        record_.source.line = loc.line();
    }

    ~RecordBuilder() {
        detail::finalize(record_);
        buffer_.push(std::move(record_));
    }

    RecordBuilder(const RecordBuilder&) = delete;
    RecordBuilder& operator=(const RecordBuilder&) = delete;

    RecordBuilder& tag(std::string_view key, std::string_view value) {
        record_.tags.add(scratch_.store(key), scratch_.store(value));
        return *this;
    }
    // Any integer or floating-point type (a plain `int` would otherwise be
    // ambiguous between int64_t and double). bool is logged as a string instead.
    template <typename T>
        requires((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>)
    RecordBuilder& tag(std::string_view key, T value) {
        const std::string_view text = scratch_.store_number(value);
        if (!text.empty()) record_.tags.add(scratch_.store(key), text);
        return *this;
    }
    // A template so that string literals (const char* -> bool is a standard
    // conversion) still pick the string_view overload.
    template <std::same_as<bool> T>
    RecordBuilder& tag(std::string_view key, T value) {
        return tag(key, std::string_view(value ? "true" : "false"));
    }

private:
    LogBuffer& buffer_;
    LogRecord record_{};
    detail::StringScratch scratch_;
};

// ── MetricBuilder ─────────────────────────────────────────────────────────────

class MetricBuilder {
public:
    MetricBuilder(MetricBuffer& buffer, std::string_view name, double value)
        : buffer_(buffer) {
        record_.name = scratch_.store(name);
        record_.value = value;
        record_.timestamp_ns = now_ns();
    }

    ~MetricBuilder() {
        detail::finalize(record_);
        buffer_.push(std::move(record_));
    }

    MetricBuilder(const MetricBuilder&) = delete;
    MetricBuilder& operator=(const MetricBuilder&) = delete;

    MetricBuilder& tag(std::string_view key, std::string_view value) {
        record_.tags.add(scratch_.store(key), scratch_.store(value));
        return *this;
    }
    // Any integer or floating-point type (a plain `int` would otherwise be
    // ambiguous between int64_t and double). bool is logged as a string instead.
    template <typename T>
        requires((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>)
    MetricBuilder& tag(std::string_view key, T value) {
        const std::string_view text = scratch_.store_number(value);
        if (!text.empty()) record_.tags.add(scratch_.store(key), text);
        return *this;
    }
    // A template so that string literals (const char* -> bool is a standard
    // conversion) still pick the string_view overload.
    template <std::same_as<bool> T>
    MetricBuilder& tag(std::string_view key, T value) {
        return tag(key, std::string_view(value ? "true" : "false"));
    }

private:
    MetricBuffer& buffer_;
    MetricRecord record_{};
    detail::StringScratch scratch_;
};

// ── ProgressHandle ────────────────────────────────────────────────────────────

class ProgressHandle {
public:
    ProgressHandle(ProgressBuffer& buffer, std::string_view label, uint64_t total)
        : buffer_(&buffer), label_(label), total_(total), timestamp_ns_(now_ns()) {
        static std::atomic<uint64_t> next_id_{1};
        id_ = next_id_.fetch_add(1, std::memory_order_relaxed);
    }

    ~ProgressHandle() {
        if (!finished_) {
            push_();
        }
    }

    ProgressHandle(const ProgressHandle&) = delete;
    ProgressHandle& operator=(const ProgressHandle&) = delete;

    ProgressHandle(ProgressHandle&& other) noexcept
        : buffer_(other.buffer_)
        , label_(std::move(other.label_))
        , id_(other.id_)
        , current_(other.current_)
        , total_(other.total_)
        , timestamp_ns_(other.timestamp_ns_)
        , finished_(other.finished_) {
        other.finished_ = true;
    }
    ProgressHandle& operator=(ProgressHandle&& other) noexcept {
        if (this != &other) {
            if (!finished_) {
                push_();
            }
            buffer_ = other.buffer_;
            label_ = std::move(other.label_);
            id_ = other.id_;
            current_ = other.current_;
            total_ = other.total_;
            timestamp_ns_ = other.timestamp_ns_;
            finished_ = other.finished_;
            other.finished_ = true;
        }
        return *this;
    }

    void update(uint64_t current) {
        current_ = current;
        timestamp_ns_ = now_ns();
        push_();
    }

    void tick() {
        update(current_ + 1);
    }

    void finish() {
        current_ = total_;
        timestamp_ns_ = now_ns();
        if (!finished_) {
            push_();
            finished_ = true;
        }
    }

private:
    void push_() {
        ProgressRecord record{};
        record.id = id_;
        record.label = label_;
        record.current = current_;
        record.total = total_;
        record.timestamp_ns = timestamp_ns_;
        detail::finalize(record);
        buffer_->push(std::move(record));
    }

    ProgressBuffer* buffer_;  // non-owning
    std::string label_;
    uint64_t id_ = 0;
    uint64_t current_ = 0;
    uint64_t total_ = 0;
    uint64_t timestamp_ns_ = 0;
    bool finished_ = false;
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

#ifndef has_cpp_attribute_
#define has_cpp_attribute_(x) 0
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
        lumen::RecordBuilder LUMEN_CONCAT(lumen_fatal_, __LINE__) \
            (lumen::core().log_buffer(), lumen::LogLevel::FATAL, \
             lumen::detail::format_message(__VA_ARGS__)); \
    } \
    lumen::core().flush(); \
    std::terminate(); \
} while(0)

// ── Scope macros ──────────────────────────────────────────────────────────────

#define LUMEN_SCOPE(key, value) \
    lumen::detail::ScopeGuard LUMEN_CONCAT(lumen_scope_, __LINE__)(key, value)

#define LUMEN_FRAME_SCOPE(key, value) \
    lumen::detail::ScopeGuard LUMEN_CONCAT(lumen_frame_, __LINE__)(key, value)

#define LUMEN_MEMBER_SCOPE \
    auto LUMEN_CONCAT(lumen_member_, __LINE__) = lumen_scope()

#define LUMEN_LOOP(label, var, total) \
    for (lumen::detail::LoopScope LUMEN_CONCAT(lumen_loop_, __LINE__)(label, lumen::core().progress_buffer(), static_cast<uint64_t>(total)); \
         LUMEN_CONCAT(lumen_loop_, __LINE__).advance(); ) \
        if (uint64_t var = LUMEN_CONCAT(lumen_loop_, __LINE__).current(); true)

#endif
