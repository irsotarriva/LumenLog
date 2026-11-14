// Before the OpenMP fragments of docs/bridges.md: the work the example
// spreads over threads. Compiled, never linked.
#ifndef LUMEN_TESTS_DOCS_PRELUDES_BRIDGE_OPENMP_HPP
#define LUMEN_TESTS_DOCS_PRELUDES_BRIDGE_OPENMP_HPP

#include <cstdint>

#include "lumen/lumen.hpp"

inline constexpr int64_t n_events = 1000;
void process(int64_t event);

#endif
