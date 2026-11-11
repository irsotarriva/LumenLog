#include "lumen/error.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace lumen {

namespace {

struct ErrorMessage {
    LumenError code;
    std::string_view text;
};

// One entry per LumenError, in declaration order, so that an enumerator's
// value is its index. A new enumerator is appended to the enum and its entry
// to the end of this table; the static_assert below rejects a table whose
// entries are out of order.
constexpr std::array error_messages = std::to_array<ErrorMessage>({
    {LumenError::None, "no error"},
    {LumenError::InvalidSinkId, "sink id not found"},
    {LumenError::NullSink, "sink is null"},
    {LumenError::SinkBusy, "sink is busy in a callback on this thread"},
    {LumenError::TagTableFull, "tag table is full"},
    {LumenError::InvalidPredicate, "invalid predicate query"},
    {LumenError::InvalidSinkConfig, "invalid sink configuration"},
    {LumenError::FileOpenFailed, "file could not be opened"},
    {LumenError::IoError, "write to the sink's destination failed"},
    {LumenError::NotATerminal, "destination is not a terminal"},
    {LumenError::CoreShutDown, "the core has shut down"},
    {LumenError::FlushTimedOut, "the flush did not finish in time"},
});

[[nodiscard]] consteval bool indexed_by_value() {
    for (std::size_t i = 0; i < error_messages.size(); ++i) {
        if (std::to_underlying(error_messages[i].code) != i) {
            return false;
        }
    }
    return true;
}
static_assert(indexed_by_value(), "error_messages must list every LumenError in order");

}  // namespace

const char* LumenErrorCategory::name() const noexcept {
    return "lumen";
}

std::string LumenErrorCategory::message(int ev) const {
    if (ev < 0 || static_cast<std::size_t>(ev) >= error_messages.size()) {
        return "unknown lumen error";
    }
    return std::string(error_messages[static_cast<std::size_t>(ev)].text);
}

const LumenErrorCategory& lumen_error_category() {
    static const LumenErrorCategory instance;
    return instance;
}

std::error_code make_error_code(LumenError e) {
    return {static_cast<int>(e), lumen_error_category()};
}

}  // namespace lumen
