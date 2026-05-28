#include "lumen/core.h"

#include <array>
#include <cstddef>
#include <string>

#include "lumen/detail/context.h"
#include "lumen/detail/error.h"

namespace lumen {

namespace {

struct OwnedTag {
    std::string key;
    std::string value;
};

std::array<OwnedTag, 16> __proc_tags;
size_t __proc_tag_count = 0;
std::mutex __proc_tag_mutex;

thread_local std::array<OwnedTag, 8> __tls_tags;
thread_local size_t __tls_tag_count = 0;

thread_local TagSet<8> __tls_tagset;
TagSet<16> __proc_tagset;

void rebuild_proc_tagset() {
    __proc_tagset = TagSet<16>{};
    for (size_t i = 0; i < __proc_tag_count; ++i) {
        __proc_tagset.add(__proc_tags[i].key, __proc_tags[i].value);
    }
}

void rebuild_tls_tagset() {
    __tls_tagset = TagSet<8>{};
    for (size_t i = 0; i < __tls_tag_count; ++i) {
        __tls_tagset.add(__tls_tags[i].key, __tls_tags[i].value);
    }
}

}  // namespace

namespace detail {

const TagSet<8>& tls_tags() {
    return __tls_tagset;
}

const TagSet<16>& proc_tags() {
    return __proc_tagset;
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
}

void Core::set_process_tag(std::string_view key, std::string_view value) {
    std::lock_guard lock(__proc_tag_mutex);
    for (size_t i = 0; i < __proc_tag_count; ++i) {
        if (__proc_tags[i].key == key) {
            __proc_tags[i].value = value;
            rebuild_proc_tagset();
            return;
        }
    }
    if (__proc_tag_count < 16) {
        __proc_tags[__proc_tag_count].key = key;
        __proc_tags[__proc_tag_count].value = value;
        ++__proc_tag_count;
        rebuild_proc_tagset();
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
    if (__tls_tag_count < 8) {
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
    __wake_cv.notify_one();
}

void Core::emit(LogRecord&& record) {
    __log_buffer.push(std::move(record));
    __wake_cv.notify_one();
}

void Core::emit(MetricRecord&& record) {
    __metric_buffer.push(std::move(record));
    __wake_cv.notify_one();
}

void Core::emit(ProgressRecord&& record) {
    __progress_buffer.push(std::move(record));
    __wake_cv.notify_one();
}

void Core::__dispatch_loop() {
    while (__running.load(std::memory_order_relaxed)) {
        {
            std::unique_lock lock(__wake_mutex);
            __wake_cv.wait_for(lock, std::chrono::milliseconds(10));
        }

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
}

void Core::__dispatch_log(const LogRecord& record) {
    std::lock_guard lock(__sink_mutex);
    for (auto& entry : __sinks) {
        if (entry.predicate.evaluate(record.tags, record.level)) {
            entry.sink->on_log(record);
        }
    }
}

void Core::__dispatch_metric(const MetricRecord& record) {
    std::lock_guard lock(__sink_mutex);
    for (auto& entry : __sinks) {
        if (entry.predicate.evaluate(record.tags, LogLevel::TRACE)) {
            entry.sink->on_metric(record);
        }
    }
}

void Core::__dispatch_progress(const ProgressRecord& record) {
    std::lock_guard lock(__sink_mutex);
    for (auto& entry : __sinks) {
        if (entry.predicate.evaluate(record.tags, LogLevel::TRACE)) {
            entry.sink->on_progress(record);
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
