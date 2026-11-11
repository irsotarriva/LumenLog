#include "rotating_file.hpp"

#include <format>
#include <ios>
#include <string_view>
#include <utility>

#include "lumen/error.hpp"

namespace lumen::detail {

namespace {

// Binary: a line's bytes on disk are exactly the bytes counted (no "\r\n"
// translation on Windows).
constexpr std::ios::openmode append_mode = std::ios::out | std::ios::app | std::ios::binary;

// The size of an existing file, which the sink appends to; 0 when it has none
// (a new file, or a device such as /dev/null).
[[nodiscard]] uint64_t existing_size(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

void note_failure(BatchOutcome& outcome, std::string_view what, const std::error_code& ec) {
    if (outcome.failure.empty()) {
        outcome.failure = std::format("{}: {}", what, ec.message());
    }
}

}  // namespace

std::expected<RotatingFile, std::error_code> RotatingFile::open(std::filesystem::path path,
                                                                Limits limits) {
    std::ofstream file(path, append_mode);
    if (!file.is_open()) {
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
    : path_(std::move(path)), limits_(limits), file_(std::move(file)), bytes_(bytes) {}

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
    return limits_.max_file_bytes > 0 && bytes_ > 0 && bytes_ + line_bytes > limits_.max_file_bytes;
}

void RotatingFile::rotate_(BatchOutcome& outcome) {
    flush_(outcome);
    file_.close();
    std::error_code ec;
    if (limits_.max_files == 1) {
        std::filesystem::remove(path_, ec);
        if (ec) {
            note_failure(outcome, "cannot remove " + path_.string(), ec);
        }
    } else {
        shift_old_files_(outcome);
        std::filesystem::rename(path_, numbered_(1), ec);
        if (ec) {
            note_failure(outcome, "cannot rename " + path_.string(), ec);
        }
    }
    if (!reopen_()) {
        note_failure(outcome, "cannot reopen " + path_.string(),
                     make_error_code(LumenError::FileOpenFailed));
    }
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
