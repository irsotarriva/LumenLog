#include "lumen/detail/context.h"

namespace lumen {
namespace detail {

namespace {

void add_tags_to_record(auto& record,
                        const TagSet<16>& process_tags,
                        const TagSet<8>& thread_tags,
                        const TagSet<8>* scope_tags,
                        const TagSet<8>* instance_tags) {
    for (size_t i = 0; i < process_tags.count(); ++i) {
        record.tags.add(process_tags.begin()[i].key, process_tags.begin()[i].value);
    }
    for (size_t i = 0; i < thread_tags.count(); ++i) {
        record.tags.add(thread_tags.begin()[i].key, thread_tags.begin()[i].value);
    }
    if (scope_tags) {
        for (size_t i = 0; i < scope_tags->count(); ++i) {
            record.tags.add(scope_tags->begin()[i].key, scope_tags->begin()[i].value);
        }
    }
    if (instance_tags) {
        for (size_t i = 0; i < instance_tags->count(); ++i) {
            record.tags.add(instance_tags->begin()[i].key, instance_tags->begin()[i].value);
        }
    }
}

}  // namespace

void merge_context(LogRecord& record,
                   const TagSet<16>& process_tags,
                   const TagSet<8>& thread_tags,
                   const TagSet<8>* scope_tags,
                   const TagSet<8>* instance_tags) {
    add_tags_to_record(record, process_tags, thread_tags, scope_tags, instance_tags);
}

void merge_context(MetricRecord& record,
                   const TagSet<16>& process_tags,
                   const TagSet<8>& thread_tags,
                   const TagSet<8>* scope_tags,
                   const TagSet<8>* instance_tags) {
    add_tags_to_record(record, process_tags, thread_tags, scope_tags, instance_tags);
}

void merge_context(ProgressRecord& record,
                   const TagSet<16>& process_tags,
                   const TagSet<8>& thread_tags,
                   const TagSet<8>* scope_tags,
                   const TagSet<8>* instance_tags) {
    add_tags_to_record(record, process_tags, thread_tags, scope_tags, instance_tags);
}

}  // namespace detail
}  // namespace lumen
