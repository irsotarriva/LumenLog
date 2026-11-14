#ifndef LUMEN_PROGRESS_HPP
#define LUMEN_PROGRESS_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "lumen/core.hpp"

namespace lumen {

// A progress bar: a sequence of ProgressRecords sharing one id.
//
//   auto bar = lumen::progress("files", files.size());
//   for (const auto& f : files) { copy(f); bar.tick(); }
//   bar.finish();
//
// `current` counts completed units of work. Every record the handle sends is
// a state that was actually reached:
//   - Creating the handle sends nothing.
//   - update(n) sends n/total; tick() sends (current + 1)/total. A call that
//     would send the state already sent last is not sent again.
//   - finish() sends total/total once, if that state was not already the last
//     one sent, and ends the bar: later update(), tick() and finish() do
//     nothing.
//   - Destroying a handle that was not finished sends nothing. The last
//     record is the last progress made; no record claims completion that did
//     not happen. A sink that needs to know the bar was abandoned has to
//     decide from the absence of total/total.
//   - A moved-from handle does nothing.
// Each record carries the context of the thread and moment it was sent.
//
// A handle is not synchronised: use it from one thread at a time.
class ProgressHandle {
public:
    ProgressHandle(Core& core, std::string_view label, uint64_t total);
    ~ProgressHandle() = default;

    ProgressHandle(const ProgressHandle&) = delete;
    ProgressHandle& operator=(const ProgressHandle&) = delete;
    ProgressHandle(ProgressHandle&& other) noexcept;
    ProgressHandle& operator=(ProgressHandle&& other) noexcept;

    void update(uint64_t current);
    void tick();
    void finish();

    [[nodiscard]] uint64_t current() const noexcept { return current_; }
    [[nodiscard]] uint64_t total() const noexcept { return total_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

private:
    void send_();

    Core* core_;  // non-owning; null once moved from
    std::string label_;
    uint64_t id_;
    uint64_t current_ = 0;
    uint64_t total_;
    bool sent_any_ = false;
    uint64_t last_sent_ = 0;
    bool finished_ = false;
};

// A progress bar on the global core().
[[nodiscard]] ProgressHandle progress(std::string_view label, uint64_t total);

}  // namespace lumen

#endif
