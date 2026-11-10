#include "lumen/error.hpp"

namespace lumen {

const char* LumenErrorCategory::name() const noexcept {
    return "lumen";
}

std::string LumenErrorCategory::message(int ev) const {
    switch (static_cast<LumenError>(ev)) {
        case LumenError::None:             return "no error";
        case LumenError::InvalidSinkId:    return "sink id not found";
        case LumenError::NullSink:         return "sink is null";
        case LumenError::SinkBusy:         return "sink cannot be removed from its own callback";
        case LumenError::TagTableFull:     return "tag table is full";
        case LumenError::InvalidPredicate: return "invalid predicate query";
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
