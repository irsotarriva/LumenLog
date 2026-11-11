#ifndef LUMEN_DETAIL_STRING_SCRATCH_HPP
#define LUMEN_DETAIL_STRING_SCRATCH_HPP

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <format>
#include <forward_list>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace lumen::detail {

// A type std::format can format: its std::formatter is enabled. An enabled
// specialization is default constructible and copyable, a disabled one is
// neither, which is the test std::formattable makes (not every standard
// library supported here provides std::formattable yet).
template <typename T>
concept FormatArgument = std::semiregular<std::formatter<std::remove_cvref_t<T>, char>>;

// Copies of the strings handed to one builder, kept until the builder submits
// its record. Tag arguments are often temporaries that die before the end of
// the logging statement, so they are copied as soon as they are passed in.
// Short strings go to an inline buffer on the builder's stack; longer ones go
// to list nodes, whose addresses never change. Returned views stay valid for
// the scratch object's lifetime.
//
// Cost. Storing a string allocates nothing while the strings stored so far
// fit in the inline buffer (inline_capacity bytes in all). Each string that
// does not fit costs two heap allocations: its list node and the string's
// own buffer. A formatted message that does not fit is formatted twice, the
// second time into a buffer of the size the first pass measured.
class StringScratch {  // NOLINT(cppcoreguidelines-pro-type-member-init): inline_buf_
public:
    static constexpr std::size_t inline_capacity = 512;

    StringScratch() = default;  // NOLINT(cppcoreguidelines-pro-type-member-init): inline_buf_
    ~StringScratch() = default;
    StringScratch(const StringScratch&) = delete;
    StringScratch& operator=(const StringScratch&) = delete;
    StringScratch(StringScratch&&) = delete;
    StringScratch& operator=(StringScratch&&) = delete;

    [[nodiscard]] std::string_view store(std::string_view s) {
        if (s.size() <= inline_capacity - used_) {
            char* const dst = inline_buf_.data() + used_;
            if (!s.empty()) {
                std::memcpy(dst, s.data(), s.size());
            }
            used_ += s.size();
            return {dst, s.size()};
        }
        overflow_.emplace_front(s);
        return overflow_.front();
    }

    // Integers exactly; floating point as the shortest text that parses back
    // to the same value, so numeric predicates see the value that was logged.
    template <typename T>
        requires(std::integral<T> || std::floating_point<T>)
    [[nodiscard]] std::string_view store_number(T value) {
        std::array<char, 32> buf{};
        const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
        // 32 characters hold every 64-bit integer and every double in
        // shortest form, so to_chars cannot fail here.
        if (ec != std::errc{}) {
            return {};
        }
        return store(std::string_view(buf.data(), static_cast<std::size_t>(end - buf.data())));
    }

    // std::format(fmt, args...), stored like store() would store the result.
    // Formatting can still fail at run time (std::format_error for a dynamic
    // width that is not a valid integer; anything a user's formatter
    // throws). Such an exception does not leave this function: the stored
    // text then says that formatting failed, with the exception's text and
    // the format string. std::bad_alloc from storing that text is the only
    // exception that can leave it.
    template <FormatArgument... Args>
    [[nodiscard]] std::string_view store_formatted(std::format_string<Args...> fmt,
                                                   Args&&... args) {
        try {
            return format_(fmt, std::forward<Args>(args)...);
        } catch (const std::exception& e) {
            return store_format_failure_(fmt.get(), e.what());
        } catch (...) {
            return store_format_failure_(fmt.get(), "an exception that is not a std::exception");
        }
    }

private:
    template <FormatArgument... Args>
    [[nodiscard]] std::string_view format_(std::format_string<Args...> fmt, Args&&... args) {
        const std::size_t available = inline_capacity - used_;
        char* const dst = inline_buf_.data() + used_;
        const auto result = std::format_to_n(dst, static_cast<std::ptrdiff_t>(available), fmt,
                                             std::forward<Args>(args)...);
        const auto size = static_cast<std::size_t>(result.size);
        if (size <= available) {
            used_ += size;
            return {dst, size};
        }
        // Too long for the inline buffer: format again, into a string reserved
        // at the measured size (it still grows if a user formatter writes
        // more the second time), then into a list node.
        std::string text;
        text.reserve(size);
        std::vformat_to(std::back_inserter(text), fmt.get(), std::make_format_args(args...));
        overflow_.emplace_front(std::move(text));
        return overflow_.front();
    }

    [[nodiscard]] std::string_view store_format_failure_(std::string_view fmt,
                                                         std::string_view what) {
        std::string text = "formatting failed: ";
        text += what;
        text += "; format string: ";
        text += fmt;
        return store(text);
    }

    // Left uninitialised on purpose: only the first used_ bytes are ever
    // read, and zeroing 512 bytes per logging statement is measurable.
    std::array<char, inline_capacity> inline_buf_;
    std::size_t used_ = 0;
    std::forward_list<std::string> overflow_;
};

}  // namespace lumen::detail

#endif
