#ifndef LUMEN_CONTEXT_HPP
#define LUMEN_CONTEXT_HPP

#include <expected>
#include <string_view>
#include <system_error>

// Ambient context: tags that Core adds to every record without the logging
// statement naming them, in four layers (2-5 below) under the record's own
// explicit tags (1). When a record is submitted,
// Core keeps the record's own (explicit) tags and then adds each layer in this
// order, skipping keys the record already has, until the record holds
// max_record_tags tags (lumen/limits.hpp):
//
//   1. explicit   .tag(k, v) on the logging statement
//   2. instance   lumen::Tagged objects whose LUMEN_MEMBER_SCOPE is active on
//                 this thread, innermost first          (lumen/tagged.hpp)
//   3. scope      LUMEN_SCOPE / LUMEN_LOOP active on this thread,
//                 innermost first                       (lumen/macros.hpp)
//   4. thread     set_thread_tag() on this thread
//   5. process    set_process_tag() from any thread
//
// So the first layer to name a key wins, and when the record is full the
// lowest layers lose. Each ambient tag that is left out only because the
// record is full is counted in Core::statistics().tags_dropped.
//
// Within one layer a key is held once: setting it again replaces its value.
//
// Context is ambient, not per Core: the process layer is one table for the
// whole program, and the other layers belong to the calling thread. Every
// Core (the global lumen::core() and any other) reads the same context.
//
// Threads. Every function here may be called from any thread. The process
// table is guarded by a mutex; each producing thread keeps a reference to the
// version it last read and re-reads it only after a change, so logging takes
// no lock unless the process tags changed since that thread's previous
// record. Thread tags are visible only to records produced by the thread that
// set them.
//
// Lifetime. Keys and values are copied. The process table lives until the
// program ends: it is never destroyed. A thread's context (its tags and its
// scopes) is destroyed together with the thread's thread_local objects, in an
// order the program does not control, so a log statement can run after it:
// in the destructor of a thread_local, or on the main thread in the
// destructor of a static object. Such a record is submitted like any other,
// with its explicit tags and the process tags; the thread's tags and scopes
// are no longer added. From then on set_thread_tag() fails with
// std::errc::operation_not_permitted, remove_thread_tag() returns false and
// LUMEN_SCOPE adds nothing.

namespace lumen {

// Sets `key` to `value` in the process layer, replacing an existing value.
// Fails with LumenError::TagTableFull when `key` is new and the table already
// holds max_process_tags keys; the table is then unchanged.
[[nodiscard]] std::expected<void, std::error_code> set_process_tag(std::string_view key,
                                                                   std::string_view value);

// Removes `key` from the process layer. Returns false if it was not there.
[[nodiscard]] bool remove_process_tag(std::string_view key);

// Removes every process tag.
void clear_process_tags();

// As set_process_tag, for the calling thread's layer (max_thread_tags keys).
// Also fails with std::errc::operation_not_permitted once the calling
// thread's context has been destroyed (see Lifetime above).
[[nodiscard]] std::expected<void, std::error_code> set_thread_tag(std::string_view key,
                                                                  std::string_view value);

// Removes `key` from the calling thread's layer. Returns false if it was not there.
[[nodiscard]] bool remove_thread_tag(std::string_view key);

// Removes every tag of the calling thread's layer.
void clear_thread_tags();

}  // namespace lumen

#endif
