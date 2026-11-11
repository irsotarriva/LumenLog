#include "dispatcher_thread.hpp"

#include <exception>
#include <memory>
#include <utility>

namespace lumen::detail {

DispatcherThread::~DispatcherThread() {
    detach();
}

std::expected<void, std::string> DispatcherThread::start(std::function<void()> body) {
    {
        const std::scoped_lock lock(mutex_);
        exited_ = false;
    }
    try {
        thread_ = std::thread([this, run = std::move(body)] {
            run();
            {
                const std::scoped_lock lock(mutex_);
                exited_ = true;
            }
            exited_cv_.notify_all();
        });
    } catch (const std::exception& e) {
        return std::unexpected(std::string(e.what()));
    }
    return {};
}

bool DispatcherThread::wait_for_exit(std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex_);
    return exited_cv_.wait_for(lock, limit, [this] { return exited_; });
}

void DispatcherThread::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DispatcherThread::detach() noexcept {
    if (thread_.joinable()) {
        thread_.detach();
    }
}

std::unique_lock<std::mutex> DispatcherThread::lock_for_fork() {
    return std::unique_lock(mutex_);
}

void DispatcherThread::reset_in_fork_child() noexcept {
    // Neither joined nor detached: both would act on a thread of the parent.
    std::construct_at(&thread_);
    std::construct_at(&exited_cv_);
    exited_ = true;
}

}  // namespace lumen::detail
