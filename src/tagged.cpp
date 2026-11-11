#include "lumen/tagged.hpp"

#include <memory>

#include "lumen/limits.hpp"

namespace lumen {

Tagged::Scope::Scope(const Tagged& object) : frame_(detail::push_instance(object)) {}

Tagged::Scope::~Scope() {
    detail::pop_instance(frame_);
}

Tagged::Tagged(const Tagged& other)
    : tags_(other.tags_ ? std::make_unique<detail::TagTable>(*other.tags_) : nullptr) {}

Tagged& Tagged::operator=(const Tagged& other) {
    if (this != &other) {
        Tagged copy(other);
        tags_ = std::move(copy.tags_);
    }
    return *this;
}

std::expected<void, std::error_code> Tagged::lumen_tag(std::string_view key,
                                                       std::string_view value) {
    if (!tags_) {
        tags_ = std::make_unique<detail::TagTable>(max_instance_tags);
    }
    return tags_->set(key, value);
}

bool Tagged::lumen_untag(std::string_view key) {
    return tags_ && tags_->remove(key);
}

std::span<const OwnedTag> Tagged::lumen_tags() const noexcept {
    if (!tags_) {
        return {};
    }
    return tags_->tags();
}

namespace detail {

std::span<const OwnedTag> instance_tags(const Tagged& object) noexcept {
    return object.lumen_tags();
}

}  // namespace detail

}  // namespace lumen
