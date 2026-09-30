#include "lumen/tagged.h"

namespace lumen {

namespace {

const TagSet<8>*& tls_instance_tags() {
    static thread_local const TagSet<8>* ptr = nullptr;  // non-owning
    return ptr;
}

}  // namespace

Tagged::Scope::Scope(const TagSet<8>* tags)
    : previous_(tls_instance_tags()) {
    tls_instance_tags() = tags;
}

Tagged::Scope::~Scope() {
    tls_instance_tags() = previous_;
}

Tagged::Tagged(const Tagged& other)
    : keys_(other.keys_), values_(other.values_), count_(other.count_) {
    rebuild_tags_();
}

Tagged& Tagged::operator=(const Tagged& other) {
    if (this != &other) {
        keys_ = other.keys_;
        values_ = other.values_;
        count_ = other.count_;
        rebuild_tags_();
    }
    return *this;
}

Tagged::Tagged(Tagged&& other) noexcept
    : keys_(std::move(other.keys_)), values_(std::move(other.values_)),
      count_(other.count_) {
    rebuild_tags_();
    other.count_ = 0;
    other.rebuild_tags_();
}

Tagged& Tagged::operator=(Tagged&& other) noexcept {
    if (this != &other) {
        keys_ = std::move(other.keys_);
        values_ = std::move(other.values_);
        count_ = other.count_;
        rebuild_tags_();
        other.count_ = 0;
        other.rebuild_tags_();
    }
    return *this;
}

void Tagged::lumen_tag(std::string_view key, std::string_view value) {
    if (count_ >= keys_.size()) {
        return;
    }
    keys_[count_] = key;
    values_[count_] = value;
    ++count_;
    rebuild_tags_();
}

void Tagged::rebuild_tags_() {
    tags_ = TagSet<8>{};
    for (size_t i = 0; i < count_; ++i) {
        tags_.add(keys_[i], values_[i]);
    }
}

Tagged::Scope Tagged::lumen_scope() {
    return Scope(&tags_);
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
