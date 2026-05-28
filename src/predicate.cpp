#include "lumen/predicate.h"

#include <memory>
#include <string>

namespace lumen {

struct Predicate::Node {
    virtual ~Node() = default;
    virtual bool evaluate(const TagSet<16>& tags, LogLevel level) const = 0;
    virtual std::unique_ptr<Node> clone() const = 0;
};

namespace {

struct AlwaysNode : Predicate::Node {
    bool evaluate(const TagSet<16>&, LogLevel) const override { return true; }
    std::unique_ptr<Predicate::Node> clone() const override {
        return std::make_unique<AlwaysNode>();
    }
};

struct NeverNode : Predicate::Node {
    bool evaluate(const TagSet<16>&, LogLevel) const override { return false; }
    std::unique_ptr<Predicate::Node> clone() const override {
        return std::make_unique<NeverNode>();
    }
};

struct LevelAtLeastNode : Predicate::Node {
    LogLevel min_level;
    explicit LevelAtLeastNode(LogLevel l) : min_level(l) {}
    bool evaluate(const TagSet<16>&, LogLevel level) const override {
        return level >= min_level;
    }
    std::unique_ptr<Predicate::Node> clone() const override {
        return std::make_unique<LevelAtLeastNode>(min_level);
    }
};

struct LevelEqualsNode : Predicate::Node {
    LogLevel target;
    explicit LevelEqualsNode(LogLevel l) : target(l) {}
    bool evaluate(const TagSet<16>&, LogLevel level) const override {
        return level == target;
    }
    std::unique_ptr<Predicate::Node> clone() const override {
        return std::make_unique<LevelEqualsNode>(target);
    }
};

struct TagEqualsNode : Predicate::Node {
    std::string key;
    std::string value;
    TagEqualsNode(std::string_view k, std::string_view v) : key(k), value(v) {}
    bool evaluate(const TagSet<16>& tags, LogLevel) const override {
        return tags.find(key) == value;
    }
    std::unique_ptr<Predicate::Node> clone() const override {
        return std::make_unique<TagEqualsNode>(key, value);
    }
};

struct TagExistsNode : Predicate::Node {
    std::string key;
    explicit TagExistsNode(std::string_view k) : key(k) {}
    bool evaluate(const TagSet<16>& tags, LogLevel) const override {
        return !tags.find(key).empty();
    }
    std::unique_ptr<Predicate::Node> clone() const override {
        return std::make_unique<TagExistsNode>(key);
    }
};

struct AndNode : Predicate::Node {
    std::unique_ptr<Predicate::Node> left;
    std::unique_ptr<Predicate::Node> right;
    bool evaluate(const TagSet<16>& tags, LogLevel level) const override {
        return left->evaluate(tags, level) && right->evaluate(tags, level);
    }
    std::unique_ptr<Predicate::Node> clone() const override {
        auto n = std::make_unique<AndNode>();
        n->left = left->clone();
        n->right = right->clone();
        return n;
    }
};

struct OrNode : Predicate::Node {
    std::unique_ptr<Predicate::Node> left;
    std::unique_ptr<Predicate::Node> right;
    bool evaluate(const TagSet<16>& tags, LogLevel level) const override {
        return left->evaluate(tags, level) || right->evaluate(tags, level);
    }
    std::unique_ptr<Predicate::Node> clone() const override {
        auto n = std::make_unique<OrNode>();
        n->left = left->clone();
        n->right = right->clone();
        return n;
    }
};

struct NotNode : Predicate::Node {
    std::unique_ptr<Predicate::Node> child;
    bool evaluate(const TagSet<16>& tags, LogLevel level) const override {
        return !child->evaluate(tags, level);
    }
    std::unique_ptr<Predicate::Node> clone() const override {
        auto n = std::make_unique<NotNode>();
        n->child = child->clone();
        return n;
    }
};

}  // namespace

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

bool Predicate::evaluate(const TagSet<16>& tags, LogLevel level) const {
    if (!__node) return true;
    return __node->evaluate(tags, level);
}

Predicate always() {
    return Predicate(std::make_unique<AlwaysNode>());
}

Predicate never() {
    return Predicate(std::make_unique<NeverNode>());
}

Predicate level_at_least(LogLevel min_level) {
    return Predicate(std::make_unique<LevelAtLeastNode>(min_level));
}

Predicate level_equals(LogLevel level) {
    return Predicate(std::make_unique<LevelEqualsNode>(level));
}

Predicate tag_equals(std::string_view key, std::string_view value) {
    return Predicate(std::make_unique<TagEqualsNode>(key, value));
}

Predicate tag_exists(std::string_view key) {
    return Predicate(std::make_unique<TagExistsNode>(key));
}

Predicate operator&&(const Predicate& a, const Predicate& b) {
    auto node = std::make_unique<AndNode>();
    node->left = a.__node ? a.__node->clone() : std::make_unique<AlwaysNode>();
    node->right = b.__node ? b.__node->clone() : std::make_unique<AlwaysNode>();
    return Predicate(std::move(node));
}

Predicate operator||(const Predicate& a, const Predicate& b) {
    auto node = std::make_unique<OrNode>();
    node->left = a.__node ? a.__node->clone() : std::make_unique<AlwaysNode>();
    node->right = b.__node ? b.__node->clone() : std::make_unique<AlwaysNode>();
    return Predicate(std::move(node));
}

Predicate operator!(const Predicate& p) {
    auto node = std::make_unique<NotNode>();
    node->child = p.__node ? p.__node->clone() : std::make_unique<AlwaysNode>();
    return Predicate(std::move(node));
}

}  // namespace lumen
