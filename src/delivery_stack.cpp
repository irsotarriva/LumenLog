#include "delivery_stack.hpp"

namespace lumen::detail {

namespace {

// The calling thread's innermost delivery frame. This file is its only
// owner: DeliveryScope pushes and pops, delivery_frame_for reads.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local DeliveryFrame* tls_innermost_delivery = nullptr;

}  // namespace

DeliveryScope::DeliveryScope(const Delivery& owner) noexcept
    : frame_{&owner, tls_innermost_delivery, {}} {
    tls_innermost_delivery = &frame_;
}

DeliveryScope::~DeliveryScope() {
    tls_innermost_delivery = frame_.outer;
}

DeliveryFrame* innermost_delivery_frame() noexcept {
    return tls_innermost_delivery;
}

DeliveryFrame* delivery_frame_for(const Delivery& owner) noexcept {
    for (DeliveryFrame* frame = tls_innermost_delivery; frame != nullptr; frame = frame->outer) {
        if (frame->owner == &owner) {
            return frame;
        }
    }
    return nullptr;
}

}  // namespace lumen::detail
