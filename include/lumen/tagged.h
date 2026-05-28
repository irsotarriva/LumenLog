#ifndef LUMEN_TAGGED_H
#define LUMEN_TAGGED_H

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
        const TagSet<8>* __previous;
    };

    void lumen_tag(std::string_view key, std::string_view value);

    [[nodiscard]] const TagSet<8>& tags() const { return __tags; }

    [[nodiscard]] Scope lumen_scope();

    [[nodiscard]] static const TagSet<8>* current_instance_tags();

    virtual ~Tagged() = default;

private:
    TagSet<8> __tags;
};

}  // namespace lumen

#endif
