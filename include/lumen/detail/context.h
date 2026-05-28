#ifndef LUMEN_DETAIL_CONTEXT_H
#define LUMEN_DETAIL_CONTEXT_H

#include <cstddef>
#include <string_view>

#include "lumen/record.h"

namespace lumen {
namespace detail {

const TagSet<8>& tls_tags();
const TagSet<16>& proc_tags();
const TagSet<8>* current_instance_tags();

void merge_context(LogRecord& record,
                   const TagSet<16>& process_tags,
                   const TagSet<8>& thread_tags,
                   const TagSet<8>* scope_tags,
                   const TagSet<8>* instance_tags);

void merge_context(MetricRecord& record,
                   const TagSet<16>& process_tags,
                   const TagSet<8>& thread_tags,
                   const TagSet<8>* scope_tags,
                   const TagSet<8>* instance_tags);

void merge_context(ProgressRecord& record,
                   const TagSet<16>& process_tags,
                   const TagSet<8>& thread_tags,
                   const TagSet<8>* scope_tags,
                   const TagSet<8>* instance_tags);

}  // namespace detail
}  // namespace lumen

#endif
