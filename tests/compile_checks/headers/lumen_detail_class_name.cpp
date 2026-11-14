// lumen/detail/class_name.hpp alone declares what the builders use.
#include "lumen/detail/class_name.hpp"

namespace lumen::detail {

static_assert(extract_class_name("void Detector::b(std::function<void(int)>)") == "Detector");
static_assert(is_class_name_char('_') && !is_class_name_char(':'));

}  // namespace lumen::detail
