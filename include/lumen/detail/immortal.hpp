#ifndef LUMEN_DETAIL_IMMORTAL_HPP
#define LUMEN_DETAIL_IMMORTAL_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace lumen::detail {

// Storage for an object that is constructed once and never destroyed. Used
// for the few process-wide objects that code may still reach during static
// destruction (a log statement in the destructor of a static object): those
// must outlive every other static, which only an object that is never
// destroyed can guarantee. The memory stays reachable, so leak checkers do
// not report it.
template <typename T>
    requires std::is_object_v<T>
class Immortal {
public:
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    explicit Immortal(Args&&... args)
        // The storage is raw bytes until T is constructed in it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        : object_(std::construct_at(reinterpret_cast<T*>(storage_.data()),
                                    std::forward<Args>(args)...)) {}
    ~Immortal() = default;  // deliberately does not destroy the object
    Immortal(const Immortal&) = delete;
    Immortal& operator=(const Immortal&) = delete;
    Immortal(Immortal&&) = delete;
    Immortal& operator=(Immortal&&) = delete;

    [[nodiscard]] T& get() noexcept { return *object_; }

private:
    alignas(T) std::array<std::byte, sizeof(T)> storage_;
    T* object_;  // non-owning: points into storage_
};

}  // namespace lumen::detail

#endif
