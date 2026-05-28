#include "lumen/tagged.h"

namespace lumen {

namespace {

const TagSet<8>*& tls_instance_tags() {
    static thread_local const TagSet<8>* ptr = nullptr;
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

void Tagged::lumen_tag(std::string_view key, std::string_view value) {
    __tags.add(key, value);
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
