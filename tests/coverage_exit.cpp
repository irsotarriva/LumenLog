// Linked into every test program of a coverage build (LUMEN_COVERAGE). GCC
// writes a program's coverage counts when it exits normally. The code under
// test also ends processes by SIGABRT (fatal records) and by _exit() (the
// children the fork tests start), and those would leave what they ran
// uncounted. This file writes the counts on those two ways out as well.
#include <sys/syscall.h>
#include <unistd.h>

#include <csignal>

// GCC's <gcov.h> declares its functions without C linkage for C++.
extern "C" {
#include <gcov.h>
}

namespace {

// Writes the counts, then lets the signal end the process as it would have.
// __gcov_dump is not async-signal-safe; the aborts measured here are raised
// by the aborting thread itself, outside any allocation or stream lock.
extern "C" void write_counts_then_abort(int /*signal*/) {
    __gcov_dump();
    static_cast<void>(std::signal(SIGABRT, SIG_DFL));
    static_cast<void>(std::raise(SIGABRT));
}

// Installed before main. A test that installs its own SIGABRT handler
// replaces this one, and then only that process's counts are missing.
const auto previous_abort_handler = std::signal(SIGABRT, &write_counts_then_abort);

[[noreturn]] void write_counts_then_leave(int status) noexcept {
    __gcov_dump();
    static_cast<void>(syscall(SYS_exit_group, status));
    __builtin_unreachable();  // exit_group does not return
}

}  // namespace

// Replace the C library's functions for this program: the library under test
// and the tests call them by these names.
extern "C" [[noreturn]] void _exit(int status) {
    write_counts_then_leave(status);
}

extern "C" [[noreturn]] void _Exit(int status) {
    write_counts_then_leave(status);
}
