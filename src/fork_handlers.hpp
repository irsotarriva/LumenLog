#ifndef LUMEN_SRC_FORK_HANDLERS_HPP
#define LUMEN_SRC_FORK_HANDLERS_HPP

#include <functional>

// What the library's objects with locks or threads do around fork(), so that
// the child can go on using them (lumen/core.hpp, "fork()", states the
// behaviour for users). POSIX only; on Windows, which has no fork, a
// registration does nothing.
//
// One pthread_atfork registration, made with the first ForkRegistration,
// calls every registered object:
//   - prepare, in the parent before the fork, in registration order: takes
//     the object's short-held locks, so that no other thread is half-way
//     through changing what they guard when the process is copied. Only
//     locks that are never held while waiting for anything else may be
//     taken here (a lock held across a sink callback could be held by a
//     thread waiting for the forking thread, a Python thread holding the
//     GIL for instance);
//   - parent, in the parent after the fork, in reverse order: releases them;
//   - child, in the child after the fork, in reverse order: puts the object
//     in the state the child needs (its threads do not exist there: what
//     they held or were doing is abandoned) and releases the locks.
// The list itself is locked from the first prepare to the last parent or
// child, so objects that register or unregister on other threads meanwhile
// wait.
//
// What a child handler may do. Strictly, after fork() in a multithreaded
// process only async-signal-safe functions may be called until exec. The
// child handlers stay close to that: they store to memory and atomics,
// re-initialise mutexes and condition variables that no thread of the child
// can be using, and release memory (free), which glibc and macOS support in
// the child. Starting threads is left to the first use in the child.

namespace lumen::detail {

struct ForkHandlers {
    std::function<void()> prepare;
    std::function<void()> parent;
    std::function<void()> child;
};

// Registers `handlers` for its lifetime. The handlers must not throw.
class ForkRegistration {
public:
    explicit ForkRegistration(ForkHandlers handlers);
    ~ForkRegistration();
    ForkRegistration(const ForkRegistration&) = delete;
    ForkRegistration& operator=(const ForkRegistration&) = delete;
    ForkRegistration(ForkRegistration&&) = delete;
    ForkRegistration& operator=(ForkRegistration&&) = delete;

private:
    friend class ForkList;
    const ForkHandlers handlers_;
};

}  // namespace lumen::detail

#endif
