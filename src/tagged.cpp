#include "lumen/tagged.h"

namespace lumen {

namespace {

const TagSet<8>*& tls_instance_tags() {
    static thread_local const TagSet<8>* ptr = nullptr;  // non-owning
    return ptr;
}

}  // namespace

Tagged::Scope::Scope(const TagSet<8>* tags)
    : __previous(tls_instance_tags()) {
    tls_instance_tags() = tags;
}

Tagged::Scope::~Scope() {
    tls_instance_tags() = __previous;
}

Tagged::Tagged(const Tagged& other)
    : __keys(other.__keys), __values(other.__values), __count(other.__count) {
    __rebuild_tags();
}

Tagged& Tagged::operator=(const Tagged& other) {
    if (this != &other) {
        __keys = other.__keys;
        __values = other.__values;
        __count = other.__count;
        __rebuild_tags();
    }
    return *this;
}

Tagged::Tagged(Tagged&& other) noexcept
    : __keys(std::move(other.__keys)), __values(std::move(other.__values)),
      __count(other.__count) {
    __rebuild_tags();
    other.__count = 0;
    other.__rebuild_tags();
}

Tagged& Tagged::operator=(Tagged&& other) noexcept {
    if (this != &other) {
        __keys = std::move(other.__keys);
        __values = std::move(other.__values);
        __count = other.__count;
        __rebuild_tags();
        other.__count = 0;
        other.__rebuild_tags();
    }
    return *this;
}

void Tagged::lumen_tag(std::string_view key, std::string_view value) {
    if (__count >= __keys.size()) {
        return;
    }
    __keys[__count] = key;
    __values[__count] = value;
    ++__count;
    __rebuild_tags();
}

void Tagged::__rebuild_tags() {
    __tags = TagSet<8>{};
    for (size_t i = 0; i < __count; ++i) {
        __tags.add(__keys[i], __values[i]);
    }
}

Tagged::Scope Tagged::lumen_scope() {
    return Scope(&__tags);
}

const TagSet<8>* Tagged::current_instance_tags() {
    return tls_instance_tags();
}

namespace detail {

const TagSet<8>* current_instance_tags() {
    return Tagged::current_instance_tags();
}

}  // namespace detail

}  // namespace lumen
