#include "lumen/detail/context.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "immortal.hpp"
#include "lumen/context.hpp"
#include "lumen/error.hpp"
#include "lumen/limits.hpp"

namespace lumen {
namespace detail {
namespace {

// A table of distinct keys with a fixed maximum size.
class TagTable {
public:
    explicit TagTable(std::size_t max_size) : max_size_(max_size) {}

    [[nodiscard]] std::expected<void, std::error_code> set(std::string_view key,
                                                           std::string_view value) {
        if (OwnedTag* const existing = find_(key)) {
            existing->value = value;
            return {};
        }
        if (tags_.size() == max_size_) {
            return std::unexpected(make_error_code(LumenError::TagTableFull));
        }
        tags_.push_back(OwnedTag{std::string(key), std::string(value)});
        return {};
    }

    [[nodiscard]] bool remove(std::string_view key) {
        return std::erase_if(tags_, [key](const OwnedTag& tag) { return tag.key == key; }) != 0;
    }

    void clear() { tags_.clear(); }

    [[nodiscard]] std::span<const OwnedTag> tags() const { return tags_; }

private:
    [[nodiscard]] OwnedTag* find_(std::string_view key) {
        const auto it =
            std::ranges::find_if(tags_, [key](const OwnedTag& tag) { return tag.key == key; });
        return it == tags_.end() ? nullptr : &*it;
    }

    std::size_t max_size_;
    std::vector<OwnedTag> tags_;
};

// The process layer. `version` grows after every change, so that a thread can
// tell cheaply whether its copy is current.
struct ProcessContext {
    std::mutex mutex;
    TagTable table{max_process_tags};  // guarded by mutex
    std::atomic<uint64_t> version{1};  // written under mutex, read without it
};

// Never destroyed: a record submitted during static destruction still reads it.
ProcessContext& process_context() {
    static Immortal<ProcessContext> instance;
    return instance.get();
}

// Everything ambient that belongs to one thread. This thread_local is the
// single owner of thread tags, scope frames and instance frames.
struct ThreadContext {
    TagTable thread_tags{max_thread_tags};
    ScopeFrame* scope_head = nullptr;        // non-owning: innermost live scope frame
    InstanceFrame* instance_head = nullptr;  // non-owning: innermost live instance frame
    // This thread's copy of the process layer, so that merging reads strings
    // no other thread writes. Refreshed when process_context().version moves.
    uint64_t process_version = 0;
    std::vector<OwnedTag> process_tags;
};

thread_local ThreadContext tls_context;

[[nodiscard]] std::span<const OwnedTag> current_process_tags() {
    ProcessContext& process = process_context();
    ThreadContext& context = tls_context;
    if (process.version.load(std::memory_order_acquire) != context.process_version) {
        const std::scoped_lock lock(process.mutex);
        const std::span<const OwnedTag> tags = process.table.tags();
        context.process_tags.assign(tags.begin(), tags.end());
        context.process_version = process.version.load(std::memory_order_relaxed);
    }
    return context.process_tags;
}

void bump_version(ProcessContext& process) {
    process.version.fetch_add(1, std::memory_order_release);
}

// Unlinks `frame` from the list starting at `head`. In a correct program it
// is the head; anything else is a guard destroyed out of order or on another
// thread, which the debug assertion reports.
template <typename Frame>
    requires std::same_as<Frame, ScopeFrame> || std::same_as<Frame, InstanceFrame>
void unlink(Frame*& head, Frame& frame) noexcept {
    assert(head == &frame &&
           "lumen scope guard destroyed out of order or on a thread other than its own");
    for (Frame** link = &head; *link != nullptr; link = &(*link)->next) {
        if (*link == &frame) {
            *link = frame.next;
            return;
        }
    }
}

// Merges one layer; returns how many of its tags did not fit.
[[nodiscard]] uint32_t add_layer(RecordTags& tags, std::span<const OwnedTag> layer) {
    uint32_t dropped = 0;
    for (const OwnedTag& tag : layer) {
        if (tags.contains(tag.key)) continue;
        if (!tags.add(tag.key, tag.value)) ++dropped;
    }
    return dropped;
}

}  // namespace

void push_scope(ScopeFrame& frame) noexcept {
    frame.next = tls_context.scope_head;
    tls_context.scope_head = &frame;
}

void pop_scope(ScopeFrame& frame) noexcept {
    unlink(tls_context.scope_head, frame);
}

void push_instance(InstanceFrame& frame) noexcept {
    frame.next = tls_context.instance_head;
    tls_context.instance_head = &frame;
}

void pop_instance(InstanceFrame& frame) noexcept {
    unlink(tls_context.instance_head, frame);
}

uint32_t merge_context(RecordTags& tags) {
    const ThreadContext& context = tls_context;
    uint32_t dropped = 0;
    for (const InstanceFrame* f = context.instance_head; f != nullptr; f = f->next) {
        dropped += add_layer(tags, instance_tags(*f->object));
    }
    for (const ScopeFrame* f = context.scope_head; f != nullptr; f = f->next) {
        if (tags.contains(f->key)) continue;
        if (!tags.add(f->key, f->value)) ++dropped;
    }
    dropped += add_layer(tags, context.thread_tags.tags());
    dropped += add_layer(tags, current_process_tags());
    return dropped;
}

}  // namespace detail

std::expected<void, std::error_code> set_process_tag(std::string_view key, std::string_view value) {
    detail::ProcessContext& process = detail::process_context();
    const std::scoped_lock lock(process.mutex);
    auto result = process.table.set(key, value);
    if (result) detail::bump_version(process);
    return result;
}

bool remove_process_tag(std::string_view key) {
    detail::ProcessContext& process = detail::process_context();
    const std::scoped_lock lock(process.mutex);
    const bool removed = process.table.remove(key);
    if (removed) detail::bump_version(process);
    return removed;
}

void clear_process_tags() {
    detail::ProcessContext& process = detail::process_context();
    const std::scoped_lock lock(process.mutex);
    process.table.clear();
    detail::bump_version(process);
}

std::expected<void, std::error_code> set_thread_tag(std::string_view key, std::string_view value) {
    return detail::tls_context.thread_tags.set(key, value);
}

bool remove_thread_tag(std::string_view key) {
    return detail::tls_context.thread_tags.remove(key);
}

void clear_thread_tags() {
    detail::tls_context.thread_tags.clear();
}

}  // namespace lumen
