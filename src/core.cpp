#include "lumen/core.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

#include "lumen/detail/context.h"
#include "lumen/detail/error.h"

namespace lumen {

namespace {

struct OwnedTag {
    std::string key;
    std::string value;
};

// Process tags: the authoritative copy lives under a mutex. Each thread keeps
// its own snapshot and refreshes it when the version changes, so emitting a
// record never reads strings another thread may be rewriting.
std::mutex __proc_tag_mutex;
std::array<OwnedTag, 16> __proc_tags;
size_t __proc_tag_count = 0;
std::atomic<uint64_t> __proc_tag_version{1};

struct ProcTagSnapshot {
    uint64_t version = 0;
    std::array<OwnedTag, 16> tags;
    TagSet<16> tagset;
};
thread_local ProcTagSnapshot __proc_snapshot;

thread_local std::array<OwnedTag, 8> __tls_tags;
thread_local size_t __tls_tag_count = 0;
thread_local TagSet<8> __tls_tagset;

// True while this thread is invoking sink callbacks.
thread_local bool __in_dispatch = false;

void rebuild_tls_tagset() {
    __tls_tagset = TagSet<8>{};
    for (size_t i = 0; i < __tls_tag_count; ++i) {
        __tls_tagset.add(__tls_tags[i].key, __tls_tags[i].value);
    }
}

// Runs one sink callback; an escaping exception is counted and reported once
// per sink instead of terminating the dispatch thread or reaching the caller.
void report_sink_exception(SinkEntry& entry, const char* callback, const char* what) {
    if (entry.exceptions++ == 0) {
        std::fprintf(stderr,
                     "lumen: sink %llu threw from %s: %s "
                     "(further exceptions from this sink are only counted)\n",
                     static_cast<unsigned long long>(entry.id), callback, what);
    }
}

// Runs one sink callback; an escaping exception is counted and reported once
// per sink instead of terminating the dispatch thread or reaching the caller.
// Reporting happens inside the handler: e.what() dies with the exception.
template <typename F>
void call_sink(SinkEntry& entry, const char* callback, F&& fn) noexcept {
    try {
        fn();
    } catch (const std::exception& e) {
        report_sink_exception(entry, callback, e.what());
    } catch (...) {
        report_sink_exception(entry, callback, "unknown exception");
    }
}

struct DispatchScope {
    DispatchScope() { __in_dispatch = true; }
    ~DispatchScope() { __in_dispatch = false; }
    DispatchScope(const DispatchScope&) = delete;
    DispatchScope& operator=(const DispatchScope&) = delete;
};

}  // namespace

namespace detail {

const TagSet<8>& tls_tags() {
    return __tls_tagset;
}

const TagSet<16>& proc_tags() {
    const uint64_t version = __proc_tag_version.load(std::memory_order_acquire);
    if (__proc_snapshot.version != version) {
        std::lock_guard lock(__proc_tag_mutex);
        __proc_snapshot.tagset = TagSet<16>{};
        for (size_t i = 0; i < __proc_tag_count; ++i) {
            __proc_snapshot.tags[i] = __proc_tags[i];
            __proc_snapshot.tagset.add(__proc_snapshot.tags[i].key, __proc_snapshot.tags[i].value);
        }
        __proc_snapshot.version = __proc_tag_version.load(std::memory_order_relaxed);
    }
    return __proc_snapshot.tagset;
}

}  // namespace detail

Core::Core() {
    __dispatch_thread = std::jthread(&Core::__dispatch_loop, this);
}

Core::~Core() {
    __running.store(false, std::memory_order_relaxed);
    __wake_cv.notify_one();
    if (__dispatch_thread.joinable()) {
        __dispatch_thread.join();
    }
    // Deliver whatever was still queued when the dispatcher stopped.
    std::lock_guard consume(__consume_mutex);
    __drain_available();
    __flush_sinks();
}

void Core::set_process_tag(std::string_view key, std::string_view value) {
    std::lock_guard lock(__proc_tag_mutex);
    for (size_t i = 0; i < __proc_tag_count; ++i) {
        if (__proc_tags[i].key == key) {
            __proc_tags[i].value = value;
            __proc_tag_version.fetch_add(1, std::memory_order_release);
            return;
        }
    }
    if (__proc_tag_count < __proc_tags.size()) {
        __proc_tags[__proc_tag_count].key = key;
        __proc_tags[__proc_tag_count].value = value;
        ++__proc_tag_count;
        __proc_tag_version.fetch_add(1, std::memory_order_release);
    }
}

void Core::set_thread_tag(std::string_view key, std::string_view value) {
    for (size_t i = 0; i < __tls_tag_count; ++i) {
        if (__tls_tags[i].key == key) {
            __tls_tags[i].value = value;
            rebuild_tls_tagset();
            return;
        }
    }
    if (__tls_tag_count < __tls_tags.size()) {
        __tls_tags[__tls_tag_count].key = key;
        __tls_tags[__tls_tag_count].value = value;
        ++__tls_tag_count;
        rebuild_tls_tagset();
    }
}

SinkId Core::add_sink(std::unique_ptr<Sink> sink, Predicate predicate) {
    SinkId id = __next_sink_id.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(__sink_mutex);
        __sinks.push_back(SinkEntry{id, std::move(sink), std::move(predicate)});
    }
    return id;
}

std::expected<std::unique_ptr<Sink>, std::error_code> Core::remove_sink(SinkId id) {
    std::lock_guard lock(__sink_mutex);
    for (auto it = __sinks.begin(); it != __sinks.end(); ++it) {
        if (it->id == id) {
            auto sink = std::move(it->sink);
            __sinks.erase(it);
            return sink;
        }
    }
    return std::unexpected(make_error_code(LumenError::invalid_sink_id));
}

void Core::flush() {
    if (__in_dispatch) {
        return;  // called from a sink callback; the records are already being drained
    }
    // Everything claimed before this point must be delivered before we return.
    const uint64_t log_target = __log_buffer.head_position();
    const uint64_t metric_target = __metric_buffer.head_position();
    const uint64_t progress_target = __progress_buffer.head_position();

    std::lock_guard consume(__consume_mutex);
    __drain_until(log_target, metric_target, progress_target);
    __flush_sinks();
}

void Core::emit(LogRecord&& record) {
    detail::own_strings(record);
    __log_buffer.push(std::move(record));
    __wake_cv.notify_one();
}

void Core::emit(MetricRecord&& record) {
    detail::own_strings(record);
    __metric_buffer.push(std::move(record));
    __wake_cv.notify_one();
}

void Core::emit(ProgressRecord&& record) {
    detail::own_strings(record);
    __progress_buffer.push(std::move(record));
    __wake_cv.notify_one();
}

void Core::__dispatch_loop() {
    while (__running.load(std::memory_order_relaxed)) {
        {
            std::unique_lock lock(__wake_mutex);
            __wake_cv.wait_for(lock, std::chrono::milliseconds(10));
        }
        std::lock_guard consume(__consume_mutex);
        __drain_available();
    }
}

// Caller holds __consume_mutex.
void Core::__drain_available() {
    const DispatchScope in_dispatch;

    ProgressRecord pr;
    while (__progress_buffer.pop(pr)) {
        __dispatch_progress(pr);
    }

    MetricRecord mr;
    while (__metric_buffer.pop(mr)) {
        __dispatch_metric(mr);
    }

    LogRecord lr;
    while (__log_buffer.pop(lr)) {
        __dispatch_log(lr);
    }
}

// Caller holds __consume_mutex. A producer may have claimed a slot but not yet
// published it; in that case pop() fails and we wait for the publish.
void Core::__drain_until(uint64_t log_target, uint64_t metric_target,
                         uint64_t progress_target) {
    while (true) {
        __drain_available();
        if (__progress_buffer.tail_position() >= progress_target &&
            __metric_buffer.tail_position() >= metric_target &&
            __log_buffer.tail_position() >= log_target) {
            return;
        }
        std::this_thread::yield();
    }
}

void Core::__flush_sinks() {
    const DispatchScope in_dispatch;
    std::lock_guard lock(__sink_mutex);
    for (auto& entry : __sinks) {
        call_sink(entry, "flush", [&] { entry.sink->flush(); });
    }
}

std::expected<uint64_t, std::error_code> Core::sink_exception_count(SinkId id) {
    std::lock_guard lock(__sink_mutex);
    for (const auto& entry : __sinks) {
        if (entry.id == id) {
            return entry.exceptions;
        }
    }
    return std::unexpected(make_error_code(LumenError::invalid_sink_id));
}

void Core::__dispatch_log(const LogRecord& record) {
    std::lock_guard lock(__sink_mutex);
    for (auto& entry : __sinks) {
        if (entry.predicate.evaluate(record.tags, record.level)) {
            call_sink(entry, "on_log", [&] { entry.sink->on_log(record); });
        }
    }
}

void Core::__dispatch_metric(const MetricRecord& record) {
    std::lock_guard lock(__sink_mutex);
    for (auto& entry : __sinks) {
        if (entry.predicate.evaluate(record.tags, LogLevel::TRACE)) {
            call_sink(entry, "on_metric", [&] { entry.sink->on_metric(record); });
        }
    }
}

void Core::__dispatch_progress(const ProgressRecord& record) {
    std::lock_guard lock(__sink_mutex);
    for (auto& entry : __sinks) {
        if (entry.predicate.evaluate(record.tags, LogLevel::TRACE)) {
            call_sink(entry, "on_progress", [&] { entry.sink->on_progress(record); });
        }
    }
}

Core& core() {
    static Core instance;
    return instance;
}

void set_process_tag(std::string_view key, std::string_view value) {
    core().set_process_tag(key, value);
}

void set_thread_tag(std::string_view key, std::string_view value) {
    core().set_thread_tag(key, value);
}

MetricBuilder metric(std::string_view name, double value) {
    return MetricBuilder(core().metric_buffer(), name, value);
}

ProgressHandle progress(std::string_view label, uint64_t total) {
    return ProgressHandle(core().progress_buffer(), label, total);
}

}  // namespace lumen
