#ifndef LUMEN_TAGGED_H
#define LUMEN_TAGGED_H

#include <string_view>

#include "lumen/record.h"

namespace lumen {

class Tagged {
public:
    void lumen_tag(std::string_view key, std::string_view value);

    virtual ~Tagged() = default;

private:
    TagSet<8> __tags;
};

}  // namespace lumen

#endif
