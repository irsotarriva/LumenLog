#ifndef LUMEN_DETAIL_OVERFLOW_POLICY_HPP
#define LUMEN_DETAIL_OVERFLOW_POLICY_HPP

#include <type_traits>

namespace lumen {

enum class OverflowPolicy {
    DropOldest,
    DropNewest,
    Block,
};

template <OverflowPolicy P>
struct overflow_policy_trait {
    static constexpr OverflowPolicy value = P;
};

using drop_oldest_t  = overflow_policy_trait<OverflowPolicy::DropOldest>;
using drop_newest_t  = overflow_policy_trait<OverflowPolicy::DropNewest>;
using block_t        = overflow_policy_trait<OverflowPolicy::Block>;

}  // namespace lumen

#endif
