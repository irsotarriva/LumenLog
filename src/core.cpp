#include "lumen/core.h"

namespace lumen {

Core::Core() {}
Core::~Core() = default;

void Core::set_process_tag(std::string_view /*key*/, std::string_view /*value*/) {}
void Core::set_thread_tag(std::string_view /*key*/, std::string_view /*value*/) {}

SinkId Core::add_sink(std::unique_ptr<Sink> /*sink*/, Predicate /*predicate*/) { return 0; }
std::unique_ptr<Sink> Core::remove_sink(SinkId /*id*/) { return nullptr; }

void Core::flush() {}

void Core::emit(LogRecord&& /*record*/) {}
void Core::emit(MetricRecord&& /*record*/) {}
void Core::emit(ProgressRecord&& /*record*/) {}

Core& core() {
    static Core instance;
    return instance;
}

}  // namespace lumen
