#include "lumen/detail/context.h"

#include <cstring>

#include "lumen/detail/scope_stack.h"

namespace lumen {
namespace detail {

namespace {

// Adds every entry of `src` whose key is not already on the record. Callers go
// from highest to lowest priority, so the first layer to set a key wins.
template <size_t N, size_t M>
void add_missing(TagSet<N>& dst, const TagSet<M>& src) {
    for (const auto& e : src) {
        if (!dst.contains(e.key)) {
            dst.add(e.key, e.value);
        }
    }
}

template <typename Record>
void merge_ambient_context(Record& record) {
    if (const TagSet<8>* instance = current_instance_tags()) {
        add_missing(record.tags, *instance);
    }
    for (const ScopeFrame* f = tls_scope_head; f; f = f->next) {
        add_missing(record.tags, f->tags);
    }
    add_missing(record.tags, tls_tags());
    add_missing(record.tags, proc_tags());
}

// Copies `text` and every tag string into one NUL-terminated block and
// re-points the views at it.
template <size_t N>
void own_all(std::string_view& text, TagSet<N>& tags, RecordStorage& storage) {
    size_t total = text.size() + 1;
    for (const auto& e : tags) {
        total += e.key.size() + 1 + e.value.size() + 1;
    }

    auto block = std::make_shared_for_overwrite<char[]>(total);
    char* out = block.get();
    auto copy = [&out](std::string_view s) {
        if (!s.empty()) std::memcpy(out, s.data(), s.size());
        out[s.size()] = '\0';
        const std::string_view owned(out, s.size());
        out += s.size() + 1;
        return owned;
    };

    text = copy(text);
    tags.remap(copy);
    storage = std::move(block);
}

}  // namespace

void own_strings(LogRecord& record) {
    own_all(record.message, record.tags, record.storage);
}

void own_strings(MetricRecord& record) {
    own_all(record.name, record.tags, record.storage);
}

void own_strings(ProgressRecord& record) {
    own_all(record.label, record.tags, record.storage);
}

void finalize(LogRecord& record) {
    merge_ambient_context(record);
    own_strings(record);
}

void finalize(MetricRecord& record) {
    merge_ambient_context(record);
    own_strings(record);
}

void finalize(ProgressRecord& record) {
    merge_ambient_context(record);
    own_strings(record);
}

}  // namespace detail
}  // namespace lumen
