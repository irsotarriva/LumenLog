#ifndef LUMEN_SRC_TERMINATION_HPP
#define LUMEN_SRC_TERMINATION_HPP

#include <exception>

// How Lumen ends the process after a fatal record (lumen/core.hpp, "Fatal
// records"), and how Lumen's own terminate handler (DashboardSink's) hands
// over to the handler installed before it.
//
// The C++ library's default terminate handler writes a message to standard
// error before it aborts (libstdc++: "terminate called without an active
// exception"; libc++abi likewise). When standard error blocks (a full pipe,
// a terminal stopped with Ctrl-S) that write never returns, and the process
// never ends. Its message says nothing when no exception is active, so in
// that case Lumen raises SIGABRT itself instead of calling that handler. A
// handler the program installed with std::set_terminate is still called.

namespace lumen::detail {

// Whether `handler` is the terminate handler that was installed when the
// library was loaded (before main), which is the C++ library's default
// unless the program replaced it during static initialisation.
[[nodiscard]] bool is_initial_terminate_handler(std::terminate_handler handler) noexcept;

// Ends the process after a fatal record: SIGABRT if the installed terminate
// handler is the initial one (raised directly rather than through
// std::abort(), whose sanitizer interceptors flush the C streams first),
// otherwise std::terminate().
[[noreturn]] void terminate_after_fatal() noexcept;

// What a terminate handler of Lumen's calls last, with the handler it
// replaced: that handler, unless it is the initial one and no exception is
// active, in which case SIGABRT as above. A null `previous` aborts too.
[[noreturn]] void hand_over_terminate(std::terminate_handler previous) noexcept;

}  // namespace lumen::detail

#endif
