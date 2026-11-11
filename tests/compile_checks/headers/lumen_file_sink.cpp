// lumen/file_sink.hpp alone declares what it documents.
#include "lumen/file_sink.hpp"

#include <array>
#include <type_traits>

namespace lumen {

static_assert(std::is_base_of_v<Sink, FileSink>);
static_assert(std::is_same_v<decltype(FileSink::create(FileSink::Config{})),
                             std::expected<std::unique_ptr<FileSink>, std::error_code>>);
inline constexpr std::array every_format{FileFormat::Text, FileFormat::KeyValue, FileFormat::Json};
static_assert(std::is_same_v<decltype(FileSink::Config{}.path), std::filesystem::path>);
static_assert(std::is_same_v<decltype(FileSink::Config{}.format), FileFormat>);
static_assert(std::is_same_v<decltype(FileSink::Config{}.max_file_bytes), uint64_t>);
static_assert(std::is_same_v<decltype(FileSink::Config{}.max_files), uint32_t>);
static_assert(std::is_same_v<decltype(FileSink::Config{}.diagnostics), DiagnosticHandler>);
static_assert(std::is_same_v<decltype(&FileSink::statistics),
                             WriterStatistics (FileSink::*)() const noexcept>);

}  // namespace lumen
