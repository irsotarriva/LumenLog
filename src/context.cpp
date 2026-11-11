#include "lumen/detail/context.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "lumen/context.hpp"
#include "lumen/detail/immortal.hpp"
#include "lumen/detail/tag_table.hpp"
#include "lumen/error.hpp"
#include "lumen/limits.hpp"

namespace lumen {
namespace detail {

std::expected<void, std::error_code> TagTable::set(std::string_view key, std::string_view value) {
    const auto it = std::ranges::find(tags_, key, &OwnedTag::key);
    if (it != tags_.end()) {
        it->value = value;
        return {};
    }
    if (tags_.size() == max_size_) {
        return std::unexpected(make_error_code(LumenError::TagTableFull));
    }
    tags_.push_back(OwnedTag{std::string(key), std::string(value)});
    return {};
}

bool TagTable::remove(std::string_view key) {
    return std::erase_if(tags_, [key](const OwnedTag& tag) { return tag.key == key; }) != 0;
}

namespace {

// The process layer. Its table is copied on write: a change publishes a new
// table, and a thread that still holds the previous one reads it safely.
// `version` grows after every change, so that a thread can tell cheaply
// whether the table it holds is current.
struct ProcessContext {
    std::mutex mutex;
    std::shared_ptr<const TagTable> tags =
        std::make_shared<const TagTable>(max_process_tags);  // guarded by mutex
    std::atomic<uint64_t> version{1};                        // written under mutex, read without it
};

// Never destroyed: a record submitted during static destruction still reads it.
ProcessContext& process_context() {
    static Immortal<ProcessContext> instance;
    return instance.get();
}

[[nodiscard]] std::shared_ptr<const TagTable> process_tags_now(ProcessContext& process) {
    const std::scoped_lock lock(process.mutex);
    return process.tags;
}

// Applies `change` to a copy of the process table and publishes the copy if
// `change` reports that it changed something.
template <typename Change>
    requires std::invocable<Change&, TagTable&>
auto change_process_tags(Change change) {
    ProcessContext& process = process_context();
    const std::scoped_lock lock(process.mutex);
    auto next = std::make_shared<TagTable>(*process.tags);
    auto result = change(*next);
    if (result) {
        process.tags = std::move(next);
        process.version.fetch_add(1, std::memory_order_release);
    }
    return result;
}

[[nodiscard]] uint64_t next_context_serial() {
    // Serials are never reused, so a token from an exited thread never
    // matches a later thread's context.
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// One stack of frames, innermost last. Slots at and above `count` are kept,
// so that pushing again reuses their strings' capacity.
template <typename Entry>
struct FrameStack {
    std::vector<Entry> slots;
    std::size_t count = 0;

    // The slot the next push fills; push() then commits it.
    [[nodiscard]] Entry& next_slot() {
        if (count == slots.size()) {
            slots.emplace_back();
        }
        return slots[count];
    }

    [[nodiscard]] FrameToken push(uint64_t serial) noexcept {
        return FrameToken{.context = serial, .depth = count++};
    }

    // Pops `token`'s frame and every frame above it. A token of another
    // thread's context, or one whose frame was already popped together with
    // a frame below it, changes nothing.
    void pop(const FrameToken& token, uint64_t serial) noexcept {
        assert(token.context == serial && token.depth + 1 == count &&
               "lumen scope guard destroyed out of order or on a thread other than its own");
        if (token.context == serial) {
            count = std::min(count, token.depth);
        }
    }
};

// Everything ambient that belongs to one thread.
struct ThreadContext {
    uint64_t serial = next_context_serial();
    TagTable thread_tags{max_thread_tags};
    FrameStack<OwnedTag> scopes;
    FrameStack<const Tagged*> instances;  // non-owning: objects with a live member scope
    // The process table this thread last read; merging reads it without a lock.
    uint64_t process_version = 0;
    std::shared_ptr<const TagTable> process_tags;
};

// Constructs this thread's context, publishes it in `live_context` and
// withdraws it when the thread's thread_local objects are destroyed.
class ThreadContextOwner;

// Both are trivially destructible, so they can be read at every point of the
// thread's teardown, including after ThreadContextOwner is destroyed. Their
// single writer is ThreadContextOwner.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
thread_local ThreadContext* live_context = nullptr;  // non-owning: null unless alive
thread_local bool context_destroyed = false;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

class ThreadContextOwner {
public:
    ThreadContextOwner() noexcept { live_context = &context_; }
    ~ThreadContextOwner() {
        live_context = nullptr;
        context_destroyed = true;
    }
    ThreadContextOwner(const ThreadContextOwner&) = delete;
    ThreadContextOwner& operator=(const ThreadContextOwner&) = delete;
    ThreadContextOwner(ThreadContextOwner&&) = delete;
    ThreadContextOwner& operator=(ThreadContextOwner&&) = delete;

private:
    ThreadContext context_;
};

// The calling thread's context, constructed on first use; null once it has
// been destroyed. The owner is touched only before that, since a thread_local
// must not be used after its destructor ran.
[[nodiscard]] ThreadContext* this_thread_context() {
    if (live_context == nullptr && !context_destroyed) {
        // The single owner of this thread's ThreadContext.
        thread_local const ThreadContextOwner owner;
        static_cast<void>(owner);
    }
    return live_context;
}

// The process tags as `context` last read them, re-read if they changed.
[[nodiscard]] std::span<const OwnedTag> current_process_tags(ThreadContext& context) {
    ProcessContext& process = process_context();
    if (process.version.load(std::memory_order_acquire) != context.process_version) {
        const std::scoped_lock lock(process.mutex);
        context.process_tags = process.tags;
        context.process_version = process.version.load(std::memory_order_relaxed);
    }
    return context.process_tags->tags();
}

// Adds one tag unless the key is already there; returns 1 if it did not fit.
[[nodiscard]] uint32_t add_tag(RecordTags& tags, std::string_view key, std::string_view value) {
    if (tags.contains(key)) {
        return 0;
    }
    return tags.add(key, value) ? 0 : 1;
}

// Merges one layer; returns how many of its tags did not fit.
[[nodiscard]] uint32_t add_layer(RecordTags& tags, std::span<const OwnedTag> layer) {
    uint32_t dropped = 0;
    for (const OwnedTag& tag : layer) {
        dropped += add_tag(tags, tag.key, tag.value);
    }
    return dropped;
}

// The frame stacks, innermost first.
[[nodiscard]] uint32_t add_frames(RecordTags& tags, const ThreadContext& context) {
    uint32_t dropped = 0;
    const std::span instances(context.instances.slots.data(), context.instances.count);
    for (const Tagged* const object : std::views::reverse(instances)) {
        dropped += add_layer(tags, instance_tags(*object));
    }
    const std::span scopes(context.scopes.slots.data(), context.scopes.count);
    for (const OwnedTag& scope : std::views::reverse(scopes)) {
        dropped += add_tag(tags, scope.key, scope.value);
    }
    return dropped;
}

template <typename Entry>
void pop_frame(FrameStack<Entry> ThreadContext::*stack, const FrameToken& token) noexcept {
    if (token.context == 0) {
        return;  // never pushed
    }
    ThreadContext* const context = live_context;  // non-owning
    // A thread without a live context cannot have pushed the frame, unless
    // the frame outlived its own thread's context during teardown.
    assert((context != nullptr || context_destroyed) &&
           "lumen scope guard destroyed on a thread other than its own");
    if (context != nullptr) {
        (context->*stack).pop(token, context->serial);
    }
}

}  // namespace

FrameToken push_scope(std::string_view key, std::string_view value) {
    ThreadContext* const context = this_thread_context();  // non-owning
    if (context == nullptr) {
        return {};
    }
    OwnedTag& slot = context->scopes.next_slot();
    slot.key = key;
    slot.value = value;
    return context->scopes.push(context->serial);
}

void pop_scope(const FrameToken& token) noexcept {
    pop_frame(&ThreadContext::scopes, token);
}

FrameToken push_instance(const Tagged& object) {
    ThreadContext* const context = this_thread_context();  // non-owning
    if (context == nullptr) {
        return {};
    }
    context->instances.next_slot() = &object;
    return context->instances.push(context->serial);
}

void pop_instance(const FrameToken& token) noexcept {
    pop_frame(&ThreadContext::instances, token);
}

uint32_t merge_context(RecordTags& tags, ContextPin& pin) {
    ThreadContext* const context = this_thread_context();  // non-owning
    if (context == nullptr) {
        // The thread's context is gone; the process table is not.
        pin.process_tags_ = process_tags_now(process_context());
        return add_layer(tags, pin.process_tags_->tags());
    }
    uint32_t dropped = add_frames(tags, *context);
    dropped += add_layer(tags, context->thread_tags.tags());
    dropped += add_layer(tags, current_process_tags(*context));
    return dropped;
}

}  // namespace detail

std::expected<void, std::error_code> set_process_tag(std::string_view key, std::string_view value) {
    return detail::change_process_tags(
        [key, value](detail::TagTable& table) { return table.set(key, value); });
}

bool remove_process_tag(std::string_view key) {
    return detail::change_process_tags(
        [key](detail::TagTable& table) { return table.remove(key); });
}

void clear_process_tags() {
    static_cast<void>(detail::change_process_tags([](detail::TagTable& table) {
        table.clear();
        return true;  // published even when it was empty, which is harmless
    }));
}

std::expected<void, std::error_code> set_thread_tag(std::string_view key, std::string_view value) {
    detail::ThreadContext* const context = detail::this_thread_context();  // non-owning
    if (context == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
    return context->thread_tags.set(key, value);
}

bool remove_thread_tag(std::string_view key) {
    detail::ThreadContext* const context = detail::live_context;  // non-owning
    return context != nullptr && context->thread_tags.remove(key);
}

void clear_thread_tags() {
    detail::ThreadContext* const context = detail::live_context;  // non-owning
    if (context != nullptr) {
        context->thread_tags.clear();
    }
}

}  // namespace lumen
