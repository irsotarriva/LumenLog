#include "lumen/error.hpp"

namespace lumen {

const char* LumenErrorCategory::name() const noexcept {
    return "lumen";
}

std::string LumenErrorCategory::message(int ev) const {
    switch (static_cast<LumenError>(ev)) {
        case LumenError::None:              return "no error";
        case LumenError::InvalidSinkId:     return "sink id not found";
        case LumenError::NullSink:          return "sink is null";
        case LumenError::SinkBusy:          return "sink cannot be removed while its callback runs on this thread";
        case LumenError::TagTableFull:      return "tag table is full";
        case LumenError::InvalidPredicate:  return "invalid predicate query";
        case LumenError::InvalidSinkConfig: return "invalid sink configuration";
        case LumenError::FileOpenFailed:    return "file could not be opened";
        case LumenError::IoError:           return "write to the sink's destination failed";
        case LumenError::NotATerminal:      return "destination is not a terminal";
        case LumenError::CoreShutDown:      return "the core has shut down";
    }
    return "unknown lumen error";
}

const LumenErrorCategory& lumen_error_category() {
    static const LumenErrorCategory instance;
    return instance;
}

std::error_code make_error_code(LumenError e) {
    return {static_cast<int>(e), lumen_error_category()};
}

}  // namespace lumen
