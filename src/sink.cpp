#include "lumen/sink.h"

namespace lumen {

// ── TerminalSink ─────────────────────────────────────────────────────────────

TerminalSink::Config TerminalSink::default_config() { return {}; }
TerminalSink::TerminalSink(Config cfg) : __cfg(std::move(cfg)) {}
void TerminalSink::on_log(const LogRecord& /*record*/) {}
void TerminalSink::on_metric(const MetricRecord& /*record*/) {}
void TerminalSink::on_progress(const ProgressRecord& /*record*/) {}
void TerminalSink::flush() {}

// ── FileSink ─────────────────────────────────────────────────────────────────

struct FileSink::Impl {};

FileSink::FileSink(Config cfg) : __cfg(std::move(cfg)), __impl(std::make_unique<Impl>()) {}
FileSink::~FileSink() = default;
void FileSink::on_log(const LogRecord& /*record*/) {}
void FileSink::on_metric(const MetricRecord& /*record*/) {}
void FileSink::on_progress(const ProgressRecord& /*record*/) {}
void FileSink::flush() {}

// ── JsonSink ─────────────────────────────────────────────────────────────────

struct JsonSink::Impl {};

JsonSink::JsonSink(std::string_view /*path*/) : __impl(std::make_unique<Impl>()) {}
JsonSink::~JsonSink() = default;
void JsonSink::on_log(const LogRecord& /*record*/) {}
void JsonSink::on_metric(const MetricRecord& /*record*/) {}
void JsonSink::on_progress(const ProgressRecord& /*record*/) {}
void JsonSink::flush() {}

}  // namespace lumen
