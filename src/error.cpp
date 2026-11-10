#include "lumen/detail/error.hpp"

namespace lumen {

const char* LumenErrorCategory::name() const noexcept {
    return "lumen";
}

std::string LumenErrorCategory::message(int ev) const {
    switch (static_cast<LumenError>(ev)) {
        case LumenError::None:
            return "no error";
        case LumenError::BufferFull:
            return "ring buffer is full";
        case LumenError::InvalidSinkId:
            return "sink id not found";
        case LumenError::IoError:
            return "I/O error";
        case LumenError::FileOpenError:
            return "failed to open file";
        case LumenError::SinkException:
            return "exception caught in sink callback";
        case LumenError::InvalidPredicate:
            return "invalid predicate query";
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
