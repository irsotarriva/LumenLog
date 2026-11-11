// Run by check_blocked_stderr.py with standard error on a pipe that nobody
// reads, so that every write to it blocks once the pipe is full. A
// TerminalSink writes to standard error with its default diagnostics (which
// also write to standard error); a second sink counts what it receives.
//
//   blocked_stderr deliver   logs 2000 records and prints how many the
//                            counting sink received, then ends itself with
//                            SIGKILL (see end_now)
//   blocked_stderr fatal     fills the pipe, then LUMEN_LOG_FATAL: the
//                            process must terminate rather than hang
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lumen/lumen.hpp"

namespace {

constexpr uint64_t records = 2000;

// What the counting sink received; the main thread waits on it.
struct Received {
    std::mutex mutex;
    std::condition_variable changed;  // waits on mutex
    uint64_t count = 0;               // guarded by mutex
};

class CountingSink final : public lumen::Sink {
public:
    explicit CountingSink(std::shared_ptr<Received> received) : received_(std::move(received)) {}
    void on_log(const lumen::LogRecord& /*record*/) override {
        {
            const std::scoped_lock lock(received_->mutex);
            ++received_->count;
        }
        received_->changed.notify_all();
    }
    void flush() override {}

private:
    std::shared_ptr<Received> received_;
};

// Ends the process at once. exit() would shut the global core down, which
// waits for the blocked stream; even _exit() and abort() can block, because
// the sanitizer runtimes flush the C streams on the way out, and standard
// error's lock is held by the blocked writer.
[[noreturn]] void end_now() {
    static_cast<void>(kill(getpid(), SIGKILL));
    std::abort();  // unreachable: SIGKILL cannot be caught
}

// Large records: the pipe is full after a few of them.
const std::string large(4000, 'x');

[[nodiscard]] int deliver(Received& received) {
    for (uint64_t i = 0; i < records; ++i) {
        LUMEN_LOG_INFO("{}", large);
    }
    // Not core().flush(): it would wait for the blocked TerminalSink, as
    // flush() may. The dispatch thread delivers on its own.
    std::unique_lock lock(received.mutex);
    static_cast<void>(received.changed.wait_for(lock, std::chrono::seconds(20),
                                                [&] { return received.count >= records; }));
    std::cout << "received " << received.count << " of " << records << '\n' << std::flush;
    end_now();
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> raw_args(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> args(raw_args.begin(), raw_args.end());
    if (args.size() != 2 || (args.at(1) != "deliver" && args.at(1) != "fatal")) {
        std::cout << "usage: blocked_stderr deliver|fatal\n";
        return 2;
    }
    lumen::TerminalSink::Config config;
    config.max_queued_lines = 10;
    auto terminal = lumen::TerminalSink::create(std::move(config));
    if (!terminal || !lumen::core().add_sink(std::move(*terminal))) {
        std::cout << "cannot add the terminal sink\n";
        return 2;
    }
    const auto received = std::make_shared<Received>();
    if (!lumen::core().add_sink(std::make_unique<CountingSink>(received))) {
        std::cout << "cannot add the counting sink\n";
        return 2;
    }
    if (args.at(1) == "deliver") {
        return deliver(*received);
    }
    // The C++ library's default terminate handler prints a message to standard
    // error, which blocks here like everything else on it; that is the
    // handler's business, not the sinks'. This one ends the process at once,
    // so the test sees whether the sinks let the fatal path reach
    // std::terminate.
    std::set_terminate([] { end_now(); });
    for (uint32_t i = 0; i < 100; ++i) {
        LUMEN_LOG_INFO("{}", large);
    }
    std::cout << "fatal" << '\n' << std::flush;
    LUMEN_LOG_FATAL("the end");
}
