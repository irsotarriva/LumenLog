#ifndef LUMEN_DETAIL_CONTEXT_HPP
#define LUMEN_DETAIL_CONTEXT_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "lumen/record.hpp"

// The internal side of lumen/context.hpp: how scope guards, Tagged and Core
// reach the ambient context. All context state (the process table and each
// thread's tags and frame lists) is owned by src/context.cpp; nothing else
// holds any.
//
// Frames. Each live LUMEN_SCOPE, LUMEN_LOOP and LUMEN_MEMBER_SCOPE owns one
// frame, a node of an intrusive list headed in the calling thread's context.
// A frame is pushed when its guard is constructed and popped when the guard
// is destroyed, so the lists follow the guards' stack discipline: a frame
// must be popped by the thread that pushed it, after every frame pushed
// after it. Debug builds assert both; release builds unlink the frame
// wherever it is in the list, or ignore it if this thread does not hold it.

namespace lumen {

class Tagged;

namespace detail {

struct OwnedTag {
    std::string key;
    std::string value;
};

// One tag contributed by a LUMEN_SCOPE or LUMEN_LOOP. The views point into
// the guard that owns the frame.
struct ScopeFrame {
    std::string_view key;
    std::string_view value;
    ScopeFrame* next = nullptr;  // non-owning: the enclosing frame on this thread
};

// The tags of one Tagged object, read at the time a record is submitted.
struct InstanceFrame {
    const Tagged* object = nullptr;  // non-owning
    InstanceFrame* next = nullptr;   // non-owning: the enclosing frame on this thread
};

void push_scope(ScopeFrame& frame) noexcept;
void pop_scope(ScopeFrame& frame) noexcept;
void push_instance(InstanceFrame& frame) noexcept;
void pop_instance(InstanceFrame& frame) noexcept;

// The tags of a Tagged object, for the merge (defined in src/tagged.cpp).
[[nodiscard]] std::span<const OwnedTag> instance_tags(const Tagged& object) noexcept;

// Adds the calling thread's ambient context to `tags`, which already holds
// the record's explicit tags: instance, scope, thread, then process tags,
// skipping keys already present. Returns how many tags were left out because
// `tags` was full. Must run on the thread that produces the record.
[[nodiscard]] uint32_t merge_context(RecordTags& tags);

}  // namespace detail
}  // namespace lumen

#endif
