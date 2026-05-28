#include "lumen/detail/context.h"

namespace lumen {
namespace detail {

void merge_context(LogRecord& record,
                   const TagSet<16>& process_tags,
                   const TagSet<8>& thread_tags) {
    for (size_t i = 0; i < thread_tags.count(); ++i) {
        record.tags.add(thread_tags.begin()[i].key, thread_tags.begin()[i].value);
    }
    for (size_t i = 0; i < process_tags.count(); ++i) {
        record.tags.add(process_tags.begin()[i].key, process_tags.begin()[i].value);
    }
}

void merge_context(MetricRecord& record,
                   const TagSet<16>& process_tags,
                   const TagSet<8>& thread_tags) {
    for (size_t i = 0; i < thread_tags.count(); ++i) {
        record.tags.add(thread_tags.begin()[i].key, thread_tags.begin()[i].value);
    }
    for (size_t i = 0; i < process_tags.count(); ++i) {
        record.tags.add(process_tags.begin()[i].key, process_tags.begin()[i].value);
    }
}

void merge_context(ProgressRecord& record,
                   const TagSet<16>& process_tags,
                   const TagSet<8>& thread_tags) {
    for (size_t i = 0; i < thread_tags.count(); ++i) {
        record.tags.add(thread_tags.begin()[i].key, thread_tags.begin()[i].value);
    }
    for (size_t i = 0; i < process_tags.count(); ++i) {
        record.tags.add(process_tags.begin()[i].key, process_tags.begin()[i].value);
    }
}

}  // namespace detail
}  // namespace lumen
