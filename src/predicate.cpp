#include "lumen/predicate.h"

namespace lumen {

struct Predicate::Node {
    virtual ~Node() = default;
    virtual bool evaluate(const TagSet<16>& tags, LogLevel level) const = 0;
    virtual std::unique_ptr<Node> clone() const = 0;
};

Predicate::Predicate(std::unique_ptr<Node> node) : __node(std::move(node)) {}
Predicate::~Predicate() = default;

Predicate::Predicate(const Predicate& other)
    : __node(other.__node ? other.__node->clone() : nullptr) {}

Predicate::Predicate(Predicate&&) noexcept = default;

Predicate& Predicate::operator=(const Predicate& other) {
    if (this != &other) {
        __node = other.__node ? other.__node->clone() : nullptr;
    }
    return *this;
}

Predicate& Predicate::operator=(Predicate&&) noexcept = default;

bool Predicate::evaluate(const TagSet<16>& /*tags*/, LogLevel /*level*/) const {
    return true;
}

Predicate always() { return Predicate{}; }
Predicate never() { return Predicate{}; }
Predicate level_at_least(LogLevel /*min_level*/) { return Predicate{}; }
Predicate level_equals(LogLevel /*level*/) { return Predicate{}; }
Predicate tag_equals(std::string_view /*key*/, std::string_view /*value*/) { return Predicate{}; }
Predicate tag_exists(std::string_view /*key*/) { return Predicate{}; }

Predicate operator&&(const Predicate& /*a*/, const Predicate& /*b*/) { return Predicate{}; }
Predicate operator||(const Predicate& /*a*/, const Predicate& /*b*/) { return Predicate{}; }
Predicate operator!(const Predicate& /*p*/) { return Predicate{}; }

}  // namespace lumen
