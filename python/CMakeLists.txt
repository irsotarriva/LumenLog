pybind11_add_module(lumen_bindings
    lumen.cpp
)

target_link_libraries(lumen_bindings PRIVATE lumen)
target_compile_features(lumen_bindings PRIVATE cxx_std_20)
