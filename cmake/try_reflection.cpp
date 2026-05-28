#if __has_include(<experimental/meta>)
#include <experimental/meta>
#elif __has_include(<meta>)
#include <meta>
#else
#error C++26 static reflection header not found
#endif

#include <string_view>

consteval std::string_view probe_class_name() {
    return std::meta::name_of(^int);
}

static_assert(probe_class_name() == "int" || probe_class_name() == "int ",
              "reflection operator must return typename");

int main() { return 0; }
