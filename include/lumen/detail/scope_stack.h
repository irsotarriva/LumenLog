#ifndef LUMEN_DETAIL_SCOPE_STACK_H
#define LUMEN_DETAIL_SCOPE_STACK_H

#include <cstdint>
#include <cstdio>
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

class ScopeGuard {
public:
    ScopeGuard(std::string_view key, std::string_view value) {
        __frame.tags.add(key, value);
        __frame.next = tls_scope_head;
        tls_scope_head = &__frame;
    }

    ~ScopeGuard() {
        tls_scope_head = __frame.next;
    }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

private:
    ScopeFrame __frame;
};

class LoopScope {
public:
    LoopScope(std::string_view label, ProgressBuffer& pb, uint64_t total)
        : __label(label), __total(total), __current(0),
          __ph(pb, label, total) {
        __frame.tags.add("loop_label", label);
        __frame.next = tls_scope_head;
        tls_scope_head = &__frame;
    }

    ~LoopScope() {
        tls_scope_head = __frame.next;
        if (!__ph_done) {
            __ph.finish();
        }
    }

    LoopScope(const LoopScope&) = delete;
    LoopScope& operator=(const LoopScope&) = delete;
    LoopScope(LoopScope&&) = delete;
    LoopScope& operator=(LoopScope&&) = delete;

    [[nodiscard]] bool advance() {
        if (__current >= __total) {
            return false;
        }
        __frame.tags.add("iteration",
            std::string_view{__iter_buf, static_cast<size_t>(
                snprintf(__iter_buf, 24, "%llu",
                    static_cast<unsigned long long>(__current)))});
        __frame.tags.add("total",
            std::string_view{__total_buf, static_cast<size_t>(
                snprintf(__total_buf, 24, "%llu",
                    static_cast<unsigned long long>(__total)))});
        __ph.update(__current);
        ++__current;
        if (__current >= __total) {
            __ph.finish();
            __ph_done = true;
        }
        return true;
    }

    [[nodiscard]] uint64_t current() const {
        if (__current == 0) return 0;
        return __current - 1;
    }

private:
    ScopeFrame __frame;
    std::string_view __label;
    uint64_t __total;
    uint64_t __current;
    ProgressHandle __ph;
    bool __ph_done{false};
    char __iter_buf[24]{};
    char __total_buf[24]{};
};

}  // namespace detail
}  // namespace lumen

#endif
