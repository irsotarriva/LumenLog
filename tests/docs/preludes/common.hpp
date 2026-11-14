// Included before the documentation fragments marked prelude=common: the
// library, and declarations of the user code the fragments call. The
// fragments are compiled, never linked, so the functions need no definition.
#ifndef LUMEN_TESTS_DOCS_PRELUDES_COMMON_HPP
#define LUMEN_TESTS_DOCS_PRELUDES_COMMON_HPP

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lumen/lumen.hpp"

[[nodiscard]] double read_energy();
void process(uint64_t event);
[[nodiscard]] std::vector<std::string> list_files();
void copy_file(const std::string& name);

#endif
