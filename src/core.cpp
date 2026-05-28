#include "lumen/core.h"

namespace lumen {

Core::Core() = default;
Core::~Core() = default;

void Core::set_process_tag(std::string_view /*key*/, std::string_view /*value*/) {}
void Core::set_thread_tag(std::string_view /*key*/, std::string_view /*value*/) {}

SinkId Core::add_sink(std::unique_ptr<Sink> /*sink*/, Predicate /*predicate*/) { return 0; }
std::unique_ptr<Sink> Core::remove_sink(SinkId /*id*/) { return nullptr; }

void Core::flush() {}

void Core::emit(LogRecord&& record) {
    __log_buffer.push(std::move(record));
}

void Core::emit(MetricRecord&& record) {
    __metric_buffer.push(std::move(record));
}

void Core::emit(ProgressRecord&& record) {
    __progress_buffer.push(std::move(record));
}

Core& core() {
    static Core instance;
    return instance;
}

MetricBuilder metric(std::string_view name, double value) {
    return MetricBuilder(core().metric_buffer(), name, value);
}

ProgressHandle progress(std::string_view label, uint64_t total) {
    return ProgressHandle(core().progress_buffer(), label, total);
}

}  // namespace lumen
