# Provides an imported target `jpeglitch::turbojpeg` exposing the TurboJPEG-3 API.
#
# Resolution order:
#   1. JPEGLITCH_SYSTEM_JPEGTURBO=ON (user opt-in, e.g. on Linux distro packaging) ->
#      require a system libjpeg-turbo via pkg-config/find_package.
#   2. A system libjpeg-turbo providing the tj3*() API is already present
#      (common on a dev machine with Homebrew/apt) AND it can satisfy every
#      architecture this build targets -> use it, for fast local iteration.
#      (A Homebrew/apt libjpeg-turbo is normally single-arch, so it's skipped
#      for a multi-arch macOS build, e.g. the "arm64;x86_64" Universal preset.)
#   3. Otherwise -> download and build libjpeg-turbo from source as a static
#      library via ExternalProject_Add() (libjpeg-turbo's own CMakeLists.txt
#      explicitly refuses to be add_subdirectory()'d/FetchContent_MakeAvailable
#      -d into another project), so CI runners and end users never need to
#      install anything, and the result still honors this build's own
#      CMAKE_OSX_ARCHITECTURES/deployment target/etc.
#
# This keeps the default distributable build statically linked (per the
# project spec), while still allowing "use the system library" on Linux and
# avoiding a multi-minute from-source build on a dev machine that already has
# a (single-arch) one.

option(JPEGLITCH_SYSTEM_JPEGTURBO "Require a system-installed libjpeg-turbo instead of building it statically" OFF)

if(TARGET jpeglitch::turbojpeg)
  return()
endif()

list(LENGTH CMAKE_OSX_ARCHITECTURES _jpeglitch_osx_arch_count)
if(_jpeglitch_osx_arch_count GREATER 1)
  set(JPEGLITCH_PC_TURBOJPEG_FOUND FALSE)
  message(STATUS "jpeglitch: multi-arch macOS build (${CMAKE_OSX_ARCHITECTURES}) -- skipping system libjpeg-turbo")
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(JPEGLITCH_PC_TURBOJPEG QUIET libturbojpeg)
    if(JPEGLITCH_PC_TURBOJPEG_FOUND)
      include(CheckCSourceCompiles)
      set(_jpeglitch_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
      set(CMAKE_REQUIRED_INCLUDES "${JPEGLITCH_PC_TURBOJPEG_INCLUDE_DIRS}")
      check_c_source_compiles(
        "
        #include <turbojpeg.h>
        #ifndef TJPARAM_STOPONWARNING
        #error \"libturbojpeg is missing the TurboJPEG-3 API\"
        #endif
        int main(void) {
          tjhandle (*init_fn)(int) = tj3Init;
          int (*set_fn)(tjhandle, int, int) = tj3Set;
          tjhandle h = init_fn(TJINIT_DECOMPRESS);
          return set_fn(h, TJPARAM_STOPONWARNING, 0);
        }
        "
        JPEGLITCH_PC_TURBOJPEG_HAS_TJ3_API
      )
      set(CMAKE_REQUIRED_INCLUDES "${_jpeglitch_saved_required_includes}")
      if(NOT JPEGLITCH_PC_TURBOJPEG_HAS_TJ3_API)
        set(JPEGLITCH_PC_TURBOJPEG_FOUND FALSE)
        message(STATUS "jpeglitch: system libjpeg-turbo lacks tj3 API; building from source")
      endif()
    endif()
  endif()
endif()

if(JPEGLITCH_PC_TURBOJPEG_FOUND)
  add_library(jpeglitch::turbojpeg INTERFACE IMPORTED)
  target_include_directories(jpeglitch::turbojpeg INTERFACE ${JPEGLITCH_PC_TURBOJPEG_INCLUDE_DIRS})
  target_link_directories(jpeglitch::turbojpeg INTERFACE ${JPEGLITCH_PC_TURBOJPEG_LIBRARY_DIRS})
  target_link_libraries(jpeglitch::turbojpeg INTERFACE ${JPEGLITCH_PC_TURBOJPEG_LIBRARIES})
  message(STATUS "jpeglitch: using system libjpeg-turbo ${JPEGLITCH_PC_TURBOJPEG_VERSION} (via pkg-config)")
elseif(JPEGLITCH_SYSTEM_JPEGTURBO)
  message(
    FATAL_ERROR
    "jpeglitch: JPEGLITCH_SYSTEM_JPEGTURBO is ON but no usable system libjpeg-turbo (libturbojpeg) was found via pkg-config"
  )
else()
  message(STATUS "jpeglitch: no usable system libjpeg-turbo found, building it statically from source")

  include(ExternalProject)
  set(_jpeglitch_tj_prefix "${CMAKE_BINARY_DIR}/_deps/libjpeg-turbo")

  # SIMD needs NASM/yasm and gets fiddly to get right for a cross/universal
  # build; turned off for now since the glitch filter's bottleneck is the
  # byte-level corruption pass, not JPEG codec throughput. Revisit once
  # milestone 4's performance measurements call for it.
  set(
    _jpeglitch_tj_cmake_args
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
    -DENABLE_SHARED=OFF
    -DENABLE_STATIC=ON
    -DWITH_TURBOJPEG=ON
    -DWITH_JAVA=OFF
    -DWITH_SIMD=OFF
  )

  list(LENGTH CMAKE_OSX_ARCHITECTURES _jpeglitch_osx_build_arch_count)
  if(_jpeglitch_osx_build_arch_count GREATER 1)
    # libjpeg-turbo's own build explicitly refuses more than one value in
    # CMAKE_OSX_ARCHITECTURES ("contains assembly code"); upstream's own
    # release process instead builds one static lib per architecture and
    # combines them with `lipo`, which is what we do here too.
    set(_jpeglitch_tj_arch_libs "")
    foreach(_jpeglitch_arch ${CMAKE_OSX_ARCHITECTURES})
      set(_jpeglitch_tj_arch_install "${_jpeglitch_tj_prefix}/install-${_jpeglitch_arch}")
      set(_jpeglitch_tj_arch_lib "${_jpeglitch_tj_arch_install}/lib/libturbojpeg.a")
      ExternalProject_Add(
        jpeglitch_libjpeg_turbo_ext_${_jpeglitch_arch}
        GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
        GIT_TAG 3.0.4
        GIT_SHALLOW TRUE
        SOURCE_DIR "${_jpeglitch_tj_prefix}/src-${_jpeglitch_arch}"
        CMAKE_ARGS
          ${_jpeglitch_tj_cmake_args} -DCMAKE_INSTALL_PREFIX=${_jpeglitch_tj_arch_install}
          -DCMAKE_OSX_ARCHITECTURES=${_jpeglitch_arch}
        BUILD_BYPRODUCTS "${_jpeglitch_tj_arch_lib}"
        INSTALL_DIR "${_jpeglitch_tj_arch_install}"
      )
      list(APPEND _jpeglitch_tj_arch_libs "${_jpeglitch_tj_arch_lib}")
    endforeach()

    set(_jpeglitch_tj_install "${_jpeglitch_tj_prefix}/install")
    set(_jpeglitch_tj_lib "${_jpeglitch_tj_install}/lib/libturbojpeg.a")
    file(MAKE_DIRECTORY "${_jpeglitch_tj_install}/lib")

    add_custom_command(
      OUTPUT "${_jpeglitch_tj_lib}"
      COMMAND lipo -create ${_jpeglitch_tj_arch_libs} -output "${_jpeglitch_tj_lib}"
      DEPENDS ${_jpeglitch_tj_arch_libs}
      COMMENT "jpeglitch: combining per-arch libturbojpeg.a into a universal binary"
    )
    add_custom_target(jpeglitch_libjpeg_turbo_universal DEPENDS "${_jpeglitch_tj_lib}")
    foreach(_jpeglitch_arch ${CMAKE_OSX_ARCHITECTURES})
      # File-level DEPENDS above isn't enough to order this against the
      # per-arch ExternalProject targets with the Xcode generator (unlike
      # Ninja/Makefiles); force it explicitly.
      add_dependencies(jpeglitch_libjpeg_turbo_universal jpeglitch_libjpeg_turbo_ext_${_jpeglitch_arch})
    endforeach()

    list(GET CMAKE_OSX_ARCHITECTURES 0 _jpeglitch_first_arch)
    file(MAKE_DIRECTORY "${_jpeglitch_tj_install}/include")
    add_custom_command(
      TARGET jpeglitch_libjpeg_turbo_universal
      POST_BUILD
      COMMAND
        ${CMAKE_COMMAND} -E copy_directory "${_jpeglitch_tj_prefix}/install-${_jpeglitch_first_arch}/include"
        "${_jpeglitch_tj_install}/include"
    )

    add_library(jpeglitch::turbojpeg STATIC IMPORTED GLOBAL)
    set_target_properties(jpeglitch::turbojpeg PROPERTIES IMPORTED_LOCATION "${_jpeglitch_tj_lib}")
    target_include_directories(jpeglitch::turbojpeg INTERFACE "${_jpeglitch_tj_install}/include")
    add_dependencies(jpeglitch::turbojpeg jpeglitch_libjpeg_turbo_universal)
  else()
    set(_jpeglitch_tj_install "${_jpeglitch_tj_prefix}/install")
    if(WIN32)
      set(_jpeglitch_tj_lib "${_jpeglitch_tj_install}/lib/turbojpeg-static.lib")
    else()
      set(_jpeglitch_tj_lib "${_jpeglitch_tj_install}/lib/libturbojpeg.a")
    endif()

    ExternalProject_Add(
      jpeglitch_libjpeg_turbo_ext
      GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
      GIT_TAG 3.0.4
      GIT_SHALLOW TRUE
      PREFIX "${_jpeglitch_tj_prefix}"
      CMAKE_ARGS
        ${_jpeglitch_tj_cmake_args} -DCMAKE_INSTALL_PREFIX=${_jpeglitch_tj_install}
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
      BUILD_BYPRODUCTS "${_jpeglitch_tj_lib}"
      INSTALL_DIR "${_jpeglitch_tj_install}"
    )

    file(MAKE_DIRECTORY "${_jpeglitch_tj_install}/include")

    add_library(jpeglitch::turbojpeg STATIC IMPORTED GLOBAL)
    set_target_properties(jpeglitch::turbojpeg PROPERTIES IMPORTED_LOCATION "${_jpeglitch_tj_lib}")
    target_include_directories(jpeglitch::turbojpeg INTERFACE "${_jpeglitch_tj_install}/include")
    add_dependencies(jpeglitch::turbojpeg jpeglitch_libjpeg_turbo_ext)
  endif()
endif()
