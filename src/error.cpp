#include "lumen/detail/error.h"

namespace lumen {

const char* LumenErrorCategory::name() const noexcept {
    return "lumen";
}

std::string LumenErrorCategory::message(int ev) const {
    switch (static_cast<LumenError>(ev)) {
        case LumenError::none:
            return "no error";
        case LumenError::arena_overflow:
            return "thread-local arena overflow";
        case LumenError::buffer_full:
            return "ring buffer is full";
        case LumenError::invalid_sink_id:
            return "sink id not found";
    }
    return "unknown lumen error";
}

bool LumenErrorCategory::equivalent(const std::error_code& code,
                                    int condition) const noexcept {
    if (code.category() == *this) {
        return code.value() == condition;
    }
    return false;
}

const LumenErrorCategory& lumen_error_category() {
    static const LumenErrorCategory instance;
    return instance;
}

std::error_code make_error_code(LumenError e) {
    return {static_cast<int>(e), lumen_error_category()};
}

}  // namespace lumen
