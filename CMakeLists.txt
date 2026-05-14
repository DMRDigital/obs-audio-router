cmake_minimum_required(VERSION 3.16)
project(obs-audio-router VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD   11)

# -------------------------------------------------------------------------
# Find OBS
# -------------------------------------------------------------------------
if(NOT DEFINED OBS_SOURCE_DIR)
  # When built by GitHub Actions via obs-plugintemplate, OBS is provided
  # via cmake package. Try both approaches.
  find_package(libobs QUIET)
endif()

if(NOT libobs_FOUND AND NOT DEFINED OBS_SOURCE_DIR)
  message(FATAL_ERROR
    "Could not find OBS Studio. Set OBS_SOURCE_DIR or install libobs.")
endif()

# -------------------------------------------------------------------------
# Plugin target
# -------------------------------------------------------------------------
add_library(obs-audio-router MODULE
  src/plugin-main.cpp
  src/audio-sender.cpp
  src/audio-receiver.cpp
)

target_include_directories(obs-audio-router PRIVATE
  src
)

if(libobs_FOUND)
  target_link_libraries(obs-audio-router PRIVATE OBS::libobs)
else()
  target_include_directories(obs-audio-router PRIVATE
    "${OBS_SOURCE_DIR}/libobs"
    "${OBS_SOURCE_DIR}/UI/obs-frontend-api"
  )
  target_link_directories(obs-audio-router PRIVATE
    "${OBS_BUILD_DIR}/libobs"
  )
  target_link_libraries(obs-audio-router PRIVATE obs)
endif()

# Windows: link Winsock
if(WIN32)
  target_link_libraries(obs-audio-router PRIVATE ws2_32)
endif()

# -------------------------------------------------------------------------
# Install paths (matches OBS plugin layout)
# -------------------------------------------------------------------------
if(WIN32)
  set(OBS_PLUGIN_DIR
    "$ENV{ProgramFiles}/obs-studio/obs-plugins/64bit"
    CACHE PATH "OBS plugins directory")
  install(TARGETS obs-audio-router
    LIBRARY DESTINATION "${OBS_PLUGIN_DIR}")
elseif(APPLE)
  install(TARGETS obs-audio-router
    LIBRARY DESTINATION
      "$ENV{HOME}/Library/Application Support/obs-studio/plugins/obs-audio-router/bin")
else()
  install(TARGETS obs-audio-router
    LIBRARY DESTINATION
      "$ENV{HOME}/.config/obs-studio/plugins/obs-audio-router/bin/64bit")
endif()

# -------------------------------------------------------------------------
# Package for distribution (used by GitHub Actions)
# -------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME            "obs-audio-router")
set(CPACK_PACKAGE_VERSION         "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OBS plugin: route audio tracks between two PCs")
set(CPACK_GENERATOR               "ZIP")
include(CPack)
