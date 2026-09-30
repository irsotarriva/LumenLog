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
std::mutex proc_tag_mutex_;
std::array<OwnedTag, 16> proc_tags_;
size_t proc_tag_count_ = 0;
std::atomic<uint64_t> proc_tag_version_{1};

struct ProcTagSnapshot {
    uint64_t version = 0;
    std::array<OwnedTag, 16> tags;
    TagSet<16> tagset;
};
thread_local ProcTagSnapshot proc_snapshot_;

thread_local std::array<OwnedTag, 8> tls_tags_;
thread_local size_t tls_tag_count_ = 0;
thread_local TagSet<8> tls_tagset_;

// True while this thread is invoking sink callbacks.
thread_local bool in_dispatch_ = false;

void rebuild_tls_tagset() {
    tls_tagset_ = TagSet<8>{};
    for (size_t i = 0; i < tls_tag_count_; ++i) {
        tls_tagset_.add(tls_tags_[i].key, tls_tags_[i].value);
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
    DispatchScope() { in_dispatch_ = true; }
    ~DispatchScope() { in_dispatch_ = false; }
    DispatchScope(const DispatchScope&) = delete;
    DispatchScope& operator=(const DispatchScope&) = delete;
};

}  // namespace

namespace detail {

const TagSet<8>& tls_tags() {
    return tls_tagset_;
}

const TagSet<16>& proc_tags() {
    const uint64_t version = proc_tag_version_.load(std::memory_order_acquire);
    if (proc_snapshot_.version != version) {
        std::lock_guard lock(proc_tag_mutex_);
        proc_snapshot_.tagset = TagSet<16>{};
        for (size_t i = 0; i < proc_tag_count_; ++i) {
            proc_snapshot_.tags[i] = proc_tags_[i];
            proc_snapshot_.tagset.add(proc_snapshot_.tags[i].key, proc_snapshot_.tags[i].value);
        }
        proc_snapshot_.version = proc_tag_version_.load(std::memory_order_relaxed);
    }
    return proc_snapshot_.tagset;
}

}  // namespace detail

Core::Core() {
    dispatch_thread_ = std::jthread(&Core::dispatch_loop_, this);
}

Core::~Core() {
    running_.store(false, std::memory_order_relaxed);
    wake_cv_.notify_one();
    if (dispatch_thread_.joinable()) {
        dispatch_thread_.join();
    }
    // Deliver whatever was still queued when the dispatcher stopped.
    std::lock_guard consume(consume_mutex_);
    drain_available_();
    flush_sinks_();
}

void Core::set_process_tag(std::string_view key, std::string_view value) {
    std::lock_guard lock(proc_tag_mutex_);
    for (size_t i = 0; i < proc_tag_count_; ++i) {
        if (proc_tags_[i].key == key) {
            proc_tags_[i].value = value;
            proc_tag_version_.fetch_add(1, std::memory_order_release);
            return;
        }
    }
    if (proc_tag_count_ < proc_tags_.size()) {
        proc_tags_[proc_tag_count_].key = key;
        proc_tags_[proc_tag_count_].value = value;
        ++proc_tag_count_;
        proc_tag_version_.fetch_add(1, std::memory_order_release);
    }
}

void Core::set_thread_tag(std::string_view key, std::string_view value) {
    for (size_t i = 0; i < tls_tag_count_; ++i) {
        if (tls_tags_[i].key == key) {
            tls_tags_[i].value = value;
            rebuild_tls_tagset();
            return;
        }
    }
    if (tls_tag_count_ < tls_tags_.size()) {
        tls_tags_[tls_tag_count_].key = key;
        tls_tags_[tls_tag_count_].value = value;
        ++tls_tag_count_;
        rebuild_tls_tagset();
    }
}

SinkId Core::add_sink(std::unique_ptr<Sink> sink, Predicate predicate) {
    SinkId id = next_sink_id_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(sink_mutex_);
        sinks_.push_back(SinkEntry{id, std::move(sink), std::move(predicate)});
    }
    return id;
}

std::expected<std::unique_ptr<Sink>, std::error_code> Core::remove_sink(SinkId id) {
    std::lock_guard lock(sink_mutex_);
    for (auto it = sinks_.begin(); it != sinks_.end(); ++it) {
        if (it->id == id) {
            auto sink = std::move(it->sink);
            sinks_.erase(it);
            return sink;
        }
    }
    return std::unexpected(make_error_code(LumenError::invalid_sink_id));
}

void Core::flush() {
    if (in_dispatch_) {
        return;  // called from a sink callback; the records are already being drained
    }
    // Everything claimed before this point must be delivered before we return.
    const uint64_t log_target = log_buffer_.head_position();
    const uint64_t metric_target = metric_buffer_.head_position();
    const uint64_t progress_target = progress_buffer_.head_position();

    std::lock_guard consume(consume_mutex_);
    drain_until_(log_target, metric_target, progress_target);
    flush_sinks_();
}

void Core::emit(LogRecord&& record) {
    detail::own_strings(record);
    log_buffer_.push(std::move(record));
    wake_cv_.notify_one();
}

void Core::emit(MetricRecord&& record) {
    detail::own_strings(record);
    metric_buffer_.push(std::move(record));
    wake_cv_.notify_one();
}

void Core::emit(ProgressRecord&& record) {
    detail::own_strings(record);
    progress_buffer_.push(std::move(record));
    wake_cv_.notify_one();
}

void Core::dispatch_loop_() {
    while (running_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(10));
        }
        std::lock_guard consume(consume_mutex_);
        drain_available_();
    }
}

// Caller holds consume_mutex_.
void Core::drain_available_() {
    const DispatchScope in_dispatch;

    ProgressRecord pr;
    while (progress_buffer_.pop(pr)) {
        dispatch_progress_(pr);
    }

    MetricRecord mr;
    while (metric_buffer_.pop(mr)) {
        dispatch_metric_(mr);
    }

    LogRecord lr;
    while (log_buffer_.pop(lr)) {
        dispatch_log_(lr);
    }
}

// Caller holds consume_mutex_. A producer may have claimed a slot but not yet
// published it; in that case pop() fails and we wait for the publish.
void Core::drain_until_(uint64_t log_target, uint64_t metric_target,
                         uint64_t progress_target) {
    while (true) {
        drain_available_();
        if (progress_buffer_.tail_position() >= progress_target &&
            metric_buffer_.tail_position() >= metric_target &&
            log_buffer_.tail_position() >= log_target) {
            return;
        }
        std::this_thread::yield();
    }
}

void Core::flush_sinks_() {
    const DispatchScope in_dispatch;
    std::lock_guard lock(sink_mutex_);
    for (auto& entry : sinks_) {
        call_sink(entry, "flush", [&] { entry.sink->flush(); });
    }
}

std::expected<uint64_t, std::error_code> Core::sink_exception_count(SinkId id) {
    std::lock_guard lock(sink_mutex_);
    for (const auto& entry : sinks_) {
        if (entry.id == id) {
            return entry.exceptions;
        }
    }
    return std::unexpected(make_error_code(LumenError::invalid_sink_id));
}

void Core::dispatch_log_(const LogRecord& record) {
    std::lock_guard lock(sink_mutex_);
    for (auto& entry : sinks_) {
        if (entry.predicate.evaluate(record.tags, record.level)) {
            call_sink(entry, "on_log", [&] { entry.sink->on_log(record); });
        }
    }
}

void Core::dispatch_metric_(const MetricRecord& record) {
    std::lock_guard lock(sink_mutex_);
    for (auto& entry : sinks_) {
        if (entry.predicate.evaluate(record.tags)) {
            call_sink(entry, "on_metric", [&] { entry.sink->on_metric(record); });
        }
    }
}

void Core::dispatch_progress_(const ProgressRecord& record) {
    std::lock_guard lock(sink_mutex_);
    for (auto& entry : sinks_) {
        if (entry.predicate.evaluate(record.tags)) {
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
