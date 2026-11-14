#include "rotating_file.hpp"

#include <cerrno>
#include <cstddef>
#include <format>
#include <ios>
#include <limits>
#include <string_view>
#include <utility>

#ifndef PLATFORM_WINDOWS
#include <fcntl.h>
#include <unistd.h>
#endif

#include "lumen/error.hpp"

namespace lumen::detail {

namespace {

// Binary: a line's bytes on disk are exactly the bytes counted (no "\r\n"
// translation on Windows).
constexpr std::ios::openmode append_mode = std::ios::out | std::ios::app | std::ios::binary;

// A batch's failure text stops growing at about this size; the first
// failures are the informative ones.
constexpr std::size_t max_failure_text = 512;

// The size of an existing file, which the sink appends to; 0 when it has none
// (a new file, or a device such as /dev/null).
[[nodiscard]] uint64_t existing_size(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

void note_failure(BatchOutcome& outcome, std::string_view what, const std::error_code& ec) {
    if (outcome.failure.size() >= max_failure_text) {
        return;
    }
    if (!outcome.failure.empty()) {
        outcome.failure += "; ";
    }
    outcome.failure += std::format("{}: {}", what, ec.message());
}

[[nodiscard]] uint64_t saturating_add(uint64_t a, uint64_t b) noexcept {
    return a > std::numeric_limits<uint64_t>::max() - b ? std::numeric_limits<uint64_t>::max()
                                                        : a + b;
}

void report_open_failure(const DiagnosticHandler& diagnostics, const std::filesystem::path& path,
                         std::string_view reason) noexcept {
    if (!diagnostics) {
        return;  // an empty handler means: discard diagnostics
    }
    try {
        diagnostics(std::format("lumen: FileSink: cannot open {}: {}", path.string(), reason));
    } catch (...) {
        // The caller still gets LumenError::FileOpenFailed; only the detail
        // is lost.
        return;
    }
}

#ifndef PLATFORM_WINDOWS
// Holds a non-blocking write end of a named pipe open while the stream opens
// the pipe, so that the stream's blocking open finds the reader present, and
// the reader does not see an end of file between the two opens.
class PipeProbe {
public:
    // Fails with the reason when `path` is a named pipe without a reader.
    [[nodiscard]] static std::expected<PipeProbe, std::string> open(
        const std::filesystem::path& path) {
        std::error_code ec;
        if (std::filesystem::status(path, ec).type() != std::filesystem::file_type::fifo) {
            return PipeProbe(-1);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic
        const int fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            return PipeProbe(fd);
        }
        const int cause = errno;
        if (cause == ENXIO) {
            return std::unexpected("a named pipe without a reader (opening it would wait for one)");
        }
        return std::unexpected(std::generic_category().message(cause));
    }
    ~PipeProbe() {
        if (fd_ >= 0) {
            // Only the stream's own descriptor is written to; a failing close
            // of this one loses nothing.
            static_cast<void>(::close(fd_));
        }
    }
    PipeProbe(PipeProbe&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    PipeProbe(const PipeProbe&) = delete;
    PipeProbe& operator=(const PipeProbe&) = delete;
    PipeProbe& operator=(PipeProbe&&) = delete;

private:
    explicit PipeProbe(int fd) noexcept : fd_(fd) {}
    int fd_;  // -1: not a pipe, nothing held
};
#endif

// Why the stream could not open a file, as far as the platform says.
[[nodiscard]] std::string open_failure_reason(int cause) {
    return cause == 0 ? std::string("the platform gave no reason")
                      : std::generic_category().message(cause);
}

}  // namespace

std::expected<RotatingFile, std::error_code> RotatingFile::open(
    std::filesystem::path path, Limits limits, const DiagnosticHandler& diagnostics) {
#ifndef PLATFORM_WINDOWS
    const std::expected<PipeProbe, std::string> probe = PipeProbe::open(path);
    if (!probe) {
        report_open_failure(diagnostics, path, probe.error());
        return std::unexpected(make_error_code(LumenError::FileOpenFailed));
    }
#endif
    errno = 0;
    std::ofstream file(path, append_mode);
    if (!file.is_open()) {
        // The standard streams do not report why; errno is what the
        // underlying open(2) or _wfopen left.
        report_open_failure(diagnostics, path, open_failure_reason(errno));
        return std::unexpected(make_error_code(LumenError::FileOpenFailed));
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        limits.max_file_bytes = 0;  // a device or pipe cannot be renamed away
    }
    const uint64_t bytes = existing_size(path);
    return RotatingFile(std::move(path), limits, std::move(file), bytes);
}

RotatingFile::RotatingFile(std::filesystem::path path, Limits limits, std::ofstream file,
                           uint64_t bytes)
    : path_(std::move(path))
    , limits_(limits)
    , file_(std::move(file))
    , bytes_(bytes)
    , rotate_after_bytes_(limits.max_file_bytes) {}

BatchOutcome RotatingFile::write(std::span<const std::string> lines) {
    BatchOutcome outcome;
    for (const std::string& line : lines) {
        if (needs_rotation_(line.size())) {
            rotate_(outcome);
        }
        if (!file_.is_open() && !reopen_()) {
            ++outcome.failed_lines;
            note_failure(outcome, "cannot reopen " + path_.string(),
                         make_error_code(LumenError::FileOpenFailed));
            continue;
        }
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        bytes_ += line.size();
        ++unflushed_;
    }
    flush_(outcome);
    return outcome;
}

bool RotatingFile::needs_rotation_(uint64_t line_bytes) const noexcept {
    return limits_.max_file_bytes > 0 && bytes_ > 0 &&
           saturating_add(bytes_, line_bytes) > rotate_after_bytes_;
}

void RotatingFile::rotate_(BatchOutcome& outcome) {
    flush_(outcome);
    file_.close();
    const bool moved = limits_.max_files == 1 ? remove_active_(outcome) : shift_active_(outcome);
    if (!reopen_()) {
        note_failure(outcome, "cannot reopen " + path_.string(),
                     make_error_code(LumenError::FileOpenFailed));
    }
    // A rotation that failed would fail again on the next line; trying once
    // per max_file_bytes of growth bounds the cost.
    rotate_after_bytes_ =
        moved ? limits_.max_file_bytes : saturating_add(bytes_, limits_.max_file_bytes);
}

bool RotatingFile::remove_active_(BatchOutcome& outcome) const {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    if (ec) {
        note_failure(outcome, "cannot remove " + path_.string(), ec);
        return false;
    }
    return true;
}

bool RotatingFile::shift_active_(BatchOutcome& outcome) const {
    shift_old_files_(outcome);
    std::error_code ec;
    std::filesystem::rename(path_, numbered_(1), ec);
    if (ec) {
        note_failure(outcome, "cannot rename " + path_.string(), ec);
        return false;
    }
    return true;
}

// <path>.<max_files - 1> is deleted, then <path>.k becomes <path>.<k + 1>,
// oldest first, so that no rename has to replace an existing file.
void RotatingFile::shift_old_files_(BatchOutcome& outcome) const {
    std::error_code ec;
    std::filesystem::remove(numbered_(limits_.max_files - 1), ec);
    if (ec) {
        note_failure(outcome, "cannot remove " + numbered_(limits_.max_files - 1).string(), ec);
    }
    for (uint32_t index = limits_.max_files - 1; index > 1; --index) {
        const std::filesystem::path from = numbered_(index - 1);
        if (!std::filesystem::exists(from, ec)) {
            continue;
        }
        std::filesystem::rename(from, numbered_(index), ec);
        if (ec) {
            note_failure(outcome, "cannot rename " + from.string(), ec);
        }
    }
}

bool RotatingFile::reopen_() {
    file_.clear();
    file_.open(path_, append_mode);
    bytes_ = file_.is_open() ? existing_size(path_) : 0;
    return file_.is_open();
}

void RotatingFile::flush_(BatchOutcome& outcome) {
    if (!file_.is_open()) {
        return;
    }
    file_.flush();
    if (!file_.good()) {
        outcome.failed_lines += unflushed_;
        note_failure(outcome, "cannot write " + path_.string(),
                     make_error_code(LumenError::IoError));
        file_.clear();
    }
    unflushed_ = 0;
}

std::filesystem::path RotatingFile::numbered_(uint32_t index) const {
    std::filesystem::path numbered = path_;
    numbered += "." + std::to_string(index);
    return numbered;
}

}  // namespace lumen::detail
