#include "lumen/file_sink.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "line_writer.hpp"
#include "lumen/error.hpp"
#include "lumen/thread_clock.hpp"
#include "record_format.hpp"
#include "rotating_file.hpp"
#include "time_format.hpp"

namespace lumen {

namespace {

template <typename R>
concept SinkRecord =
    std::same_as<R, LogRecord> || std::same_as<R, MetricRecord> || std::same_as<R, ProgressRecord>;

// Config::path with every "{date}" replaced by the UTC date of `now_ns`.
[[nodiscard]] std::filesystem::path expand_date(const std::filesystem::path& path,
                                                uint64_t now_ns) {
    const std::u8string placeholder = u8"{date}";
    const std::string date = detail::utc_date(now_ns);
    const std::u8string replacement(date.begin(), date.end());
    std::u8string text = path.u8string();
    for (std::size_t pos = text.find(placeholder); pos != std::u8string::npos;
         pos = text.find(placeholder, pos + replacement.size())) {
        text.replace(pos, placeholder.size(), replacement);
    }
    return {text};
}

[[nodiscard]] bool valid(const FileSink::Config& config) noexcept {
    return config.max_files > 0 && config.max_queued_lines > 0 && !config.path.empty();
}

}  // namespace

struct FileSink::Impl {
    Impl(detail::RotatingFile rotating_file, FileFormat line_format, std::size_t max_queued_lines,
         DiagnosticHandler diagnostics)
        : path(rotating_file.path())
        , file(std::move(rotating_file))
        , format(line_format)
        , style{.time = detail::iso8601_utc_format(),
                .colors = std::nullopt,
                .shown_tags = std::nullopt,
                .show_thread = true,
                .show_source = true}
        , writer(
              "FileSink", max_queued_lines,
              [this](std::span<const std::string> lines) { return file.write(lines); },
              std::move(diagnostics)) {}

    template <SinkRecord R>
    [[nodiscard]] std::string line(const R& record) const {
        switch (format) {
            case FileFormat::Text:     return detail::text_line(record, style);
            case FileFormat::KeyValue: return detail::key_value_line(record);
            case FileFormat::Json:     return detail::json_line(record);
        }
        return detail::text_line(record, style);  // a value outside the enum
    }

    const std::filesystem::path path;
    detail::RotatingFile file;  // used by the writer thread only
    const FileFormat format;
    const detail::TextStyle style;
    detail::LineWriter writer;  // last: its thread uses the members above
};

std::expected<std::unique_ptr<FileSink>, std::error_code> FileSink::create(Config config) {
    if (!valid(config)) {
        return std::unexpected(make_error_code(LumenError::InvalidSinkConfig));
    }
    std::expected<detail::RotatingFile, std::error_code> file = detail::RotatingFile::open(
        expand_date(config.path, now_ns()),
        {.max_file_bytes = config.max_file_bytes, .max_files = config.max_files});
    if (!file) {
        return std::unexpected(file.error());
    }
    auto impl = std::make_unique<Impl>(std::move(*file), config.format, config.max_queued_lines,
                                       std::move(config.diagnostics));
    return std::make_unique<FileSink>(ConstructionKey{}, std::move(impl));
}

FileSink::FileSink(ConstructionKey /*key*/, std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FileSink::~FileSink() = default;

void FileSink::on_log(const LogRecord& record) {
    impl_->writer.enqueue(impl_->line(record));
}

void FileSink::on_metric(const MetricRecord& record) {
    impl_->writer.enqueue(impl_->line(record));
}

void FileSink::on_progress(const ProgressRecord& record) {
    impl_->writer.enqueue(impl_->line(record));
}

void FileSink::flush() {
    impl_->writer.wait_until_written();
}

const std::filesystem::path& FileSink::path() const noexcept {
    return impl_->path;
}

WriterStatistics FileSink::statistics() const noexcept {
    return impl_->writer.statistics();
}

}  // namespace lumen
