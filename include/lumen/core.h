#ifndef LUMEN_CORE_H
#define LUMEN_CORE_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
};

class Core {
public:
    void set_process_tag(std::string_view key, std::string_view value);
    void set_thread_tag(std::string_view key, std::string_view value);

    SinkId add_sink(std::unique_ptr<Sink> sink, Predicate predicate);
    std::unique_ptr<Sink> remove_sink(SinkId id);

    void flush();

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
