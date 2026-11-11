#ifndef LUMEN_BRIDGES_OPENMP_HPP
#define LUMEN_BRIDGES_OPENMP_HPP

// OpenMP helpers. Nothing here hooks the OpenMP runtime: each function emits
// records only when the program calls it.
//
//   emit_openmp_configuration()   once, from serial code: the thread count the
//                                 runtime will use and the processors it sees.
//   OpenMpWorkBalance             how evenly one parallel region spread its
//                                 work over the team.
//
// The header is empty unless the translation unit is compiled with OpenMP
// 3.0 or later enabled (GCC and Clang -fopenmp): _OPENMP then holds the
// date of the specification, 200805 for 3.0. OpenMP 2.0 (MSVC /openmp)
// lacks omp_get_level().

#if defined(_OPENMP) && _OPENMP >= 200805

#include <omp.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lumen/core.hpp"
#include "lumen/record_builder.hpp"

namespace lumen::bridges {

// Emits two metrics, without tags:
//   openmp.max_threads  omp_get_max_threads(): the team size of the next
//                       parallel region without a num_threads clause
//   openmp.num_procs    omp_get_num_procs(): processors available to the program
// Call it from serial code; inside a parallel region omp_get_max_threads()
// describes a nested region instead.
inline void emit_openmp_configuration(Core& core = lumen::core()) {
    // NOLINTBEGIN(bugprone-unused-raii): a builder submits its record when destroyed
    MetricBuilder(core, "openmp.max_threads", static_cast<double>(omp_get_max_threads()));
    MetricBuilder(core, "openmp.num_procs", static_cast<double>(omp_get_num_procs()));
    // NOLINTEND(bugprone-unused-raii)
}

// Per-thread tallies of work done in one parallel region, and the imbalance
// they show. The unit of work is the caller's: events, iterations, bytes.
//
//   lumen::bridges::OpenMpWorkBalance balance("events");
//   #pragma omp parallel for schedule(dynamic)
//   for (int64_t i = 0; i < n_events; ++i) {
//       process(i);
//       balance.add(1);
//   }
//   balance.emit();
//
// add() is called inside the region, by any thread of the team, without a
// lock: each thread writes only its own slot. emit() is called after the
// region has ended (its closing barrier is what makes the slots visible).
//
// emit() sends, each tagged region=<name> and threads=<team size>:
//   openmp.work.total         units added to thread slots
//   openmp.work.imbalance     the busiest thread's units divided by the mean
//                             per thread: 1 is perfect balance, `threads`
//                             means one thread did everything. Not sent when
//                             the total is 0, nor when some units are
//                             unattributed (their threads are unknown, so the
//                             ratio would be wrong).
//   openmp.work.unattributed  units that could not be given to a thread slot
//                             (see below). Sent only when not 0.
// The team size is the largest omp_get_num_threads() seen by an add() that
// reached a slot, and 0 if none did; a thread of that team that never called
// add() counts as having done no work.
//
// Slots. The object has one slot per thread number below `max_team_size`
// (default: omp_get_max_threads() when the object is constructed). add()
// goes to the unattributed count instead
//   - from a thread whose number is not below it (a num_threads clause asking
//     for more), and
//   - from inside a region nested in an enclosing region whose team has more
//     than one thread: thread numbers then repeat across the inner teams.
// A region nested only in regions with a team of one thread (serialised by
// if(false), num_threads(1) or max-active-levels) is attributed like an
// outermost one: its thread numbers name distinct threads. Called outside
// any parallel region, add() counts as thread 0 of a team of 1.
class OpenMpWorkBalance {
public:
    explicit OpenMpWorkBalance(std::string_view region,
                               int32_t max_team_size = omp_get_max_threads())
        : region_(region), slots_(static_cast<std::size_t>(std::max<int32_t>(max_team_size, 1))) {}

    OpenMpWorkBalance(const OpenMpWorkBalance&) = delete;
    OpenMpWorkBalance& operator=(const OpenMpWorkBalance&) = delete;
    OpenMpWorkBalance(OpenMpWorkBalance&&) = delete;
    OpenMpWorkBalance& operator=(OpenMpWorkBalance&&) = delete;
    ~OpenMpWorkBalance() = default;

    void add(uint64_t units) noexcept {
        const int32_t thread = omp_get_thread_num();
        if (!enclosing_teams_are_single_() || thread < 0 ||
            static_cast<std::size_t>(thread) >= slots_.size()) {
            unattributed_.fetch_add(units, std::memory_order_relaxed);
            return;
        }
        Slot& slot = slots_[static_cast<std::size_t>(thread)];
        slot.units += units;
        slot.team_size = std::max<int32_t>(slot.team_size, omp_get_num_threads());
    }

    void emit(Core& core = lumen::core()) const {
        int32_t team_size = 0;
        uint64_t total = 0;
        uint64_t busiest = 0;
        for (const Slot& slot : slots_) {
            team_size = std::max(team_size, slot.team_size);
            total += slot.units;
            busiest = std::max(busiest, slot.units);
        }
        MetricBuilder(core, "openmp.work.total", static_cast<double>(total))
            .tag("region", region_)
            .tag("threads", team_size);
        const uint64_t unattributed = unattributed_.load(std::memory_order_relaxed);
        if (total > 0 && unattributed == 0) {
            const double mean = static_cast<double>(total) / static_cast<double>(team_size);
            MetricBuilder(core, "openmp.work.imbalance", static_cast<double>(busiest) / mean)
                .tag("region", region_)
                .tag("threads", team_size);
        }
        if (unattributed > 0) {
            MetricBuilder(core, "openmp.work.unattributed", static_cast<double>(unattributed))
                .tag("region", region_)
                .tag("threads", team_size);
        }
    }

private:
    // True when every region enclosing the calling thread's innermost one
    // has a team of one thread, so that the innermost thread number names
    // one thread of the process.
    [[nodiscard]] static bool enclosing_teams_are_single_() noexcept {
        const int32_t level = omp_get_level();
        for (int32_t enclosing = 1; enclosing < level; ++enclosing) {
            if (omp_get_team_size(enclosing) > 1) {
                return false;
            }
        }
        return true;
    }

    // One cache line per slot, so that threads adding to neighbouring slots
    // do not slow each other down. 64 bytes rather than
    // std::hardware_destructive_interference_size, whose value GCC warns may
    // differ between compilers and so should not shape a type in a header.
    struct alignas(64) Slot {
        uint64_t units = 0;
        int32_t team_size = 0;
    };

    std::string region_;
    std::vector<Slot> slots_;  // slot i is written only by thread i of the team
    std::atomic<uint64_t> unattributed_{0};
};

}  // namespace lumen::bridges

#endif  // defined(_OPENMP) && _OPENMP >= 200805

#endif
