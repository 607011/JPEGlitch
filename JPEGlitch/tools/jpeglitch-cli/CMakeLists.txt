cmake_minimum_required(VERSION 3.20)

project(jpeglitch-cli C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/jpeglitch")
include(FindOrFetchTurboJPEG)
include(FetchStbImageWrite)

if(NOT TARGET jpeglitch-glitch-core)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/glitch" "${CMAKE_CURRENT_BINARY_DIR}/glitch-core")
endif()
if(NOT TARGET jpeglitch-codec)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/codec" "${CMAKE_CURRENT_BINARY_DIR}/codec")
endif()

add_executable(jpeglitch-cli main.c)
target_link_libraries(jpeglitch-cli PRIVATE jpeglitch-glitch-core jpeglitch-codec jpeglitch::stb_image_write)
set_target_properties(jpeglitch-cli PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

option(JPEGLITCH_BUILD_CODEC_TESTS "Build the libjpeg-turbo codec robustness test" ${PROJECT_IS_TOP_LEVEL})
if(JPEGLITCH_BUILD_CODEC_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
