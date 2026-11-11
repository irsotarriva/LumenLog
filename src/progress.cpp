#include "lumen/progress.hpp"

#include <atomic>
#include <utility>

namespace lumen {

namespace {

[[nodiscard]] uint64_t next_progress_id() {
    // Ids are unique for the whole program, across Cores.
    static std::atomic<uint64_t> next_id{1};
    return next_id.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

ProgressHandle::ProgressHandle(Core& core, std::string_view label, uint64_t total)
    : core_(&core), label_(label), id_(next_progress_id()), total_(total) {}

ProgressHandle::ProgressHandle(ProgressHandle&& other) noexcept
    : core_(std::exchange(other.core_, nullptr))
    , label_(std::move(other.label_))
    , id_(other.id_)
    , current_(other.current_)
    , total_(other.total_)
    , sent_any_(other.sent_any_)
    , last_sent_(other.last_sent_)
    , finished_(other.finished_) {}

ProgressHandle& ProgressHandle::operator=(ProgressHandle&& other) noexcept {
    if (this != &other) {
        core_ = std::exchange(other.core_, nullptr);
        label_ = std::move(other.label_);
        id_ = other.id_;
        current_ = other.current_;
        total_ = other.total_;
        sent_any_ = other.sent_any_;
        last_sent_ = other.last_sent_;
        finished_ = other.finished_;
    }
    return *this;
}

void ProgressHandle::update(uint64_t current) {
    if (core_ == nullptr || finished_) {
        return;
    }
    current_ = current;
    send_();
}

void ProgressHandle::tick() {
    update(current_ + 1);
}

void ProgressHandle::finish() {
    if (core_ == nullptr || finished_) {
        return;
    }
    current_ = total_;
    send_();
    finished_ = true;
}

void ProgressHandle::send_() {
    if (sent_any_ && last_sent_ == current_) {
        return;
    }
    ProgressRecord record{};
    record.id = id_;
    record.label = label_;
    record.current = current_;
    record.total = total_;
    core_->submit_(std::move(record), 0);
    sent_any_ = true;
    last_sent_ = current_;
}

ProgressHandle progress(std::string_view label, uint64_t total) {
    return ProgressHandle(core(), label, total);
}

}  // namespace lumen
