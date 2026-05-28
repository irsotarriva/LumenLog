#ifndef LUMEN_DETAIL_OVERFLOW_POLICY_H
#define LUMEN_DETAIL_OVERFLOW_POLICY_H

#include <type_traits>

namespace lumen {

enum class OverflowPolicy {
    DROP_OLDEST,
    DROP_NEWEST,
    BLOCK,
};

template <OverflowPolicy P>
struct overflow_policy_trait {
    static constexpr OverflowPolicy value = P;
};

using drop_oldest_t  = overflow_policy_trait<OverflowPolicy::DROP_OLDEST>;
using drop_newest_t  = overflow_policy_trait<OverflowPolicy::DROP_NEWEST>;
using block_t        = overflow_policy_trait<OverflowPolicy::BLOCK>;

}  // namespace lumen

#endif
