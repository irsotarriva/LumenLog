#ifndef LUMEN_SRC_ROTATING_FILE_HPP
#define LUMEN_SRC_ROTATING_FILE_HPP

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>

#include "line_writer.hpp"
#include "lumen/core.hpp"

// The file behind a FileSink: appends lines and rotates by size, as
// documented with FileSink (lumen/file_sink.hpp). Used by the writer thread
// only.

namespace lumen::detail {

class RotatingFile {
public:
    struct Limits {
        uint64_t max_file_bytes = 0;  // 0: never rotate
        uint32_t max_files = 1;       // at least 1
    };

    // Opens `path` for appending (creating it if needed). A named pipe is
    // opened only if it has a reader, so that this never waits for one. Fails
    // with LumenError::FileOpenFailed, after passing one line with the cause
    // ("lumen: FileSink: cannot open <path>: <reason>") to `diagnostics`.
    [[nodiscard]] static std::expected<RotatingFile, std::error_code> open(
        std::filesystem::path path, Limits limits, const DiagnosticHandler& diagnostics);

    // Writes the lines in order, rotating before a line that would pass the
    // size limit, and flushes the stream. After a rotation that failed to
    // move the active file away, the next attempt waits until the file has
    // grown by another max_file_bytes.
    [[nodiscard]] BatchOutcome write(std::span<const std::string> lines);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    RotatingFile(std::filesystem::path path, Limits limits, std::ofstream file, uint64_t bytes);

    [[nodiscard]] bool needs_rotation_(uint64_t line_bytes) const noexcept;
    void rotate_(BatchOutcome& outcome);
    // Each returns false if the active file is still at path_.
    [[nodiscard]] bool remove_active_(BatchOutcome& outcome) const;
    [[nodiscard]] bool shift_active_(BatchOutcome& outcome) const;
    void shift_old_files_(BatchOutcome& outcome) const;
    [[nodiscard]] bool reopen_();
    void flush_(BatchOutcome& outcome);
    [[nodiscard]] std::filesystem::path numbered_(uint32_t index) const;

    std::filesystem::path path_;
    Limits limits_;
    std::ofstream file_;
    uint64_t bytes_ = 0;      // size of the active file, counting lines not yet flushed
    uint64_t unflushed_ = 0;  // lines written to file_ since the last successful flush
    // The size the active file must pass before the next rotation: the limit,
    // or after a failed rotation the size then plus another max_file_bytes.
    uint64_t rotate_after_bytes_ = 0;
};

}  // namespace lumen::detail

#endif
