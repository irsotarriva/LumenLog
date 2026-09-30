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
        const TagSet<8>* __previous;  // non-owning
    };

    Tagged() = default;
    Tagged(const Tagged& other);
    Tagged& operator=(const Tagged& other);
    Tagged(Tagged&& other) noexcept;
    Tagged& operator=(Tagged&& other) noexcept;

    // Copies key and value; temporaries are fine.
    void lumen_tag(std::string_view key, std::string_view value);

    [[nodiscard]] const TagSet<8>& tags() const { return __tags; }

    [[nodiscard]] Scope lumen_scope();

    [[nodiscard]] static const TagSet<8>* current_instance_tags();

    virtual ~Tagged() = default;

private:
    void __rebuild_tags();

    std::array<std::string, 8> __keys;
    std::array<std::string, 8> __values;
    size_t __count = 0;
    TagSet<8> __tags;  // views into __keys/__values
};

}  // namespace lumen

#endif
