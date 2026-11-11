#include "fork_handlers.hpp"

#include <mutex>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include "lumen/detail/immortal.hpp"

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
#include <pthread.h>
#endif

namespace lumen::detail {

// The registered objects, and the pthread_atfork handlers that call them.
class ForkList {
public:
    // Never destroyed: a Core that is never destroyed stays registered, and
    // fork() may run during static destruction.
    [[nodiscard]] static ForkList& instance() {
        static Immortal<ForkList> list;
        return list.get();
    }

    void add(const ForkRegistration& registration) {
        const std::scoped_lock lock(mutex_);
        install_once_locked_();
        registered_.push_back(&registration);
    }

    void remove(const ForkRegistration& registration) noexcept {
        const std::scoped_lock lock(mutex_);
        std::erase(registered_, &registration);
    }

private:
    void install_once_locked_() {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        if (installed_) {
            return;
        }
        installed_ = true;
        // Fails only without memory for the registration (ENOMEM); fork()
        // then behaves as it did before this library existed in the
        // process, which is all that can be done about it.
        static_cast<void>(pthread_atfork(&prepare_all_, &parent_all_, &child_all_));
#endif
    }

    static void prepare_all_() noexcept {
        ForkList& list = instance();
        list.held_.emplace(list.mutex_);
        for (const ForkRegistration* const registration : list.registered_) {
            registration->handlers_.prepare();
        }
    }

    static void parent_all_() noexcept {
        ForkList& list = instance();
        for (const ForkRegistration* const registration : std::views::reverse(list.registered_)) {
            registration->handlers_.parent();
        }
        list.held_.reset();
    }

    static void child_all_() noexcept {
        ForkList& list = instance();
        for (const ForkRegistration* const registration : std::views::reverse(list.registered_)) {
            registration->handlers_.child();
        }
        list.held_.reset();
    }

    std::mutex mutex_;
    // Guarded by mutex_. Non-owning: each registration removes itself
    // before it is destroyed.
    std::vector<const ForkRegistration*> registered_;
    bool installed_ = false;  // guarded by mutex_
    // mutex_, held from prepare_all_ to parent_all_ or child_all_; touched
    // only by the thread that forks.
    std::optional<std::unique_lock<std::mutex>> held_;
};

ForkRegistration::ForkRegistration(ForkHandlers handlers) : handlers_(std::move(handlers)) {
    ForkList::instance().add(*this);
}

ForkRegistration::~ForkRegistration() {
    ForkList::instance().remove(*this);
}

}  // namespace lumen::detail
