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

    LogBuffer&       log_buffer()       { return __log_buffer; }
    MetricBuffer&    metric_buffer()    { return __metric_buffer; }
    ProgressBuffer&  progress_buffer()  { return __progress_buffer; }

    Core();
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core&&) = delete;

private:
    void __dispatch_loop();
    void __drain_available();
    void __drain_until(uint64_t log_target, uint64_t metric_target, uint64_t progress_target);
    void __flush_sinks();
    void __dispatch_log(const LogRecord& record);
    void __dispatch_metric(const MetricRecord& record);
    void __dispatch_progress(const ProgressRecord& record);

    LogBuffer                     __log_buffer;
    MetricBuffer                  __metric_buffer;
    ProgressBuffer                __progress_buffer;

    std::jthread                  __dispatch_thread;
    std::mutex                    __wake_mutex;
    std::condition_variable       __wake_cv;
    std::atomic<bool>             __running{true};
    std::mutex                    __consume_mutex;  // single-consumer guard for the ring buffers

    std::mutex                    __sink_mutex;
    std::vector<SinkEntry>        __sinks;
    std::atomic<SinkId>           __next_sink_id{1};
};

Core& core();

void set_process_tag(std::string_view key, std::string_view value);
void set_thread_tag(std::string_view key, std::string_view value);

MetricBuilder metric(std::string_view name, double value);
ProgressHandle progress(std::string_view label, uint64_t total);

}  // namespace lumen

#endif
