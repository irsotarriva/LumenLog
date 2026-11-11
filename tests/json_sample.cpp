// Writes records with hard contents through a Core into a FileSink in Json
// format, at the path given as the only argument, and prints how many records
// it wrote. check_json_lines.py runs it and parses every line with Python's
// json module. Each record carries, in tags named expect_*, what the parsed
// line must contain, so the checker needs no list of its own.
#include <bit>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lumen/core.hpp"
#include "lumen/file_sink.hpp"
#include "lumen/progress.hpp"
#include "lumen/record_builder.hpp"

namespace {

[[nodiscard]] std::string hex_bytes(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out += std::format("{:02x}", static_cast<unsigned char>(c));
    }
    return out;
}

void log_exactly(lumen::Core& core, std::string_view message) {
    lumen::RecordBuilder(core, lumen::LogLevel::Warn, std::source_location::current(), message)
        .tag("expect_msg_utf8_hex", hex_bytes(message));
}

// A message whose invalid bytes are replaced: what the parsed text must be.
void log_replaced(lumen::Core& core, std::string_view message, std::string_view expected) {
    lumen::RecordBuilder(core, lumen::LogLevel::Error, std::source_location::current(), message)
        .tag("expect_msg_utf8_hex", hex_bytes(expected));
}

void metric_exactly(lumen::Core& core, double value) {
    lumen::MetricBuilder(core, "value", value)
        .tag("expect_bits", std::format("{:016x}", std::bit_cast<uint64_t>(value)));
}

void metric_non_finite(lumen::Core& core, double value, std::string_view text) {
    lumen::MetricBuilder(core, "value", value).tag("expect_value_text", text);
}

[[nodiscard]] uint32_t write_records(lumen::Core& core) {
    uint32_t count = 0;
    using std::string_view_literals::operator""sv;
    for (const std::string_view message :
         {"plain"sv, ""sv, R"(quote " backslash \)"sv, "\b\f\n\r\t\x01\x1f\x7f end"sv,
          "nul \0 inside"sv, "\xc3\xa9t\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"sv,
          "\x1b[31mred\x1b[0m"sv}) {
        log_exactly(core, message);
        ++count;
    }
    log_replaced(core,
                 "a\xff"
                 "b",
                 "a\xef\xbf\xbd"
                 "b");
    log_replaced(core, "\xc0\xaf", "\xef\xbf\xbd\xef\xbf\xbd");
    log_replaced(core, "x\xe2\x82", "x\xef\xbf\xbd\xef\xbf\xbd");
    count += 3;
    using Limits = std::numeric_limits<double>;
    for (const double value : {0.0, -0.0, 1e-9, 123456789.123456789, 0.1, Limits::denorm_min(),
                               Limits::min(), Limits::max(), -Limits::max(), 1.0 / 3.0}) {
        metric_exactly(core, value);
        ++count;
    }
    metric_non_finite(core, Limits::quiet_NaN(), "NaN");
    metric_non_finite(core, Limits::infinity(), "Infinity");
    metric_non_finite(core, -Limits::infinity(), "-Infinity");
    count += 3;
    lumen::RecordBuilder(core, lumen::LogLevel::Info, std::source_location::current(), "reserved")
        .tag("type", "t")
        .tag("ts", "1")
        .tag("msg", "m")
        .tag("level", "l")
        .tag("file", "f")
        .tag("line", "2")
        .tag("tags", "x")
        .tag("", "")
        .tag("number", int64_t{3})
        .tag("expect_tag_count", int64_t{10});
    ++count;
    lumen::ProgressHandle bar(core, "label \"quoted\"", 2);
    bar.update(1);
    bar.finish();
    count += 2;
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> raw_args(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> args(raw_args.begin(), raw_args.end());
    if (args.size() != 2) {
        std::cerr << "usage: lumen_json_sample <output file>\n";
        return 2;
    }
    lumen::Core core;
    auto sink = lumen::FileSink::create(
        {.path = std::filesystem::path(args.at(1)), .format = lumen::FileFormat::Json});
    if (!sink) {
        std::cerr << "cannot create the sink: " << sink.error().message() << '\n';
        return 1;
    }
    if (!core.add_sink(std::move(*sink)).has_value()) {
        std::cerr << "cannot register the sink\n";
        return 1;
    }
    const uint32_t count = write_records(core);
    core.flush();
    std::cout << count << '\n';
    return 0;
}
