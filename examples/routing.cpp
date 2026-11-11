// Routing: several sinks, each with a predicate that decides which records it
// receives; files in the three formats; errors from sink creation and
// registration; the Core's and the sinks' counters.
//
//   routing [directory]   writes routing.log, routing.logfmt and
//                         routing.jsonl to `directory` (default: the
//                         system's temporary directory)
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "lumen/lumen.hpp"

namespace {

// Registers `sink` and returns a non-owning pointer to it, for reading its
// statistics later. The Core owns the sink until it shuts down.
template <typename S>
    requires std::derived_from<S, lumen::Sink>
[[nodiscard]] std::expected<const S*, std::error_code> add(
    std::expected<std::unique_ptr<S>, std::error_code> created, const lumen::Predicate& predicate) {
    if (!created) {
        return std::unexpected(created.error());
    }
    const S* const sink = created->get();  // non-owning
    if (auto id = lumen::core().add_sink(std::move(*created), predicate); !id) {
        return std::unexpected(id.error());
    }
    return sink;
}

// A FileSink that starts from an empty file, so that every run writes the
// same lines.
[[nodiscard]] std::expected<std::unique_ptr<lumen::FileSink>, std::error_code> fresh_file(
    const std::filesystem::path& path, lumen::FileFormat format) {
    std::error_code removed;
    // false without an error when there was no file, which is fine
    if (!std::filesystem::remove(path, removed) && removed) {
        return std::unexpected(removed);
    }
    lumen::FileSink::Config config;
    config.path = path;
    config.format = format;
    return lumen::FileSink::create(std::move(config));
}

void show_expected_errors() {
    // A directory that does not exist: FileSink::create fails, nothing is opened.
    lumen::FileSink::Config missing_directory;
    missing_directory.path = "/nonexistent-directory/run.log";
    if (const auto sink = lumen::FileSink::create(std::move(missing_directory)); !sink) {
        std::cout << "FileSink::create: " << sink.error().message() << '\n';
    }

    // A query with an unknown level: the error names the offset of the problem.
    if (const auto query = lumen::parse_predicate("level >= LOUD"); !query) {
        std::cout << "parse_predicate: " << query.error().message << " at offset "
                  << query.error().position << '\n';
    }

    // No sink has this id.
    if (const auto removed = lumen::core().remove_sink(lumen::SinkId{999}); !removed) {
        if (removed.error() == lumen::LumenError::InvalidSinkId) {
            std::cout << "remove_sink: " << removed.error().message() << '\n';
        }
    }
}

void show_statistics(std::string_view name, const lumen::WriterStatistics& s) {
    std::cout << name << ": " << s.lines_written << " lines written, " << s.lines_dropped
              << " dropped, " << s.lines_failed << " failed\n";
}

// The sinks of this program, for reading their counters at the end.
struct Sinks {
    const lumen::TerminalSink* terminal;  // non-owning
    const lumen::FileSink* text;          // non-owning
    const lumen::FileSink* logfmt;        // non-owning
    const lumen::FileSink* json;          // non-owning
};

[[nodiscard]] std::expected<Sinks, std::error_code> set_up_sinks(
    const std::filesystem::path& directory) {
    // Warnings and errors on the terminal. Level conditions do not apply to
    // metrics and progress records, so this sink also receives every metric.
    lumen::TerminalSink::Config terminal_config;
    terminal_config.time_format = "";
    terminal_config.inline_tags = {"detector"};
    const auto terminal = add(lumen::TerminalSink::create(std::move(terminal_config)),
                              lumen::level_at_least(lumen::LogLevel::Warn));
    if (!terminal) {
        return std::unexpected(terminal.error());
    }

    // Everything from the tracker, as text for people and as logfmt.
    const lumen::Predicate tracker = lumen::tag_equals("detector", "tracker");
    const auto text = add(fresh_file(directory / "routing.log", lumen::FileFormat::Text), tracker);
    if (!text) {
        return std::unexpected(text.error());
    }
    const auto logfmt =
        add(fresh_file(directory / "routing.logfmt", lumen::FileFormat::KeyValue), tracker);
    if (!logfmt) {
        return std::unexpected(logfmt.error());
    }

    // Hot channels and errors as JSON, with the filter given as text, as it
    // would come from a configuration file or a command-line flag.
    const auto query = lumen::parse_predicate("temperature > 300 || level >= ERROR");
    if (!query) {
        return std::unexpected(query.error().code);
    }
    const auto json = add(fresh_file(directory / "routing.jsonl", lumen::FileFormat::Json), *query);
    if (!json) {
        return std::unexpected(json.error());
    }
    return Sinks{*terminal, *text, *logfmt, *json};
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    const std::filesystem::path directory =
        args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::temp_directory_path();
    const auto sinks = set_up_sinks(directory);
    if (!sinks) {
        std::cerr << "cannot set up the sinks: " << sinks.error().message() << '\n';
        return 1;
    }

    LUMEN_LOG_INFO("tracker aligned").tag("detector", "tracker");
    LUMEN_LOG_WARN("channel noisy").tag("detector", "tracker").tag("channel", 17);
    LUMEN_LOG_INFO("channel hot").tag("detector", "calorimeter").tag("temperature", 312.5);
    LUMEN_LOG_ERROR("readout lost").tag("detector", "calorimeter");
    lumen::metric("hits", 1024).tag("detector", "tracker");

    show_expected_errors();

    lumen::core().flush();

    const lumen::CoreStatistics stats = lumen::core().statistics();
    std::cout << "logs accepted: " << stats.logs.accepted
              << ", dropped (queue full): " << stats.logs.dropped_queue_full << '\n'
              << "metrics accepted: " << stats.metrics.accepted
              << ", dropped (queue full): " << stats.metrics.dropped_queue_full << '\n'
              << "tags dropped: " << stats.tags_dropped << '\n';
    show_statistics("terminal", sinks->terminal->statistics());
    show_statistics("routing.log", sinks->text->statistics());
    show_statistics("routing.logfmt", sinks->logfmt->statistics());
    show_statistics("routing.jsonl", sinks->json->statistics());
    return 0;
}
