#include "lumen/predicate.hpp"

#include "number_parse.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lumen {

using NodePtr = std::shared_ptr<const Predicate::Node>;

// Rationale: the set of node kinds is closed, so a node is a std::variant of
// plain structs instead of a class hierarchy, and evaluation is a std::visit
// instead of a virtual call per node. Nodes are immutable and held by
// shared_ptr<const Node>, so `a && b` allocates one node pointing at the
// existing operands instead of copying them, and copies of a Predicate share
// one tree. A flat array of nodes would evaluate faster but could not be
// combined without copying both operands, which makes chains quadratic.
struct Predicate::Node {
    struct Constant {
        bool value;
    };
    struct LevelCompare {
        CompareOp op;
        LogLevel level;
    };
    struct TagText {
        std::string key;
        CompareOp op;  // Equal or NotEqual
        std::string value;
    };
    struct TagExists {
        std::string key;
    };
    struct TagNumber {
        std::string key;
        CompareOp op;
        double value;
    };
    struct Not {
        NodePtr operand;
    };
    // In a junction, the right operand is looked at only when the left one
    // does not already give the junction's `decisive` result.
    struct And {
        static constexpr bool decisive = false;
        NodePtr left;
        NodePtr right;
    };
    struct Or {
        static constexpr bool decisive = true;
        NodePtr left;
        NodePtr right;
    };

    using Body = std::variant<Constant, LevelCompare, TagText, TagExists, TagNumber, Not, And, Or>;

    explicit Node(Body node_body);
    ~Node();
    Node(const Node&) = delete;
    Node(Node&&) = delete;
    Node& operator=(const Node&) = delete;
    Node& operator=(Node&&) = delete;

    Body body;
    // Levels in the tree rooted here (1 for a leaf): the evaluation stack size.
    uint32_t depth;

private:
    [[nodiscard]] std::array<NodePtr, 2> take_children_();
};

namespace {

template <typename T>
concept Junction = std::same_as<T, Predicate::Node::And> || std::same_as<T, Predicate::Node::Or>;

}  // namespace

namespace detail {

// The single place that may construct a Predicate from a node or read its node.
struct PredicateAccess {
    [[nodiscard]] static Predicate make(Predicate::Node::Body body) {
        return Predicate(std::make_shared<const Predicate::Node>(std::move(body)));
    }
    // A default-constructed Predicate has no node and behaves as always().
    [[nodiscard]] static NodePtr node_or_always(const Predicate& predicate) {
        if (predicate.node_) {
            return predicate.node_;
        }
        return std::make_shared<const Predicate::Node>(Predicate::Node::Constant{true});
    }
};

}  // namespace detail

namespace {

using detail::PredicateAccess;
using Node = Predicate::Node;

[[nodiscard]] uint32_t depth_of(const Node::Body& body) {
    return std::visit(
        []<typename T>(const T& alternative) -> uint32_t {
            if constexpr (Junction<T>) {
                return 1 + std::max(alternative.left->depth, alternative.right->depth);
            } else if constexpr (std::same_as<T, Node::Not>) {
                return 1 + alternative.operand->depth;
            } else {
                return 1;
            }
        },
        body);
}

// The children released by the outermost Node destructor running on this
// thread, or null when none is running. Only Node::~Node uses it. Without it,
// destroying the last owner of a chain of n nodes would recurse n deep.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local std::vector<NodePtr>* release_queue = nullptr;  // non-owning: a local of ~Node

}  // namespace

Predicate::Node::Node(Body node_body) : body(std::move(node_body)), depth(depth_of(body)) {}

// A node under destruction is not const, so its children can be moved out.
std::array<NodePtr, 2> Predicate::Node::take_children_() {
    return std::visit(
        []<typename T>(T& alternative) -> std::array<NodePtr, 2> {
            if constexpr (Junction<T>) {
                return {std::move(alternative.left), std::move(alternative.right)};
            } else if constexpr (std::same_as<T, Not>) {
                return {std::move(alternative.operand), nullptr};
            } else {
                return {};
            }
        },
        body);
}

namespace {

void hand_over(std::array<NodePtr, 2>& children, std::vector<NodePtr>& queue) {
    for (NodePtr& child : children) {
        if (child) {
            queue.push_back(std::move(child));
        }
    }
}

}  // namespace

// Releasing a child can destroy it, which would destroy its children, and so
// on. Instead, a nested destructor hands its children to the outermost one,
// which releases them one at a time in a loop: the stack stays one level deep.
// A leaf pushes nothing, so destroying one allocates nothing; if a push
// cannot allocate, the program terminates, as for any noexcept function.
Predicate::Node::~Node() {  // NOLINT(bugprone-exception-escape): see above
    std::array<NodePtr, 2> children = take_children_();
    if (release_queue != nullptr) {
        hand_over(children, *release_queue);
        return;
    }
    std::vector<NodePtr> queue;
    hand_over(children, queue);
    release_queue = &queue;
    while (!queue.empty()) {
        NodePtr next = std::move(queue.back());
        queue.pop_back();
        next.reset();  // may push the released node's children onto `queue`
    }
    release_queue = nullptr;
}

namespace {

// true, false, or nullopt ("no opinion"): a level condition evaluated for a
// record that has no level (metrics, progress). A junction treats a
// no-opinion operand as absent, so for such records the level parts of a
// predicate are ignored and the rest of it decides.
using Verdict = std::optional<bool>;

template <std::totally_ordered T>
[[nodiscard]] bool compare(CompareOp op, const T& lhs, const T& rhs) {
    switch (op) {
        case CompareOp::Equal:        return lhs == rhs;
        case CompareOp::NotEqual:     return lhs != rhs;
        case CompareOp::Less:         return lhs < rhs;
        case CompareOp::LessEqual:    return lhs <= rhs;
        case CompareOp::Greater:      return lhs > rhs;
        case CompareOp::GreaterEqual: return lhs >= rhs;
    }
    return false;
}

// Every tag condition looks tags up here, so all of them follow one rule: the
// first entry with `key` is the tag's value.
[[nodiscard]] std::optional<std::string_view> find_tag(const RecordTags& tags,
                                                       std::string_view key) {
    for (const RecordTags::Entry& entry : tags) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return std::nullopt;
}

struct Frame {
    const Node* node = nullptr;  // non-owning: the root's owner keeps the tree alive
    uint8_t stage = 0;           // how many operands have been evaluated
    Verdict left;                // a junction's left result, once known
};

// Depth-first walk with an explicit stack of frames, one per level of the
// path from the root to the node being evaluated. Each step either pushes an
// operand of the top frame or pops it, leaving its verdict in `result_`.
class Walk {
public:
    Walk(const RecordTags& tags, std::optional<LogLevel> level, std::span<Frame> stack)
        : tags_(tags), level_(level), stack_(stack) {}

    [[nodiscard]] Verdict run(const Node& root) {
        push_(root);
        while (size_ > 0) {
            Frame& top = stack_[size_ - 1];
            std::visit([&](const auto& body) { step_(top, body); }, top.node->body);
        }
        return result_;
    }

private:
    void push_(const Node& node) { stack_[size_++] = Frame{&node, 0, std::nullopt}; }

    void finish_(Verdict verdict) {
        result_ = verdict;
        --size_;
    }

    void step_(Frame& /*frame*/, const Node::Constant& node) { finish_(node.value); }

    void step_(Frame& /*frame*/, const Node::LevelCompare& node) {
        if (!level_) {
            finish_(std::nullopt);
            return;
        }
        finish_(compare(node.op, *level_, node.level));
    }

    void step_(Frame& /*frame*/, const Node::TagText& node) {
        const std::optional<std::string_view> value = find_tag(tags_, node.key);
        finish_(value.has_value() && compare(node.op, *value, std::string_view(node.value)));
    }

    void step_(Frame& /*frame*/, const Node::TagExists& node) {
        finish_(find_tag(tags_, node.key).has_value());
    }

    void step_(Frame& /*frame*/, const Node::TagNumber& node) {
        const std::optional<std::string_view> text = find_tag(tags_, node.key);
        const std::optional<double> value = text ? detail::parse_double(*text) : std::nullopt;
        finish_(value.has_value() && compare(node.op, *value, node.value));
    }

    void step_(Frame& frame, const Node::Not& node) {
        if (frame.stage == 0) {
            frame.stage = 1;
            push_(*node.operand);
            return;
        }
        finish_(result_.has_value() ? Verdict(!*result_) : std::nullopt);
    }

    template <Junction J>
    void step_(Frame& frame, const J& node) {
        if (frame.stage == 0) {
            frame.stage = 1;
            push_(*node.left);
            return;
        }
        if (frame.stage == 1) {
            if (result_ == J::decisive) {
                finish_(J::decisive);
                return;
            }
            frame.left = result_;
            frame.stage = 2;
            push_(*node.right);
            return;
        }
        // The left operand was not decisive, so the right one decides, unless
        // it has no opinion.
        finish_(result_.has_value() ? result_ : frame.left);
    }

    // A Walk lives only inside one evaluate() call, on its stack, so it never
    // outlives the tags it reads and is never assigned.
    const RecordTags& tags_;
    std::optional<LogLevel> level_;
    std::span<Frame> stack_;
    std::size_t size_ = 0;
    Verdict result_;
};

// Trees up to this depth are walked with a stack on the C++ stack (16 bytes a
// frame), so ordinary predicates cost no allocation per record.
inline constexpr std::size_t inline_depth = 128;

[[nodiscard]] Verdict evaluate_tree(const Node& root, const RecordTags& tags,
                                    std::optional<LogLevel> level) {
    if (root.depth <= inline_depth) {
        std::array<Frame, inline_depth> frames{};
        return Walk(tags, level, frames).run(root);
    }
    std::vector<Frame> frames(root.depth);
    return Walk(tags, level, frames).run(root);
}

}  // namespace

Predicate::Predicate(std::shared_ptr<const Node> node) : node_(std::move(node)) {}

bool Predicate::evaluate(const RecordTags& tags, LogLevel level) const {
    if (!node_) {
        return true;
    }
    // Every condition has an opinion when the level is known.
    return evaluate_tree(*node_, tags, level).value_or(true);
}

bool Predicate::evaluate(const RecordTags& tags) const {
    if (!node_) {
        return true;
    }
    // Only level conditions can be undecided, and a predicate made only of
    // them places no constraint on a record without a level.
    return evaluate_tree(*node_, tags, std::nullopt).value_or(true);
}

Predicate always() {
    return PredicateAccess::make(Node::Constant{true});
}

Predicate never() {
    return PredicateAccess::make(Node::Constant{false});
}

Predicate level_compare(CompareOp op, LogLevel level) {
    return PredicateAccess::make(Node::LevelCompare{op, level});
}

Predicate level_at_least(LogLevel min_level) {
    return level_compare(CompareOp::GreaterEqual, min_level);
}

Predicate level_equals(LogLevel level) {
    return level_compare(CompareOp::Equal, level);
}

Predicate tag_equals(std::string_view key, std::string_view value) {
    return PredicateAccess::make(
        Node::TagText{std::string(key), CompareOp::Equal, std::string(value)});
}

Predicate tag_not_equals(std::string_view key, std::string_view value) {
    return PredicateAccess::make(
        Node::TagText{std::string(key), CompareOp::NotEqual, std::string(value)});
}

Predicate tag_exists(std::string_view key) {
    return PredicateAccess::make(Node::TagExists{std::string(key)});
}

Predicate tag_compare(std::string_view key, CompareOp op, double value) {
    return PredicateAccess::make(Node::TagNumber{std::string(key), op, value});
}

Predicate tag_less(std::string_view key, double value) {
    return tag_compare(key, CompareOp::Less, value);
}

Predicate tag_less_equal(std::string_view key, double value) {
    return tag_compare(key, CompareOp::LessEqual, value);
}

Predicate tag_greater(std::string_view key, double value) {
    return tag_compare(key, CompareOp::Greater, value);
}

Predicate tag_greater_equal(std::string_view key, double value) {
    return tag_compare(key, CompareOp::GreaterEqual, value);
}

Predicate operator&&(const Predicate& lhs, const Predicate& rhs) {
    return PredicateAccess::make(
        Node::And{PredicateAccess::node_or_always(lhs), PredicateAccess::node_or_always(rhs)});
}

Predicate operator||(const Predicate& lhs, const Predicate& rhs) {
    return PredicateAccess::make(
        Node::Or{PredicateAccess::node_or_always(lhs), PredicateAccess::node_or_always(rhs)});
}

Predicate operator!(const Predicate& operand) {
    return PredicateAccess::make(Node::Not{PredicateAccess::node_or_always(operand)});
}

}  // namespace lumen
