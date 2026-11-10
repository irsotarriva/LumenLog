#ifndef LUMEN_DETAIL_SCOPE_STACK_HPP
#define LUMEN_DETAIL_SCOPE_STACK_HPP

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include "lumen/core.hpp"
#include "lumen/detail/context.hpp"
#include "lumen/progress.hpp"

// The guards behind LUMEN_SCOPE and LUMEN_LOOP (lumen/macros.hpp documents
// what they do for the user).

namespace lumen::detail {

// Adds key=value to the calling thread's context while it lives. Owns copies
// of both strings, so LUMEN_SCOPE("run", std::to_string(n)) is safe.
class ScopeGuard {
public:
    ScopeGuard(std::string_view key, std::string_view value) : key_(key), value_(value) {
        frame_.key = key_;
        frame_.value = value_;
        push_scope(frame_);
    }
    ~ScopeGuard() { pop_scope(frame_); }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

private:
    std::string key_;
    std::string value_;
    ScopeFrame frame_;
};

// The state of one LUMEN_LOOP: a progress bar and the scope tag
// label=<index of the iteration running>.
template <std::integral T>
    requires(!std::same_as<T, bool>)
class LoopScope {
public:
    LoopScope(Core& core, std::string_view label, T total)
        : total_(total > T{0} ? static_cast<uint64_t>(total) : 0)
        , label_(label)
        , progress_(core, label, total_) {
        frame_.key = label_;
        push_scope(frame_);
    }
    ~LoopScope() { pop_scope(frame_); }

    LoopScope(const LoopScope&) = delete;
    LoopScope& operator=(const LoopScope&) = delete;
    LoopScope(LoopScope&&) = delete;
    LoopScope& operator=(LoopScope&&) = delete;

    // Called before every iteration, including the first: the previous body
    // (if any) has completed, so it is counted. Returns false, after sending
    // total/total, when no iteration is left.
    [[nodiscard]] bool advance() {
        if (started_) {
            ++completed_;
        }
        started_ = true;
        if (completed_ >= total_) {
            progress_.finish();
            return false;
        }
        progress_.update(completed_);
        const auto [end, ec] =
            std::to_chars(index_.data(), index_.data() + index_.size(), completed_);
        // 24 characters hold every uint64_t, so to_chars cannot fail.
        frame_.value =
            ec == std::errc{} ? std::string_view(index_.data(), end) : std::string_view{};
        return true;
    }

    // The index of the iteration running, in the type of the loop's total.
    [[nodiscard]] T index() const noexcept { return static_cast<T>(completed_); }

private:
    uint64_t total_;
    uint64_t completed_ = 0;
    bool started_ = false;
    std::string label_;
    std::array<char, 24> index_{};
    ProgressHandle progress_;
    ScopeFrame frame_;
};

}  // namespace lumen::detail

#endif
