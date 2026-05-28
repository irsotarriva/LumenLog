#ifndef LUMEN_PREDICATE_H
#define LUMEN_PREDICATE_H

#include <memory>
#include <string_view>

#include "lumen/record.h"

namespace lumen {

class Predicate {
public:
    bool evaluate(const TagSet<16>& tags, LogLevel level) const;

    Predicate() = default;
    Predicate(const Predicate& other);
    Predicate(Predicate&&) noexcept;
    Predicate& operator=(const Predicate& other);
    Predicate& operator=(Predicate&&) noexcept;
    ~Predicate();

    friend Predicate operator&&(const Predicate& a, const Predicate& b);
    friend Predicate operator||(const Predicate& a, const Predicate& b);
    friend Predicate operator!(const Predicate& p);

    friend Predicate always();
    friend Predicate never();
    friend Predicate level_at_least(LogLevel min_level);
    friend Predicate level_equals(LogLevel level);
    friend Predicate tag_equals(std::string_view key, std::string_view value);
    friend Predicate tag_exists(std::string_view key);

    struct Node;

private:
    explicit Predicate(std::unique_ptr<Node> node);
    std::unique_ptr<Node> __node;
};

Predicate always();
Predicate never();
Predicate level_at_least(LogLevel min_level);
Predicate level_equals(LogLevel level);
Predicate tag_equals(std::string_view key, std::string_view value);
Predicate tag_exists(std::string_view key);

}  // namespace lumen

#endif
