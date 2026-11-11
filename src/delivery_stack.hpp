#ifndef LUMEN_SRC_DELIVERY_STACK_HPP
#define LUMEN_SRC_DELIVERY_STACK_HPP

#include <cstddef>
#include <variant>

#include "lumen/record.hpp"
#include "sink_registry.hpp"

namespace lumen::detail {

class Delivery;

// A record being handed to the sinks of one snapshot, and the index of the
// next sink to offer it to. A fatal record raised from a callback (or a
// shutdown run from one) finishes this record for the remaining sinks,
// because the loop that was handing it out will never resume.
struct InFlight {
    using Record =
        std::variant<std::monostate, const LogRecord*, const MetricRecord*, const ProgressRecord*>;
    Record record;                    // non-owning
    const Registry* sinks = nullptr;  // non-owning: the delivery snapshot
    std::size_t next = 0;
};

// One delivery in progress on the calling thread, which holds that
// Delivery's lock. A sink of one Core may flush another, so a thread can
// deliver for several Cores at once; the frames form a stack, innermost
// first. Lets flush, remove_sink and fatal records recognise a call made
// from inside a callback, however many Cores lie in between.
struct DeliveryFrame {
    const Delivery* owner = nullptr;  // non-owning
    DeliveryFrame* outer = nullptr;   // non-owning: the frame below it on this thread
    InFlight in_flight;
};

// Pushes a frame for `owner` on the calling thread's stack for its lifetime.
class DeliveryScope {
public:
    explicit DeliveryScope(const Delivery& owner) noexcept;
    ~DeliveryScope();
    DeliveryScope(const DeliveryScope&) = delete;
    DeliveryScope& operator=(const DeliveryScope&) = delete;
    DeliveryScope(DeliveryScope&&) = delete;
    DeliveryScope& operator=(DeliveryScope&&) = delete;

private:
    DeliveryFrame frame_;
};

// The calling thread's frame for `owner`, or null if it does not deliver
// for it.
[[nodiscard]] DeliveryFrame* delivery_frame_for(const Delivery& owner) noexcept;

// The calling thread's innermost frame, or null outside any delivery.
[[nodiscard]] DeliveryFrame* innermost_delivery_frame() noexcept;

}  // namespace lumen::detail

#endif
