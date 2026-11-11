// lumen/terminal_sink.hpp alone declares what it documents.
#include "lumen/terminal_sink.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_base_of_v<Sink, TerminalSink>);
static_assert(std::is_same_v<decltype(TerminalSink::create(TerminalSink::Config{})),
                             std::expected<std::unique_ptr<TerminalSink>, std::error_code>>);
static_assert(std::is_same_v<decltype(TerminalSink::Config{}.color), ColorMode>);
static_assert(std::is_same_v<decltype(TerminalSink::Config{}.colors), LevelColors>);
static_assert(std::is_same_v<decltype(TerminalSink::Config{}.time_format), std::string>);
static_assert(std::is_same_v<decltype(TerminalSink::Config{}.time_zone), TimeZone>);
static_assert(std::is_same_v<decltype(TerminalSink::Config{}.show_source), bool>);
static_assert(std::is_same_v<decltype(TerminalSink::Config{}.diagnostics), DiagnosticHandler>);
static_assert(
    std::is_same_v<decltype(&TerminalSink::colored), bool (TerminalSink::*)() const noexcept>);
static_assert(std::is_same_v<decltype(&TerminalSink::statistics),
                             WriterStatistics (TerminalSink::*)() const noexcept>);

}  // namespace lumen
