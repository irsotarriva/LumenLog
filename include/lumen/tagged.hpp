#ifndef LUMEN_TAGGED_HPP
#define LUMEN_TAGGED_HPP

#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include "lumen/detail/context.hpp"

namespace lumen {

// A base class that gives an object its own tags, which records produced
// inside its member scopes carry (the "instance" layer of lumen/context.hpp):
//
//   class JetFinder : public lumen::Tagged {
//   public:
//       explicit JetFinder(uint32_t run) {
//           static_cast<void>(lumen_tag("run", std::to_string(run)));
//       }
//       void process() {
//           LUMEN_MEMBER_SCOPE;              // records below carry run=<run>
//           LUMEN_LOG_INFO("processing");
//       }
//   };
//
// Cost. sizeof(Tagged) is one pointer. An object that has never been tagged
// owns nothing else; the first lumen_tag() allocates its table. There are no
// virtual functions: the destructor is protected, so a Tagged cannot be
// deleted through a pointer to the base.
//
// Copy and move. A copy gets its own copy of the tags; a moved-from object
// has no tags.
//
// Scopes. lumen_scope() (LUMEN_MEMBER_SCOPE) makes the object's tags part of
// the context of the calling thread until the returned guard is destroyed.
// The tags are read when each record is submitted, so a lumen_tag() inside the
// scope applies to the records after it. Scopes nest: the innermost object's
// tags win. The guard follows the scope rules of lumen/macros.hpp (destroyed
// on the thread that created it, in reverse order), and the object must
// outlive the guard.
//
// Threads. A Tagged is not synchronised, like any other object: lumen_tag()
// and copying or moving the object must not run while another thread is
// inside one of the object's member scopes or tagging it. Several threads may
// be inside member scopes of the same object at once, as long as none of them
// tags it.
class Tagged {
public:
    // Makes this object's tags part of the calling thread's context until it
    // is destroyed. Not copyable or movable: it lives where it was created.
    class Scope {
    public:
        explicit Scope(const Tagged& object) noexcept;
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

    private:
        detail::InstanceFrame frame_;
    };

    // Sets `key` to `value` on this object, replacing an existing value. Both
    // are copied. Fails with LumenError::TagTableFull when `key` is new and
    // the object already has max_instance_tags keys; the tags are then
    // unchanged.
    [[nodiscard]] std::expected<void, std::error_code> lumen_tag(std::string_view key,
                                                                 std::string_view value);

    // Removes `key`. Returns false if the object did not have it.
    [[nodiscard]] bool lumen_untag(std::string_view key);

    // This object's tags, in the order they were first set.
    [[nodiscard]] std::span<const detail::OwnedTag> lumen_tags() const noexcept;

    [[nodiscard]] Scope lumen_scope() const noexcept { return Scope(*this); }

protected:
    Tagged() = default;
    ~Tagged() = default;
    Tagged(const Tagged& other);
    Tagged& operator=(const Tagged& other);
    Tagged(Tagged&& other) noexcept = default;
    Tagged& operator=(Tagged&& other) noexcept = default;

private:
    std::unique_ptr<std::vector<detail::OwnedTag>> tags_;  // null until first tagged
};

}  // namespace lumen

#endif
