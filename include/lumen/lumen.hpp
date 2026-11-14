#ifndef LUMEN_LUMEN_HPP
#define LUMEN_LUMEN_HPP

// Everything a program that logs needs. Start with lumen/macros.hpp (the
// logging statements) and lumen/core.hpp (the model: threads, guarantees,
// lifetime); lumen/context.hpp describes the ambient tags and
// lumen/limits.hpp every fixed capacity.

#include "lumen/context.hpp"
#include "lumen/core.hpp"
#include "lumen/error.hpp"
#include "lumen/file_sink.hpp"
#include "lumen/level.hpp"
#include "lumen/limits.hpp"
#include "lumen/macros.hpp"
#include "lumen/predicate.hpp"
#include "lumen/progress.hpp"
#include "lumen/record.hpp"
#include "lumen/record_builder.hpp"
#include "lumen/sink.hpp"
#include "lumen/tag_set.hpp"
#include "lumen/tagged.hpp"
#include "lumen/terminal_sink.hpp"
#include "lumen/thread_clock.hpp"

#endif
