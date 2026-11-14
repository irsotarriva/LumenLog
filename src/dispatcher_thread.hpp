#ifndef LUMEN_SRC_DISPATCHER_THREAD_HPP
#define LUMEN_SRC_DISPATCHER_THREAD_HPP

#include <chrono>
#include <condition_variable>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace lumen::detail {

// A Core's dispatch thread as a thread: starting it, and ending it in a way
// that can give up. Shutdown must not wait forever for a thread stuck in a
// sink callback, and std::thread::join cannot be given a time limit, so the
// thread says when its body has returned and the joiner waits for that with
// a limit before joining.
class DispatcherThread {
public:
    DispatcherThread() = default;
    // A Core joins or detaches its thread before it is destroyed. Should a
    // thread still be running here, it is detached rather than joined (which
    // could wait forever) or left joinable (which would terminate).
    ~DispatcherThread();
    DispatcherThread(const DispatcherThread&) = delete;
    DispatcherThread& operator=(const DispatcherThread&) = delete;
    DispatcherThread(DispatcherThread&&) = delete;
    DispatcherThread& operator=(DispatcherThread&&) = delete;

    // Starts `body` on a new thread. Fails, with what std::thread threw
    // (for instance when the system has no thread to spare), if it cannot.
    [[nodiscard]] std::expected<void, std::string> start(std::function<void()> body);

    // Whether a thread was started and is neither joined nor detached.
    [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

    // Waits at most `limit` for the body to return; true once it has.
    [[nodiscard]] bool wait_for_exit(std::chrono::milliseconds limit);

    // After wait_for_exit returned true.
    void join();

    // Leaves the thread to end with the process.
    void detach() noexcept;

    // Fork handlers (src/fork_handlers.hpp). The thread does not exist in
    // the child: its handle is forgotten without being joined or detached.
    [[nodiscard]] std::unique_lock<std::mutex> lock_for_fork();
    void reset_in_fork_child() noexcept;

private:
    std::mutex mutex_;
    std::condition_variable exited_cv_;  // waits on mutex_
    bool exited_ = false;                // guarded by mutex_: the body has returned
    // std::thread, not std::jthread: Apple's libc++ does not provide jthread.
    std::thread thread_;
};

}  // namespace lumen::detail

#endif
