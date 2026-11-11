#include "termination.hpp"

#include <csignal>
#include <cstdlib>
#include <exception>

namespace lumen::detail {

namespace {

// Read during the library's static initialisation, before main. Constant
// from then on, so reading it needs no synchronisation.
const std::terminate_handler initial_terminate_handler = std::get_terminate();

// SIGABRT, as std::abort() raises it, without the sanitizer runtimes' abort()
// interceptors, which flush the C streams first and so block on a blocked
// standard error (glibc's own abort() flushes nothing). If a handler the
// program installed for SIGABRT returns, std::abort() ends the process.
[[noreturn]] void abort_without_flushing() noexcept {
    static_cast<void>(std::raise(SIGABRT));
    std::abort();
}

}  // namespace

bool is_initial_terminate_handler(std::terminate_handler handler) noexcept {
    return handler == initial_terminate_handler;
}

void terminate_after_fatal() noexcept {
    if (is_initial_terminate_handler(std::get_terminate())) {
        abort_without_flushing();
    }
    std::terminate();
}

void hand_over_terminate(std::terminate_handler previous) noexcept {
    if (previous == nullptr ||
        (is_initial_terminate_handler(previous) && std::current_exception() == nullptr)) {
        abort_without_flushing();
    }
    previous();
    std::abort();  // a terminate handler must not return; one that does ends here
}

}  // namespace lumen::detail
