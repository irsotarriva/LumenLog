#ifndef LUMEN_CORE_H
#define LUMEN_CORE_H

#include <memory>
#include <string_view>

#include "lumen/record.h"
#include "lumen/predicate.h"
#include "lumen/sink.h"

namespace lumen {

using SinkId = uint64_t;

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
    LogBuffer      __log_buffer;
    MetricBuffer   __metric_buffer;
    ProgressBuffer __progress_buffer;
};

Core& core();

void set_process_tag(std::string_view key, std::string_view value);
void set_thread_tag(std::string_view key, std::string_view value);

MetricBuilder metric(std::string_view name, double value);
ProgressHandle progress(std::string_view label, uint64_t total);

}  // namespace lumen

#endif
