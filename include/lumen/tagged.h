#ifndef LUMEN_TAGGED_H
#define LUMEN_TAGGED_H

#include <array>
#include <string>
#include <string_view>

#include "lumen/record.h"

namespace lumen {

class Tagged {
public:
    class Scope {
    public:
        explicit Scope(const TagSet<8>* tags);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

    private:
        const TagSet<8>* previous_;  // non-owning
    };

    Tagged() = default;
    Tagged(const Tagged& other);
    Tagged& operator=(const Tagged& other);
    Tagged(Tagged&& other) noexcept;
    Tagged& operator=(Tagged&& other) noexcept;

    // Copies key and value; temporaries are fine.
    void lumen_tag(std::string_view key, std::string_view value);

    [[nodiscard]] const TagSet<8>& tags() const { return tags_; }

    [[nodiscard]] Scope lumen_scope();

    [[nodiscard]] static const TagSet<8>* current_instance_tags();

    virtual ~Tagged() = default;

private:
    void rebuild_tags_();

    std::array<std::string, 8> keys_;
    std::array<std::string, 8> values_;
    size_t count_ = 0;
    TagSet<8> tags_;  // views into keys_/values_
};

}  // namespace lumen

#endif
