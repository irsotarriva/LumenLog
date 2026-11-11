#include "sink_registry.hpp"

#include <algorithm>
#include <thread>
#include <utility>

#include "lumen/error.hpp"

namespace lumen::detail {

std::expected<SinkId, std::error_code> SinkRegistry::add(std::unique_ptr<Sink> sink,
                                                         Predicate predicate) {
    if (!sink) {
        return std::unexpected(make_error_code(LumenError::NullSink));
    }
    const SinkId id = next_id_.fetch_add(1, std::memory_order_relaxed);
    // Declared before the lock, so a refused sink is destroyed after the
    // lock is released (its destructor may call back into the Core).
    auto entry = std::make_shared<SinkEntry>(id, std::move(sink), std::move(predicate));
    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return std::unexpected(make_error_code(LumenError::CoreShutDown));
    }
    auto next = std::make_shared<Registry>(*sinks_);
    next->push_back(std::move(entry));
    replace_(std::move(next));
    return id;
}

std::expected<std::unique_ptr<Sink>, std::error_code> SinkRegistry::remove(SinkId id,
                                                                           bool caller_delivers) {
    std::shared_ptr<SinkEntry> entry;
    {
        const std::scoped_lock lock(mutex_);
        entry = find_(id);
        if (!entry) {
            return std::unexpected(make_error_code(LumenError::InvalidSinkId));
        }
        // Waiting for a callback further up the caller's own stack would
        // never end.
        if (caller_delivers && entry->busy.load(std::memory_order_seq_cst)) {
            return std::unexpected(make_error_code(LumenError::SinkBusy));
        }
        entry->removed.store(true, std::memory_order_seq_cst);
        auto next = std::make_shared<Registry>(*sinks_);
        std::erase(*next, entry);
        replace_(std::move(next));
    }
    // Only a callback on another thread can be running; wait for it.
    while (entry->busy.load(std::memory_order_seq_cst)) {
        std::this_thread::yield();
    }
    return std::move(entry->owner);
}

std::expected<uint64_t, std::error_code> SinkRegistry::exception_count(SinkId id) const {
    const std::scoped_lock lock(mutex_);
    const std::shared_ptr<SinkEntry> entry = find_(id);
    if (!entry) {
        return std::unexpected(make_error_code(LumenError::InvalidSinkId));
    }
    return entry->exceptions.load(std::memory_order_relaxed);
}

void SinkRegistry::close() noexcept {
    const std::scoped_lock lock(mutex_);
    closed_ = true;
}

void SinkRegistry::destroy_sinks() noexcept {
    std::shared_ptr<const Registry> retired;
    {
        const std::scoped_lock lock(mutex_);
        retired = std::exchange(sinks_, std::make_shared<const Registry>());
        version_.fetch_add(1, std::memory_order_release);
    }
    retired.reset();
}

uint64_t SinkRegistry::version() const noexcept {
    return version_.load(std::memory_order_acquire);
}

std::shared_ptr<const Registry> SinkRegistry::refresh(std::shared_ptr<const Registry>& held,
                                                      uint64_t& version) const {
    const std::scoped_lock lock(mutex_);
    std::shared_ptr<const Registry> replaced = std::exchange(held, sinks_);
    version = version_.load(std::memory_order_relaxed);
    return replaced;
}

std::unique_lock<std::mutex> SinkRegistry::lock_for_fork() {
    return std::unique_lock(mutex_);
}

void SinkRegistry::clear_busy_in_fork_child() noexcept {
    for (const std::shared_ptr<SinkEntry>& entry : *sinks_) {
        entry->busy.store(false, std::memory_order_relaxed);
    }
}

void SinkRegistry::replace_(std::shared_ptr<const Registry> next) {
    sinks_ = std::move(next);
    version_.fetch_add(1, std::memory_order_release);
}

std::shared_ptr<SinkEntry> SinkRegistry::find_(SinkId id) const {
    const auto it = std::ranges::find_if(
        *sinks_, [id](const std::shared_ptr<SinkEntry>& e) { return e->id == id; });
    return it == sinks_->end() ? nullptr : *it;
}

}  // namespace lumen::detail
