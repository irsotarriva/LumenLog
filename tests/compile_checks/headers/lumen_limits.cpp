// lumen/limits.hpp alone declares what it documents.
#include "lumen/limits.hpp"

#include <cstddef>
#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<decltype(max_record_tags), const std::size_t>);
static_assert(std::is_same_v<decltype(max_process_tags), const std::size_t>);
static_assert(std::is_same_v<decltype(max_thread_tags), const std::size_t>);
static_assert(std::is_same_v<decltype(max_instance_tags), const std::size_t>);
static_assert(std::is_same_v<decltype(log_queue_capacity), const std::size_t>);
static_assert(std::is_same_v<decltype(metric_queue_capacity), const std::size_t>);
static_assert(std::is_same_v<decltype(progress_queue_capacity), const std::size_t>);

}  // namespace lumen
