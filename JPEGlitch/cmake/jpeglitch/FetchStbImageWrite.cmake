# Provides an imported target `jpeglitch::stb_image_write` wrapping the
# single-header, public-domain stb_image_write.h (used only by tools/jpeglitch-cli
# to write the decoded/glitched preview as a PNG).

if(TARGET jpeglitch::stb_image_write)
  return()
endif()

set(JPEGLITCH_STB_COMMIT "78770d85c4d94787a56c7859017da963bc4f3439")
set(JPEGLITCH_STB_DIR "${CMAKE_BINARY_DIR}/_deps/stb-src")
set(JPEGLITCH_STB_HEADER "${JPEGLITCH_STB_DIR}/stb_image_write.h")

if(NOT EXISTS "${JPEGLITCH_STB_HEADER}")
  file(MAKE_DIRECTORY "${JPEGLITCH_STB_DIR}")
  message(STATUS "jpeglitch: downloading stb_image_write.h@${JPEGLITCH_STB_COMMIT}")
  file(
    DOWNLOAD
      "https://raw.githubusercontent.com/nothings/stb/${JPEGLITCH_STB_COMMIT}/stb_image_write.h"
      "${JPEGLITCH_STB_HEADER}"
    TLS_VERIFY ON
  )
endif()

add_library(jpeglitch::stb_image_write INTERFACE IMPORTED)
target_include_directories(jpeglitch::stb_image_write INTERFACE "${JPEGLITCH_STB_DIR}")
