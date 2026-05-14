cmake_minimum_required(VERSION 3.16)
project(obs-audio-router VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD   11)

# -------------------------------------------------------------------------
# Find OBS headers and import lib
# OBS_INCLUDE_DIR  — directory containing obs-module.h  (set by CI)
# OBS_LIB_PATH     — full path to obs.lib               (set by CI)
# libobs cmake package is tried as a fallback for local builds
# -------------------------------------------------------------------------
if(NOT DEFINED OBS_INCLUDE_DIR)
  find_package(libobs QUIET)
  if(NOT libobs_FOUND)
    message(FATAL_ERROR
      "OBS not found. Pass -DOBS_INCLUDE_DIR=/path/to/obs/include "
      "and -DOBS_LIB_PATH=/path/to/obs.lib, or install libobs.")
  endif()
endif()

# -------------------------------------------------------------------------
# Plugin target
# -------------------------------------------------------------------------
add_library(obs-audio-router MODULE
  src/plugin-main.cpp
  src/audio-sender.cpp
  src/audio-receiver.cpp
)

target_include_directories(obs-audio-router PRIVATE src)

if(libobs_FOUND)
  target_link_libraries(obs-audio-router PRIVATE OBS::libobs)
else()
  target_include_directories(obs-audio-router PRIVATE "${OBS_INCLUDE_DIR}")

  if(DEFINED OBS_LIB_PATH AND NOT OBS_LIB_PATH STREQUAL "")
    target_link_libraries(obs-audio-router PRIVATE "${OBS_LIB_PATH}")
  else()
    # Try to find obs.lib relative to the include dir
    find_library(OBS_LIB NAMES obs libobs
      HINTS "${OBS_INCLUDE_DIR}/.." "${OBS_INCLUDE_DIR}/../lib"
            "${OBS_INCLUDE_DIR}/../../lib"
      PATH_SUFFIXES "" lib bin)
    if(OBS_LIB)
      target_link_libraries(obs-audio-router PRIVATE "${OBS_LIB}")
    else()
      message(WARNING "obs.lib not found — plugin will resolve obs.dll at load time")
    endif()
  endif()
endif()

# Windows: Winsock
if(WIN32)
  target_link_libraries(obs-audio-router PRIVATE ws2_32)
endif()

# -------------------------------------------------------------------------
# Install
# -------------------------------------------------------------------------
if(WIN32)
  set(OBS_PLUGIN_DIR "$ENV{ProgramFiles}/obs-studio/obs-plugins/64bit"
    CACHE PATH "OBS plugins directory")
  install(TARGETS obs-audio-router LIBRARY DESTINATION "${OBS_PLUGIN_DIR}")
elseif(APPLE)
  install(TARGETS obs-audio-router LIBRARY DESTINATION
    "$ENV{HOME}/Library/Application Support/obs-studio/plugins/obs-audio-router/bin")
else()
  install(TARGETS obs-audio-router LIBRARY DESTINATION
    "$ENV{HOME}/.config/obs-studio/plugins/obs-audio-router/bin/64bit")
endif()

# -------------------------------------------------------------------------
# CPack
# -------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "obs-audio-router")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OBS plugin: route audio between two PCs")
set(CPACK_GENERATOR "ZIP")
include(CPack)
