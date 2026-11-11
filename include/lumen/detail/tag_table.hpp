#ifndef LUMEN_DETAIL_TAG_TABLE_HPP
#define LUMEN_DETAIL_TAG_TABLE_HPP

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include "lumen/tag_set.hpp"

namespace lumen::detail {

// The storage of every context layer that owns its strings: the process
// table, each thread's tags and each Tagged object (defined in
// src/context.cpp). Keys are distinct and kept in the order they were first
// set; there are at most max_size of them.
class TagTable {
public:
    explicit TagTable(std::size_t max_size) noexcept : max_size_(max_size) {}

    // Sets `key` to `value`, replacing an existing value. Fails with
    // LumenError::TagTableFull when `key` is new and the table already holds
    // max_size keys; the table is then unchanged.
    [[nodiscard]] std::expected<void, std::error_code> set(std::string_view key,
                                                           std::string_view value);

    // Removes `key`. Returns false if it was not there.
    [[nodiscard]] bool remove(std::string_view key);

    void clear() noexcept { tags_.clear(); }

    [[nodiscard]] std::span<const OwnedTag> tags() const noexcept { return tags_; }

private:
    std::size_t max_size_;
    std::vector<OwnedTag> tags_;
};

}  // namespace lumen::detail

#endif
