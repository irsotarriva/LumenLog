#include "diagnostic_reporter.hpp"

#include <exception>
#include <iostream>
#include <utility>

namespace lumen {

DiagnosticHandler stderr_diagnostics() {
    return [](std::string_view line) { std::cerr << line << '\n' << std::flush; };
}

}  // namespace lumen

namespace lumen::detail {

namespace {

[[nodiscard]] std::shared_ptr<const DiagnosticHandler> share(DiagnosticHandler handler) {
    if (!handler) {
        return nullptr;
    }
    return std::make_shared<const DiagnosticHandler>(std::move(handler));
}

void write_after_handler_failure(std::string_view line, std::string_view what) noexcept {
    std::cerr << line << "\nlumen: the diagnostic handler threw (" << what
              << "); the line above is written to std::cerr instead\n"
              << std::flush;
}

// A handler that throws loses neither the line nor the process: both the
// line and what was thrown go to std::cerr instead.
void call_handler(const DiagnosticHandler& handler, std::string_view line) noexcept {
    try {
        handler(line);
    } catch (const std::exception& e) {
        write_after_handler_failure(line, e.what());
    } catch (...) {
        write_after_handler_failure(line, "an exception not derived from std::exception");
    }
}

}  // namespace

DiagnosticReporter::DiagnosticReporter(DiagnosticHandler handler)
    : handler_(share(std::move(handler))) {}

void DiagnosticReporter::report(std::string_view line) {
    {
        const std::scoped_lock lock(mutex_);
        pending_.emplace_back(line);
        if (draining_) {
            return;
        }
        draining_ = true;
    }
    drain_();
}

void DiagnosticReporter::set_handler(DiagnosticHandler handler) {
    std::shared_ptr<const DiagnosticHandler> previous = share(std::move(handler));
    {
        const std::scoped_lock lock(mutex_);
        handler_.swap(previous);
    }
    // `previous` is released here, without the lock.
}

void DiagnosticReporter::drain_() {
    while (true) {
        std::string line;
        std::shared_ptr<const DiagnosticHandler> handler;
        {
            const std::scoped_lock lock(mutex_);
            if (pending_.empty()) {
                draining_ = false;
                return;
            }
            line = std::move(pending_.front());
            pending_.pop_front();
            handler = handler_;
        }
        if (handler) {
            call_handler(*handler, line);
        }
    }
}

}  // namespace lumen::detail
