#ifndef LUMEN_SRC_FATAL_RECORDS_HPP
#define LUMEN_SRC_FATAL_RECORDS_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "delivery.hpp"
#include "lumen/record.hpp"

namespace lumen::detail {

// The fatal records of one Core, which are not queued. Several threads may
// raise fatal records at once. Each is announced (counted) before it is
// stamped and posted, and the one thread that delivers does not stop until
// every announced record has been delivered, so a fatal record raised while
// another is being delivered is delivered too. It also waits a short grace
// period after the last one for records raised at about the same time.
class FatalRecords {
public:
    void announce() noexcept;
    void post(LogRecord&& record);

    // Caller holds a Session of `delivery` (its own, or one further up its
    // stack). Finishes the record in flight, delivers everything queued and
    // every fatal record announced so far or within the grace period,
    // waiting for those still being posted, then flushes the sinks.
    void deliver_all(Delivery& delivery);

    [[nodiscard]] uint64_t announced() const noexcept;

    // Fork handlers (src/fork_handlers.hpp). In the child, the caller holds
    // the lock lock_for_fork() returned. A fatal record announced or posted
    // in the parent is the parent's to deliver; the child forgets it.
    [[nodiscard]] std::unique_lock<std::mutex> lock_for_fork();
    void reset_in_fork_child() noexcept;

private:
    void deliver_announced_(Delivery& delivery);
    [[nodiscard]] std::vector<LogRecord> take_();

    std::atomic<uint64_t> announced_{0};
    uint64_t delivered_ = 0;  // guarded by the Delivery lock (a Session)
    std::mutex mutex_;
    std::vector<LogRecord> mailbox_;  // guarded by mutex_
};

}  // namespace lumen::detail

#endif
