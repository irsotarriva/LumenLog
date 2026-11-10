#include "lumen/tagged.hpp"

#include <algorithm>
#include <string>

#include "lumen/error.hpp"
#include "lumen/limits.hpp"

namespace lumen {

Tagged::Scope::Scope(const Tagged& object) noexcept : frame_{.object = &object} {
    detail::push_instance(frame_);
}

Tagged::Scope::~Scope() {
    detail::pop_instance(frame_);
}

Tagged::Tagged(const Tagged& other)
    : tags_(other.tags_ ? std::make_unique<std::vector<detail::OwnedTag>>(*other.tags_) : nullptr) {
}

Tagged& Tagged::operator=(const Tagged& other) {
    if (this != &other) {
        Tagged copy(other);
        tags_ = std::move(copy.tags_);
    }
    return *this;
}

std::expected<void, std::error_code> Tagged::lumen_tag(std::string_view key,
                                                       std::string_view value) {
    if (!tags_) tags_ = std::make_unique<std::vector<detail::OwnedTag>>();
    const auto it =
        std::ranges::find_if(*tags_, [key](const detail::OwnedTag& tag) { return tag.key == key; });
    if (it != tags_->end()) {
        it->value = value;
        return {};
    }
    if (tags_->size() == max_instance_tags) {
        return std::unexpected(make_error_code(LumenError::TagTableFull));
    }
    tags_->push_back(detail::OwnedTag{std::string(key), std::string(value)});
    return {};
}

bool Tagged::lumen_untag(std::string_view key) {
    if (!tags_) return false;
    return std::erase_if(*tags_, [key](const detail::OwnedTag& tag) { return tag.key == key; }) !=
           0;
}

std::span<const detail::OwnedTag> Tagged::lumen_tags() const noexcept {
    if (!tags_) return {};
    return *tags_;
}

namespace detail {

std::span<const OwnedTag> instance_tags(const Tagged& object) noexcept {
    return object.lumen_tags();
}

}  // namespace detail

}  // namespace lumen
