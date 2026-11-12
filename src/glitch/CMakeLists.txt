# Standalone, OBS-free glitch core library.
# Can be built on its own (cd src/glitch && cmake -B build && cmake --build build && ctest --test-dir build)
# or pulled in via add_subdirectory() from the plugin's top-level CMakeLists.txt.

cmake_minimum_required(VERSION 3.20)

if(NOT TARGET jpeglitch-glitch-core)
  project(jpeglitch-glitch-core C)

  add_library(jpeglitch-glitch-core STATIC jpeglitch_core.c jpeglitch_mt19937.c)
  target_include_directories(jpeglitch-glitch-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  if(NOT WIN32)
    target_link_libraries(jpeglitch-glitch-core PUBLIC m)
  endif()
  set_target_properties(jpeglitch-glitch-core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)

  option(JPEGLITCH_BUILD_TESTS "Build jpeglitch-glitch-core unit tests" ${PROJECT_IS_TOP_LEVEL})
  if(JPEGLITCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
  endif()
endif()
