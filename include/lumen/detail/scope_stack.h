#ifndef LUMEN_DETAIL_SCOPE_STACK_H
#define LUMEN_DETAIL_SCOPE_STACK_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "lumen/record.h"

namespace lumen {
namespace detail {

struct ScopeFrame {
    TagSet<8> tags;
    ScopeFrame* next{nullptr};  // non-owning; frames live on the stack
};

inline thread_local ScopeFrame* tls_scope_head = nullptr;  // non-owning; head of stack-linked list

inline void collect_scope_tags(TagSet<8>& out) {
    for (const ScopeFrame* f = tls_scope_head; f; f = f->next) {
        for (const auto& e : f->tags) {
            out.add(e.key, e.value);
        }
    }
}

// Owns copies of its key and value, so a temporary such as
// LUMEN_SCOPE("run", std::to_string(n)) is safe.
class ScopeGuard {
public:
    ScopeGuard(std::string_view key, std::string_view value)
        : key_(key), value_(value) {
        frame_.tags.add(key_, value_);
        frame_.next = tls_scope_head;
        tls_scope_head = &frame_;
    }

    ~ScopeGuard() {
        tls_scope_head = frame_.next;
    }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

private:
    std::string key_;
    std::string value_;
    ScopeFrame frame_;
};

class LoopScope {
public:
    LoopScope(std::string_view label, ProgressBuffer& pb, uint64_t total)
        : label_(label), total_(total), current_(0),
          ph_(pb, label, total) {
        frame_.tags.add("loop_label", label_);
        frame_.next = tls_scope_head;
        tls_scope_head = &frame_;
    }

    ~LoopScope() {
        tls_scope_head = frame_.next;
        if (!ph_done_) {
            ph_.finish();
        }
    }

    LoopScope(const LoopScope&) = delete;
    LoopScope& operator=(const LoopScope&) = delete;
    LoopScope(LoopScope&&) = delete;
    LoopScope& operator=(LoopScope&&) = delete;

    [[nodiscard]] bool advance() {
        if (current_ >= total_) {
            return false;
        }
        frame_.tags.set("iteration",
            std::string_view{iter_buf_, static_cast<size_t>(
                snprintf(iter_buf_, 24, "%llu",
                    static_cast<unsigned long long>(current_)))});
        frame_.tags.set("total",
            std::string_view{total_buf_, static_cast<size_t>(
                snprintf(total_buf_, 24, "%llu",
                    static_cast<unsigned long long>(total_)))});
        ph_.update(current_);
        ++current_;
        if (current_ >= total_) {
            ph_.finish();
            ph_done_ = true;
        }
        return true;
    }

    [[nodiscard]] uint64_t current() const {
        if (current_ == 0) return 0;
        return current_ - 1;
    }

private:
    ScopeFrame frame_;
    std::string label_;
    uint64_t total_;
    uint64_t current_;
    ProgressHandle ph_;
    bool ph_done_{false};
    char iter_buf_[24]{};
    char total_buf_[24]{};
};

}  // namespace detail
}  // namespace lumen

#endif
