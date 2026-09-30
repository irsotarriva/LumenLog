#ifndef LUMEN_CORE_H
#define LUMEN_CORE_H

#include <atomic>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "lumen/record.h"
#include "lumen/predicate.h"
#include "lumen/sink.h"

namespace lumen {

using SinkId = uint64_t;

struct SinkEntry {
    SinkId                    id;
    std::unique_ptr<Sink>     sink;
    Predicate                 predicate;
    uint64_t                  exceptions = 0;  // thrown from this sink's callbacks
};

class Core {
public:
    void set_process_tag(std::string_view key, std::string_view value);
    void set_thread_tag(std::string_view key, std::string_view value);

    [[nodiscard]] SinkId add_sink(std::unique_ptr<Sink> sink, Predicate predicate);
    [[nodiscard]] std::expected<std::unique_ptr<Sink>, std::error_code> remove_sink(SinkId id);

    // Blocks until every record pushed before the call has been dispatched to
    // the sinks and each sink's flush() has returned (i.e. its pending I/O is
    // written). Records are drained on the calling thread, serialised with the
    // dispatch thread, so sinks are never called concurrently. Calling flush()
    // from inside a sink callback is a no-op.
    void flush();

    // Sinks should not throw, but an exception escaping a sink's on_* or
    // flush() is caught at this boundary: it is counted, the first one per
    // sink is reported on stderr, and dispatch continues with the next sink.
    // Nothing propagates into LOG_*, flush() or the dispatch thread.
    [[nodiscard]] std::expected<uint64_t, std::error_code> sink_exception_count(SinkId id);

    void emit(LogRecord&& record);
    void emit(MetricRecord&& record);
    void emit(ProgressRecord&& record);

    LogBuffer&       log_buffer()       { return log_buffer_; }
    MetricBuffer&    metric_buffer()    { return metric_buffer_; }
    ProgressBuffer&  progress_buffer()  { return progress_buffer_; }

    Core();
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core&&) = delete;

private:
    void dispatch_loop_();
    void drain_available_();
    void drain_until_(uint64_t log_target, uint64_t metric_target, uint64_t progress_target);
    void flush_sinks_();
    void dispatch_log_(const LogRecord& record);
    void dispatch_metric_(const MetricRecord& record);
    void dispatch_progress_(const ProgressRecord& record);

    LogBuffer                     log_buffer_;
    MetricBuffer                  metric_buffer_;
    ProgressBuffer                progress_buffer_;

    std::thread                   dispatch_thread_;
    std::mutex                    wake_mutex_;
    std::condition_variable       wake_cv_;
    std::atomic<bool>             running_{true};
    std::mutex                    consume_mutex_;  // single-consumer guard for the ring buffers

    std::mutex                    sink_mutex_;
    std::vector<SinkEntry>        sinks_;
    std::atomic<SinkId>           next_sink_id_{1};
};

Core& core();

void set_process_tag(std::string_view key, std::string_view value);
void set_thread_tag(std::string_view key, std::string_view value);

MetricBuilder metric(std::string_view name, double value);
ProgressHandle progress(std::string_view label, uint64_t total);

}  // namespace lumen

#endif
