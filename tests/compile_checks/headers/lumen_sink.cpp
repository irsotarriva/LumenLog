// lumen/sink.hpp alone declares what it documents.
#include "lumen/sink.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_abstract_v<Sink>);
static_assert(std::has_virtual_destructor_v<Sink>);
static_assert(std::is_same_v<decltype(&Sink::on_log), void (Sink::*)(const LogRecord&)>);
static_assert(std::is_same_v<decltype(&Sink::on_metric), void (Sink::*)(const MetricRecord&)>);
static_assert(std::is_same_v<decltype(&Sink::on_progress), void (Sink::*)(const ProgressRecord&)>);
static_assert(std::is_same_v<decltype(&Sink::flush), void (Sink::*)()>);
static_assert(std::is_base_of_v<Sink, NullSink> && std::is_final_v<NullSink>);

}  // namespace lumen
