// lumen/writer_statistics.hpp alone declares what it documents.
#include "lumen/writer_statistics.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<decltype(WriterStatistics{}.lines_queued), uint64_t>);
static_assert(std::is_same_v<decltype(WriterStatistics{}.lines_written), uint64_t>);
static_assert(std::is_same_v<decltype(WriterStatistics{}.lines_dropped), uint64_t>);
static_assert(std::is_same_v<decltype(WriterStatistics{}.lines_failed), uint64_t>);

}  // namespace lumen
