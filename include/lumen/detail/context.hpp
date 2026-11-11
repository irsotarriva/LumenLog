#ifndef LUMEN_DETAIL_CONTEXT_HPP
#define LUMEN_DETAIL_CONTEXT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "lumen/detail/tag_table.hpp"
#include "lumen/record.hpp"
#include "lumen/tag_set.hpp"

// The internal side of lumen/context.hpp: how scope guards, Tagged and Core
// reach the ambient context. All context state (the process table and each
// thread's tags and frame stacks) is owned by src/context.cpp; nothing else
// holds any.
//
// Frames. Each live LUMEN_SCOPE, LUMEN_LOOP and LUMEN_MEMBER_SCOPE has one
// frame on a stack in the calling thread's context. The stack holds copies
// of the scope's key and value (and, for an instance frame, a pointer to the
// Tagged object), never pointers into the guard, so nothing a guard does can
// leave the stack pointing at freed memory. A guard keeps the FrameToken that
// push_*() returned and hands it back to pop_*().
//
// Stack discipline. A frame must be popped by the thread that pushed it,
// after every frame pushed after it. Debug builds assert both. Release
// builds:
//   - popping a frame on its own thread also pops every frame above it;
//   - popping on another thread changes nothing: the frame stays on its own
//     thread's stack (its tag stays on that thread's records) until that
//     thread pops a frame below it, or exits. Its strings are copies, so this
//     is wrong context, not a dangling read. An instance frame left this way
//     still points at its object, which must live until then.
//
// Thread teardown. A thread's context is destroyed with the thread's other
// thread_local objects, in an order the program does not control; a log
// statement can still run after it (in the destructor of a thread_local, or
// on the main thread in the destructor of a static object). From then on
// push_*() pushes nothing and returns an empty token, and merge_context()
// adds only the process tags, which it reads from the process table itself.

namespace lumen {

class Tagged;

namespace detail {

// Where a frame sits: the thread context it was pushed on and its depth
// there. A default-constructed token stands for a frame that was not pushed.
struct FrameToken {
    uint64_t context = 0;  // serial number of the thread context; 0: not pushed
    std::size_t depth = 0;
};

// Pushes a scope frame key=value (both copied) on the calling thread.
[[nodiscard]] FrameToken push_scope(std::string_view key, std::string_view value);
void pop_scope(const FrameToken& token) noexcept;

// Pushes `object`'s tags (read at each merge, not copied) on the calling thread.
[[nodiscard]] FrameToken push_instance(const Tagged& object);
void pop_instance(const FrameToken& token) noexcept;

// The tags of a Tagged object, for the merge (defined in src/tagged.cpp).
[[nodiscard]] std::span<const OwnedTag> instance_tags(const Tagged& object) noexcept;

// Keeps alive the strings merge_context() added that do not belong to the
// calling thread's own context (the process tags, read after the thread's
// context was destroyed). The caller keeps it until it has copied the
// record's strings.
class ContextPin {
public:
    ContextPin() = default;

private:
    friend uint32_t merge_context(RecordTags& tags, ContextPin& pin);
    std::shared_ptr<const TagTable> process_tags_;
};

// Adds the calling thread's ambient context to `tags`, which already holds
// the record's explicit tags: instance, scope, thread, then process tags,
// innermost first within a layer, skipping keys already present. Returns how
// many tags were left out because `tags` was full. Must run on the thread
// that produces the record. The added views stay valid while `pin` lives and
// the calling thread does not change its context.
[[nodiscard]] uint32_t merge_context(RecordTags& tags, ContextPin& pin);

}  // namespace detail
}  // namespace lumen

#endif
