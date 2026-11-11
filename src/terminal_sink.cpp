#include "lumen/terminal_sink.hpp"

#include <optional>
#include <span>
#include <string>
#include <utility>

#include "line_writer.hpp"
#include "lumen/error.hpp"
#include "record_format.hpp"
#include "terminal_output.hpp"
#include "time_format.hpp"

namespace lumen {

struct TerminalSink::Impl {
    Impl(detail::OutputStream output_stream, bool use_color, detail::TextStyle text_style,
         std::size_t max_queued_lines, DiagnosticHandler diagnostics)
        : output(std::move(output_stream))
        , colored(use_color)
        , style(std::move(text_style))
        , writer(
              "TerminalSink", max_queued_lines,
              [this](std::span<const std::string> lines) {
                  return detail::write_and_flush(output.stream(), lines);
              },
              std::move(diagnostics)) {}

    const detail::OutputStream output;
    const bool colored;
    const detail::TextStyle style;
    detail::LineWriter writer;  // last: its thread uses the members above
};

std::expected<std::unique_ptr<TerminalSink>, std::error_code> TerminalSink::create(Config config) {
    if (config.max_queued_lines == 0) {
        return std::unexpected(make_error_code(LumenError::InvalidSinkConfig));
    }
    std::expected<detail::TimeFormat, std::error_code> time =
        detail::TimeFormat::parse(config.time_format, config.time_zone);
    if (!time) {
        return std::unexpected(time.error());
    }
    detail::OutputStream output(std::move(config.output));
    const bool colored = detail::resolve_color(config.color, output);
    detail::TextStyle style{
        .time = std::move(*time),
        .colors = colored ? std::optional<LevelColors>(config.colors) : std::nullopt,
        .shown_tags = std::move(config.inline_tags),
        .show_thread = false,
        .show_source = config.show_source,
    };
    auto impl = std::make_unique<Impl>(std::move(output), colored, std::move(style),
                                       config.max_queued_lines, std::move(config.diagnostics));
    return std::make_unique<TerminalSink>(ConstructionKey{}, std::move(impl));
}

TerminalSink::TerminalSink(ConstructionKey /*key*/, std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TerminalSink::~TerminalSink() = default;

void TerminalSink::on_log(const LogRecord& record) {
    impl_->writer.enqueue(detail::text_line(record, impl_->style));
}

void TerminalSink::on_metric(const MetricRecord& record) {
    impl_->writer.enqueue(detail::text_line(record, impl_->style));
}

void TerminalSink::on_progress(const ProgressRecord& record) {
    impl_->writer.enqueue(detail::text_line(record, impl_->style));
}

void TerminalSink::flush() {
    impl_->writer.wait_until_written();
}

bool TerminalSink::colored() const noexcept {
    return impl_->colored;
}

WriterStatistics TerminalSink::statistics() const noexcept {
    return impl_->writer.statistics();
}

}  // namespace lumen
