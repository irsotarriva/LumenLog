#ifndef LUMEN_MACROS_HPP
#define LUMEN_MACROS_HPP

#include <exception>
#include <type_traits>
#include <utility>

#include "lumen/config.hpp"
#include "lumen/context.hpp"
#include "lumen/core.hpp"
#include "lumen/detail/scope_stack.hpp"
#include "lumen/level.hpp"
#include "lumen/record_builder.hpp"

// The logging front end. Everything here goes to the global lumen::core().
//
// Log statements
//
//   LUMEN_LOG_INFO("plain message");                 // one argument: logged verbatim
//   LUMEN_LOG_INFO("energy {:.2f} GeV", energy);     // std::format, checked at compile time
//   LUMEN_LOG_INFO("done").tag("n", 42).tag("ok", true);
//
// LUMEN_LOG_TRACE ... LUMEN_LOG_ERROR are statements, not expressions: each
// expands to an `if constexpr` whose branch builds a RecordBuilder
// (lumen/record_builder.hpp: tag rules, when the record is submitted), so
// `.tag(...)` may follow the closing parenthesis and the whole is ended by
// `;`. A log statement is safe as the body of an `if` without braces, with
// or without `else`:
//
//   if (converged) LUMEN_LOG_INFO("done"); else LUMEN_LOG_WARN("not yet");
//
// It cannot be used where an expression is needed (an initializer, an
// operand of `,` or `?:`, a function argument).
//
// Messages. A message given alone is logged verbatim; a null `const char*`
// is logged as "(null)". A formatted message that fails at run time (a
// dynamic width that is not a valid integer, a user formatter that throws)
// does not throw out of the statement: the record is still sent, and its
// message says that formatting failed, with the format string and the
// exception's text.
//
// LUMEN_LOG_FATAL("...", args...) is a statement that delivers the record,
// flushes the sinks and terminates the program (exact semantics: "Fatal" in
// lumen/record_builder.hpp). It takes no .tag().
//
// Compile-time threshold. Statements below LUMEN_MIN_LEVEL_VALUE are
// discarded at compile time: they emit nothing and their arguments are not
// evaluated (they sit in the discarded branch of an `if constexpr`).
// LUMEN_MIN_LEVEL_VALUE is the numeric LogLevel (0 = Trace ... 5 = Fatal);
// CMake's LUMEN_MIN_LEVEL sets it from a level name, in lumen/config.hpp. A
// number rather than a name, so that a consumer's own DEBUG or ERROR macro
// cannot change its meaning. A translation unit that defines it before its
// first Lumen include uses its own threshold. Fatal is never discarded.
//
// Scopes
//
//   LUMEN_SCOPE("run", run_id);       // adds run=<run_id> to records until the end of the block
//   LUMEN_MEMBER_SCOPE;               // in a member function of a lumen::Tagged
//
//   LUMEN_LOOP("epoch", e, n_epochs) { ... }
//
// LUMEN_LOOP is a `for` loop over e = 0 .. n_epochs - 1; `e` has the type of
// `n_epochs` (any integer type) and is const. While a body runs, records carry
// the scope tag epoch=<e>; nested loops with different labels each add their
// own tag. Nested loops with the same label follow the rule of any two scopes
// with the same key: the inner one wins, so while the inner loop's body runs
// the records carry the inner index, and the outer index again once the
// inner loop has ended. Give nested loops different labels to see both.
// The loop drives a progress bar labelled "epoch" (ProgressHandle,
// lumen/progress.hpp) that counts completed iterations:
//   - 0/n is sent before the first body, k/n after the k-th body completes
//     (a `continue` completes the body), and n/n after the last;
//   - with n == 0 (or negative), the single record 0/0 is sent;
//   - leaving early (`break`, `return`, an exception) sends nothing more, so
//     the last record is the number of bodies that completed.
// The progress records are sent between bodies, so they do not carry the
// loop's own epoch=<e> tag (they hold the count already); they do carry the
// rest of the context, including the tag of an enclosing loop.
// The label and the total are evaluated once. The loop variable cannot be
// changed by the body, and `break`/`continue` refer to the LUMEN_LOOP itself.
// What a macro cannot fix: `var` must be a plain name, and each argument
// must not contain an unparenthesised comma.
//
// Scope rules (LUMEN_SCOPE, LUMEN_LOOP, LUMEN_MEMBER_SCOPE). The guards are
// local objects and must be destroyed on the thread that created them, in
// reverse order of creation, which is what block scope does. Moving a guard's
// lifetime across threads (coroutines resumed elsewhere, tasks handed to a
// pool) is not supported; debug builds assert on it. Release builds keep
// memory safe and get the context wrong in a bounded way: a guard destroyed
// on another thread leaves its tag on its own thread's records until that
// thread leaves an enclosing scope (or exits), and a guard destroyed out of
// order also ends the scopes created after it.

#ifdef LUMEN_MIN_LEVEL
#error \
    "LUMEN_MIN_LEVEL is the CMake option; define LUMEN_MIN_LEVEL_VALUE (0 = Trace ... 5 = Fatal) instead"
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
// `if (c) LUMEN_LOG_INFO("x"); else ...` binds the `else` to their own `if`.
#define LUMEN_IF_ENABLED(LEVEL, expr)                 \
    if constexpr (!lumen::level_compiled_in(LEVEL)) { \
    } else                                            \
        expr

// lumen::call_site() is evaluated by the compiler, so a log statement does
// not look up its class name at run time.
#define LUMEN_LOG_AT(LEVEL, ...) \
    LUMEN_IF_ENABLED(LEVEL,      \
                     lumen::RecordBuilder(lumen::core(), LEVEL, lumen::call_site(), __VA_ARGS__))

#define LUMEN_LOG_TRACE(...) LUMEN_LOG_AT(lumen::LogLevel::Trace, __VA_ARGS__)
#define LUMEN_LOG_DEBUG(...) LUMEN_LOG_AT(lumen::LogLevel::Debug, __VA_ARGS__)
#define LUMEN_LOG_INFO(...) LUMEN_LOG_AT(lumen::LogLevel::Info, __VA_ARGS__)
#define LUMEN_LOG_WARN(...) LUMEN_LOG_AT(lumen::LogLevel::Warn, __VA_ARGS__)
#define LUMEN_LOG_ERROR(...) LUMEN_LOG_AT(lumen::LogLevel::Error, __VA_ARGS__)

// The builder's destructor terminates; the std::terminate() after it is never
// reached and only tells the compiler that the statement does not return.
#define LUMEN_LOG_FATAL(...)                                                            \
    do {                                                                                \
        lumen::RecordBuilder(lumen::core(), lumen::LogLevel::Fatal, lumen::call_site(), \
                             __VA_ARGS__);                                              \
        std::terminate();                                                               \
    } while (false)

// Short names, for code that does not also use another library's LOG_*.
#ifndef LUMEN_NO_SHORT_MACROS
#define LOG_TRACE(...) LUMEN_LOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) LUMEN_LOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...) LUMEN_LOG_INFO(__VA_ARGS__)
#define LOG_WARN(...) LUMEN_LOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) LUMEN_LOG_ERROR(__VA_ARGS__)
#define LOG_FATAL(...) LUMEN_LOG_FATAL(__VA_ARGS__)
#endif

#define LUMEN_SCOPE(key, value) \
    const lumen::ScopedTag LUMEN_CONCAT(lumen_scope_, __COUNTER__)(key, value)

#define LUMEN_MEMBER_SCOPE const auto LUMEN_CONCAT(lumen_member_, __COUNTER__) = lumen_scope()

#define LUMEN_LOOP(label, var, total) \
    LUMEN_LOOP_IMPL(label, var, total, LUMEN_CONCAT(lumen_loop_, __COUNTER__))

// The `if (...; false) {} else body` form gives the body a const loop
// variable and already has its `else`, so an `else` after the loop body
// binds to the user's own `if`. `var` and `scope` name variables, which
// cannot be parenthesised.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define LUMEN_LOOP_IMPL(label, var, total, scope)                                            \
    for (lumen::detail::LoopScope<std::remove_cvref_t<decltype(total)>> scope(lumen::core(), \
                                                                              label, total); \
         scope.advance();)                                                                   \
        if ([[maybe_unused]] const auto var = scope.index(); false) {                        \
        } else
// NOLINTEND(bugprone-macro-parentheses)

#endif
