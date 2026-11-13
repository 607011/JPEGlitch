# Standalone, OBS-free JPEG decode wrapper around TurboJPEG-3, tolerant of
# broken (glitched) bitstreams. See jpeglitch_codec.h.

cmake_minimum_required(VERSION 3.20)

if(NOT TARGET jpeglitch-codec)
  project(jpeglitch-codec C)

  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/jpeglitch")
  include(FindOrFetchTurboJPEG)

  add_library(jpeglitch-codec STATIC jpeglitch_codec.c)
  target_include_directories(jpeglitch-codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(jpeglitch-codec PUBLIC jpeglitch::turbojpeg)
  set_target_properties(
    jpeglitch-codec
    PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON
  )
endif()
