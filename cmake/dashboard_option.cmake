# LUMEN_ENABLE_DASHBOARD builds DashboardSink, the only code that needs FTXUI.
#
# Left unset, it follows FTXUI: on when find_package(ftxui) succeeds, off
# otherwise, with a status line that says so. A plain add_subdirectory() of
# Lumen therefore configures whether or not FTXUI is installed. Set
# explicitly to ON, FTXUI is required and configuring fails without it; set
# to OFF, FTXUI is not looked for. The value chosen the first time is cached,
# so installing FTXUI later needs -DLUMEN_ENABLE_DASHBOARD=ON (or a fresh
# build directory).

if(DEFINED LUMEN_ENABLE_DASHBOARD)
  if(LUMEN_ENABLE_DASHBOARD)
    find_package(ftxui CONFIG REQUIRED)
  endif()
else()
  find_package(ftxui CONFIG QUIET)
  if(ftxui_FOUND)
    set(lumen_dashboard_default ON)
    message(STATUS "Lumen: FTXUI ${ftxui_VERSION} found, LUMEN_ENABLE_DASHBOARD defaults to ON")
  else()
    set(lumen_dashboard_default OFF)
    message(STATUS "Lumen: FTXUI not found, LUMEN_ENABLE_DASHBOARD defaults to OFF "
                   "(DashboardSink is not built; set CMAKE_PREFIX_PATH or ftxui_DIR to use it)")
  endif()
  set(LUMEN_ENABLE_DASHBOARD "${lumen_dashboard_default}" CACHE BOOL
      "Build the FTXUI terminal dashboard (default: on if FTXUI is found)")
endif()
