#include "lumen/terminal_sink.hpp"

#include <concepts>
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
    Impl(detail::OutputStream output_stream, bool use_color, detail::TextStyle text_style)
        : output(std::move(output_stream)), colored(use_color), style(std::move(text_style)) {}

    // Queues the line of `record`: one that a builder makes, or a progress record.
    template <typename R>
        requires BuiltRecord<R> || std::same_as<R, ProgressRecord>
    void enqueue(const R& record) {
        writer->enqueue(detail::text_line(record, style));
    }

    const detail::OutputStream output;
    const bool colored;
    const detail::TextStyle style;
    std::unique_ptr<detail::LineWriter> writer;  // last: its thread uses the members above
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
    auto impl = std::make_unique<Impl>(std::move(output), colored, std::move(style));
    const Impl* const destination = impl.get();  // non-owning: the writer dies before impl
    std::expected<std::unique_ptr<detail::LineWriter>, std::error_code> writer =
        detail::LineWriter::create(
            "TerminalSink", config.max_queued_lines,
            [destination](std::span<const std::string> lines) {
                return detail::write_and_flush(destination->output.stream(), lines);
            },
            std::move(config.diagnostics));
    if (!writer) {
        return std::unexpected(writer.error());
    }
    impl->writer = std::move(*writer);
    return std::make_unique<TerminalSink>(ConstructionKey{}, std::move(impl));
}

TerminalSink::TerminalSink(ConstructionKey /*key*/, std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TerminalSink::~TerminalSink() = default;

void TerminalSink::on_log(const LogRecord& record) {
    if (record.level == LogLevel::Fatal) {
        impl_->writer->expect_termination();
    }
    impl_->enqueue(record);
}

void TerminalSink::on_metric(const MetricRecord& record) {
    impl_->enqueue(record);
}

void TerminalSink::on_progress(const ProgressRecord& record) {
    impl_->enqueue(record);
}

void TerminalSink::flush() {
    impl_->writer->wait_until_written();
}

bool TerminalSink::colored() const noexcept {
    return impl_->colored;
}

WriterStatistics TerminalSink::statistics() const noexcept {
    return impl_->writer->statistics();
}

}  // namespace lumen
