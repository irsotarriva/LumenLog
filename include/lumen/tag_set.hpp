#ifndef LUMEN_TAG_SET_HPP
#define LUMEN_TAG_SET_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace lumen {

// A fixed-capacity list of key/value pairs. It stores views only: whoever
// fills a TagSet keeps the strings alive (a record's tags point into the
// record's own storage, see lumen/record.hpp).
//
// Keys are not required to be unique. add() appends without looking, which
// is what a caller building a test input, or copying another set, wants;
// set() replaces the value of an existing key. Readers (find, contains and
// the predicates) use the first entry with a key. The builders and the
// context layers only ever use set(), so a record that went through Core
// never holds a key twice.
template <std::size_t N>
class TagSet {
public:
    struct Entry {
        std::string_view key;
        std::string_view value;
    };

    // Appends the pair. Returns false, and leaves the set unchanged, when it
    // already holds N entries.
    [[nodiscard]] constexpr bool add(std::string_view key, std::string_view value) {
        if (count_ == N) return false;
        entries_[count_++] = {key, value};
        return true;
    }

    // Replaces the value of the first entry with `key`, or appends the pair.
    // Returns false, and leaves the set unchanged, when the key is absent and
    // the set is full.
    [[nodiscard]] constexpr bool set(std::string_view key, std::string_view value) {
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].key == key) {
                entries_[i].value = value;
                return true;
            }
        }
        return add(key, value);
    }

    // The value of the first entry with `key`, or an empty view if there is
    // none. Use contains() to tell an empty value from a missing key.
    [[nodiscard]] constexpr std::string_view find(std::string_view key) const {
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].key == key) return entries_[i].value;
        }
        return {};
    }

    [[nodiscard]] constexpr bool contains(std::string_view key) const {
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].key == key) return true;
        }
        return false;
    }

    // Replaces every key and value by `f(view)`; used to re-point the views
    // at storage that the caller owns.
    template <std::invocable<std::string_view> F>
        requires std::convertible_to<std::invoke_result_t<F&, std::string_view>, std::string_view>
    constexpr void remap(F&& f) {
        for (std::size_t i = 0; i < count_; ++i) {
            entries_[i].key = f(entries_[i].key);
            entries_[i].value = f(entries_[i].value);
        }
    }

    [[nodiscard]] constexpr std::size_t count() const { return count_; }
    [[nodiscard]] static constexpr std::size_t capacity() { return N; }
    [[nodiscard]] constexpr bool full() const { return count_ == N; }
    [[nodiscard]] constexpr const Entry* begin() const { return entries_.data(); }
    [[nodiscard]] constexpr const Entry* end() const { return entries_.data() + count_; }

private:
    std::array<Entry, N> entries_{};
    std::size_t count_ = 0;
};

}  // namespace lumen

#endif
