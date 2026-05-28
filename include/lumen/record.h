#ifndef LUMEN_RECORD_H
#define LUMEN_RECORD_H

#include <cstdint>
#include <string_view>
#include <array>
#include <source_location>

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
    std::string_view       class_name;  // C++26 reflection; empty otherwise
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

class RecordBuilder {
public:
    RecordBuilder& tag(std::string_view key, std::string_view value);
    RecordBuilder& tag(std::string_view key, int64_t value);
    RecordBuilder& tag(std::string_view key, double value);
};

class MetricBuilder {
public:
    MetricBuilder& tag(std::string_view key, std::string_view value);
    MetricBuilder& tag(std::string_view key, int64_t value);
    MetricBuilder& tag(std::string_view key, double value);
};

class ProgressHandle {
public:
    void update(uint64_t current);
    void tick();
    void finish();
    ~ProgressHandle();
};

}  // namespace lumen

#define LOG_TRACE(fmt, ...)
#define LOG_DEBUG(fmt, ...)
#define LOG_INFO(fmt, ...)
#define LOG_WARN(fmt, ...)
#define LOG_ERROR(fmt, ...)
#define LOG_FATAL(fmt, ...)

#define LUMEN_SCOPE(key, value, ...)
#define LUMEN_LOOP(label, var, total)
#define LUMEN_FRAME_SCOPE(frame_var)
#define LUMEN_MEMBER_SCOPE

#endif
