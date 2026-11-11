#ifndef LUMEN_SRC_SINK_REGISTRY_HPP
#define LUMEN_SRC_SINK_REGISTRY_HPP

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

#include "lumen/core.hpp"
#include "lumen/predicate.hpp"
#include "lumen/sink.hpp"

namespace lumen::detail {

// One registered sink. Entries are shared between the registry and the
// snapshots the delivering thread holds, so an entry outlives its removal
// until no snapshot refers to it.
//
// Removal handshake (busy/removed, both sequentially consistent): the
// delivering thread sets busy and then reads removed before each callback;
// remove() sets removed and then waits while busy. Either the deliverer sees
// removed and skips the sink, or remove() sees busy and waits for the
// callback to end. After that, nothing reads `sink` again, and `owner` can be
// handed back.
struct SinkEntry {
    SinkEntry(SinkId id_in, std::unique_ptr<Sink> owner_in, Predicate predicate_in)
        : id(id_in)
        , owner(std::move(owner_in))
        , sink(owner.get())
        , predicate(std::move(predicate_in)) {}

    const SinkId id;
    std::unique_ptr<Sink> owner;  // moved out by remove() once no callback can run
    Sink* const sink;             // non-owning: owner.get() at registration
    const Predicate predicate;
    std::atomic<bool> busy{false};     // written only by the delivering thread
    std::atomic<bool> removed{false};  // written only by remove()
    std::atomic<uint64_t> exceptions{0};
};

using Registry = std::vector<std::shared_ptr<SinkEntry>>;

// The registry at one version.
struct RegistrySnapshot {
    std::shared_ptr<const Registry> sinks;
    uint64_t version = 0;
};

// The sinks of one Core. The registry is an immutable vector replaced on
// every change and versioned, so the delivering thread can keep a snapshot
// and refresh it only when the version moved. Its mutex is held only to read
// or replace the vector, never during a callback.
class SinkRegistry {
public:
    // Fails with NullSink for a null sink and with CoreShutDown after close().
    // A refused sink is destroyed before this returns, without the lock.
    [[nodiscard]] std::expected<SinkId, std::error_code> add(std::unique_ptr<Sink> sink,
                                                             Predicate predicate);

    // `caller_delivers`: the calling thread holds the Core's delivery lock,
    // so a busy sink is in a callback further up its own stack and is
    // refused with SinkBusy instead of waited for.
    [[nodiscard]] std::expected<std::unique_ptr<Sink>, std::error_code> remove(
        SinkId id, bool caller_delivers);

    [[nodiscard]] std::expected<uint64_t, std::error_code> exception_count(SinkId id) const;

    // From now on add() fails with CoreShutDown.
    void close() noexcept;

    // Empties the registry and destroys the sinks no snapshot refers to,
    // without the lock held: a sink's destructor may call add, remove or
    // exception_count.
    void destroy_sinks() noexcept;

    // For the delivering thread's snapshot: the version alone, without the
    // lock, and the registry with its version, read together under it.
    [[nodiscard]] uint64_t version() const noexcept;
    [[nodiscard]] RegistrySnapshot snapshot() const;

private:
    // Caller holds mutex_.
    void replace_(std::shared_ptr<const Registry> next);
    [[nodiscard]] std::shared_ptr<SinkEntry> find_(SinkId id) const;

    mutable std::mutex mutex_;
    std::shared_ptr<const Registry> sinks_ =
        std::make_shared<const Registry>();  // guarded by mutex_
    std::atomic<uint64_t> version_{0};       // bumped under mutex_
    bool closed_ = false;                    // guarded by mutex_
    std::atomic<SinkId> next_id_{1};
};

}  // namespace lumen::detail

#endif
