#ifndef LUMEN_MACROS_HPP
#define LUMEN_MACROS_HPP

#include <cstdint>
#include <exception>
#include <utility>

#include "lumen/core.hpp"
#include "lumen/detail/scope_stack.hpp"
#include "lumen/record.hpp"

// ── Compile-time level threshold ──────────────────────────────────────────────
//
// LUMEN_MIN_LEVEL_VALUE is the numeric value of the lowest LogLevel that is
// compiled in (0 = Trace ... 5 = Fatal). The CMake option LUMEN_MIN_LEVEL sets
// it from a level name. A number rather than a name, so that a consumer's own
// DEBUG or ERROR macro cannot change its meaning.

#ifdef LUMEN_MIN_LEVEL
#error "LUMEN_MIN_LEVEL is the CMake option; define LUMEN_MIN_LEVEL_VALUE (0 = Trace ... 5 = Fatal) instead"
#endif

#ifndef LUMEN_MIN_LEVEL_VALUE
#define LUMEN_MIN_LEVEL_VALUE 1
#endif

static_assert(LUMEN_MIN_LEVEL_VALUE >= 0 && LUMEN_MIN_LEVEL_VALUE <= 5,
              "LUMEN_MIN_LEVEL_VALUE must be between 0 (Trace) and 5 (Fatal)");

namespace lumen {

inline constexpr LogLevel compiled_min_level = static_cast<LogLevel>(LUMEN_MIN_LEVEL_VALUE);

[[nodiscard]] consteval bool level_compiled_in(LogLevel level) {
    return std::to_underlying(level) >= std::to_underlying(compiled_min_level);
}

}  // namespace lumen

#define LUMEN_CONCAT_IMPL(a, b) a##b
#define LUMEN_CONCAT(a, b) LUMEN_CONCAT_IMPL(a, b)

// Written as `if constexpr (!enabled) {} else expr` so that a user's
// `if (c) LOG_INFO("x"); else ...` binds the `else` to their own `if`.
#define LUMEN_IF_ENABLED(LEVEL, expr) \
    if constexpr (!lumen::level_compiled_in(LEVEL)) {} else expr

#define LUMEN_LOG_AT(LEVEL, ...) \
    LUMEN_IF_ENABLED(LEVEL, \
        lumen::RecordBuilder(lumen::core().log_buffer(), LEVEL, \
                             lumen::detail::format_message(__VA_ARGS__)))

// ── Log macros ────────────────────────────────────────────────────────────────
//
//   LUMEN_LOG_INFO("plain message");                 // logged verbatim
//   LUMEN_LOG_INFO("energy {:.2f} GeV", energy);     // std::format, checked at compile time
//   LUMEN_LOG_INFO("done").tag("n", int64_t{42});    // chain tags

#define LUMEN_LOG_TRACE(...) LUMEN_LOG_AT(lumen::LogLevel::Trace, __VA_ARGS__)
#define LUMEN_LOG_DEBUG(...) LUMEN_LOG_AT(lumen::LogLevel::Debug, __VA_ARGS__)
#define LUMEN_LOG_INFO(...)  LUMEN_LOG_AT(lumen::LogLevel::Info, __VA_ARGS__)
#define LUMEN_LOG_WARN(...)  LUMEN_LOG_AT(lumen::LogLevel::Warn, __VA_ARGS__)
#define LUMEN_LOG_ERROR(...) LUMEN_LOG_AT(lumen::LogLevel::Error, __VA_ARGS__)

#define LUMEN_LOG_FATAL(...) do { \
    { \
        lumen::RecordBuilder LUMEN_CONCAT(lumen_fatal_, __LINE__) \
            (lumen::core().log_buffer(), lumen::LogLevel::Fatal, \
             lumen::detail::format_message(__VA_ARGS__)); \
    } \
    lumen::core().flush(); \
    std::terminate(); \
} while(0)

// Short names, for code that does not also use another library's LOG_*.
#ifndef LUMEN_NO_SHORT_MACROS
#define LOG_TRACE(...) LUMEN_LOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) LUMEN_LOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)  LUMEN_LOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)  LUMEN_LOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) LUMEN_LOG_ERROR(__VA_ARGS__)
#define LOG_FATAL(...) LUMEN_LOG_FATAL(__VA_ARGS__)
#endif

// ── Scope macros ──────────────────────────────────────────────────────────────

#define LUMEN_SCOPE(key, value) \
    lumen::detail::ScopeGuard LUMEN_CONCAT(lumen_scope_, __LINE__)(key, value)

#define LUMEN_MEMBER_SCOPE \
    auto LUMEN_CONCAT(lumen_member_, __LINE__) = lumen_scope()

#define LUMEN_LOOP(label, var, total) \
    for (lumen::detail::LoopScope LUMEN_CONCAT(lumen_loop_, __LINE__)(label, lumen::core().progress_buffer(), static_cast<uint64_t>(total)); \
         LUMEN_CONCAT(lumen_loop_, __LINE__).advance(); ) \
        if (uint64_t var = LUMEN_CONCAT(lumen_loop_, __LINE__).current(); true)

#endif
