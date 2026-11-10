# Install rules and the CMake package, so that another project can use
#   find_package(lumen 0.1 CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE lumen::lumen)

include(CMakePackageConfigHelpers)

set(LUMEN_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/lumen" CACHE STRING
    "Install directory of the lumen CMake package files, relative to the prefix")

set_target_properties(lumen PROPERTIES EXPORT_NAME lumen)

install(TARGETS lumen
  EXPORT lumenTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# The whole public tree, bridges included. src/ holds only private headers.
install(DIRECTORY include/lumen
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp"
)

install(EXPORT lumenTargets
  NAMESPACE lumen::
  DESTINATION ${LUMEN_INSTALL_CMAKEDIR}
)

# Read by lumenConfig.cmake.in: a static lumen carries its private link
# dependencies, so the package must find them for the consumer.
set(LUMEN_PACKAGE_NEEDS_FTXUI ${LUMEN_ENABLE_DASHBOARD})

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/lumenConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/lumenConfig.cmake"
  INSTALL_DESTINATION ${LUMEN_INSTALL_CMAKEDIR}
)
# 0.x releases may break the API in any minor version.
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/lumenConfigVersion.cmake"
  COMPATIBILITY SameMinorVersion
)
install(FILES
  "${PROJECT_BINARY_DIR}/lumenConfig.cmake"
  "${PROJECT_BINARY_DIR}/lumenConfigVersion.cmake"
  DESTINATION ${LUMEN_INSTALL_CMAKEDIR}
)
