# Terakan: a CMake front end for the Meson build.
#
# This repository is a Mesa fork -- 308 meson.build files, a 2400-line root one and 330 rules that
# run code generators -- and Mesa's build is described in Meson. Restating that in CMake would be a
# rewrite rather than a port, and it would be discarded by the next rebase onto upstream Mesa. So
# this file does not duplicate any of it: it drives the same Meson build with the same options
# bin/terakan-build passes, which makes
#
#     cmake -B build && cmake --build build && ctest --test-dir build
#
# work without a wrapper script, while the build description stays in one place.

cmake_minimum_required(VERSION 3.20)

# No compiler is looked for here: Meson finds its own toolchain, and skipping the checks keeps
# configuration to the Meson run itself.
project(terakan
        DESCRIPTION "Terakan: a Vulkan driver for AMD TeraScale (R600-R900) GPUs"
        LANGUAGES NONE)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
          "in-source builds are not supported; use a separate directory, for instance "
          "cmake -B build")
endif()

find_program(TERAKAN_MESON_EXECUTABLE NAMES meson
             DOC "Meson, which describes and drives the actual build")
find_program(TERAKAN_NINJA_EXECUTABLE NAMES ninja samu
             DOC "The backend Meson compiles with")
if(NOT TERAKAN_MESON_EXECUTABLE)
  message(FATAL_ERROR "meson was not found; it builds this project and CMake only drives it")
endif()
if(NOT TERAKAN_NINJA_EXECUTABLE)
  message(FATAL_ERROR "ninja was not found; Meson needs it as its build backend")
endif()

# --- Options, one per knob bin/terakan-build reads from the environment ----------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Release Debug RelWithDebInfo MinSizeRel)

set(TERAKAN_TARGET_GENERATION "" CACHE STRING
    "TeraScale generation to build for: auto, r600, r700, r800 or r900. Empty leaves the Meson \
default, which is auto -- and auto reads the AMD card in this machine, so a machine with an \
Evergreen card labels an R700 build r800 or r900. Set it explicitly when building for a \
generation other than the one installed here.")
set_property(CACHE TERAKAN_TARGET_GENERATION PROPERTY STRINGS "" auto r600 r700 r800 r900)

set(TERAKAN_CCACHE_DIR "${CMAKE_CURRENT_BINARY_DIR}/meson/.ccache" CACHE PATH
    "Compiler cache directory, kept out of the user's own ccache so a Terakan build cannot evict \
their unrelated objects")

set(TERAKAN_PKG_CONFIG_PATH "/usr/lib/pkgconfig:/usr/share/pkgconfig" CACHE STRING
    "PKG_CONFIG_PATH for the Meson run. Empty leaves the caller's environment alone.")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  # Matching bin/terakan-build: an ICD is only found by the Vulkan loader from a system prefix,
  # and /usr/local is not one on the distributions this driver targets.
  set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

set(TERAKAN_MESON_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/meson")
set(TERAKAN_ICD "${TERAKAN_MESON_BUILD_DIR}/src/amd/terascale/vulkan/libvulkan_terascale.so")

set(_terakan_buildtype_Release        release)
set(_terakan_buildtype_Debug          debug)
set(_terakan_buildtype_RelWithDebInfo debugoptimized)
set(_terakan_buildtype_MinSizeRel     minsize)
if(NOT DEFINED _terakan_buildtype_${CMAKE_BUILD_TYPE})
  message(FATAL_ERROR
          "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} has no Meson equivalent; use Release, Debug, "
          "RelWithDebInfo or MinSizeRel")
endif()
set(TERAKAN_MESON_BUILDTYPE ${_terakan_buildtype_${CMAKE_BUILD_TYPE}})

# Exactly the set bin/terakan-build passes: only the amd_terascale Vulkan driver, no Gallium, no
# GL/GLES/EGL/GBM entry points and no LLVM, so a build stays to the driver and its own tests.
#
# This list is duplicated from that script and the two must be changed together, or the same source
# would build differently depending on which front end was used. Folding both onto one Meson native
# file with a [project options] section would remove the duplication, at the cost of changing the
# script every other tool here already depends on.
set(TERAKAN_MESON_OPTIONS
    --prefix=${CMAKE_INSTALL_PREFIX}
    --libdir=lib
    --buildtype=${TERAKAN_MESON_BUILDTYPE}
    -Dwrap_mode=nodownload
    -Dvulkan-drivers=amd_terascale
    -Dgallium-drivers=
    -Dglx=disabled
    -Degl=disabled
    -Dgbm=disabled
    -Dopengl=false
    -Dgles1=disabled
    -Dgles2=disabled
    -Dllvm=disabled
    -Dvalgrind=disabled
    -Dlmsensors=disabled
    -Dzstd=enabled
    -Dshader-cache=enabled
    -Dlibunwind=disabled
    -Dplatforms=x11,wayland
    -Dbuild-tests=true
    -Dtools=[])
if(TERAKAN_TARGET_GENERATION)
  # Passed on setup and on every reconfigure, so switching an existing build directory between
  # generations works without a wipe.
  list(APPEND TERAKAN_MESON_OPTIONS -Dterakan-target-generation=${TERAKAN_TARGET_GENERATION})
endif()

set(TERAKAN_MESON_ENV "CCACHE_DIR=${TERAKAN_CCACHE_DIR}")
if(TERAKAN_PKG_CONFIG_PATH)
  list(APPEND TERAKAN_MESON_ENV "PKG_CONFIG_PATH=${TERAKAN_PKG_CONFIG_PATH}")
endif()

# --- Configure the Meson build ---------------------------------------------------------------

file(MAKE_DIRECTORY "${TERAKAN_CCACHE_DIR}")

if(EXISTS "${TERAKAN_MESON_BUILD_DIR}/meson-private/coredata.dat")
  set(_terakan_setup_mode --reconfigure)
else()
  set(_terakan_setup_mode)
endif()

message(STATUS "Configuring the Meson build in ${TERAKAN_MESON_BUILD_DIR}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env ${TERAKAN_MESON_ENV}
          "${TERAKAN_MESON_EXECUTABLE}" setup ${_terakan_setup_mode}
          "${TERAKAN_MESON_BUILD_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}"
          ${TERAKAN_MESON_OPTIONS}
  RESULT_VARIABLE _terakan_setup_result
  OUTPUT_VARIABLE _terakan_setup_output
  ERROR_VARIABLE _terakan_setup_output)
if(NOT _terakan_setup_result EQUAL 0)
  message(FATAL_ERROR "meson setup failed:\n${_terakan_setup_output}")
endif()

# Reconfigure when the Meson description changes, the way CMake would for its own input.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/meson.build"
             "${CMAKE_CURRENT_SOURCE_DIR}/meson.options")

# --- Build -----------------------------------------------------------------------------------

add_custom_target(terakan ALL
  COMMAND ${CMAKE_COMMAND} -E env ${TERAKAN_MESON_ENV}
          "${TERAKAN_MESON_EXECUTABLE}" compile -C "${TERAKAN_MESON_BUILD_DIR}"
  COMMAND ${CMAKE_COMMAND} -DTERAKAN_ICD=${TERAKAN_ICD}
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/terakan_check_icd.cmake"
  BYPRODUCTS "${TERAKAN_ICD}"
  USES_TERMINAL
  COMMENT "Building the Terakan Vulkan ICD"
  VERBATIM)

# `cmake --build build --target terakan-wipe` is Meson's wipe, the equivalent of
# bin/terakan-build --clean. It reconfigures from scratch and keeps no object files.
add_custom_target(terakan-wipe
  COMMAND ${CMAKE_COMMAND} -E env ${TERAKAN_MESON_ENV}
          "${TERAKAN_MESON_EXECUTABLE}" setup --wipe "${TERAKAN_MESON_BUILD_DIR}"
          "${CMAKE_CURRENT_SOURCE_DIR}" ${TERAKAN_MESON_OPTIONS}
  USES_TERMINAL
  COMMENT "Wiping and reconfiguring the Meson build"
  VERBATIM)

# --- Tests -----------------------------------------------------------------------------------

enable_testing()

# Taken from Meson rather than restated here, so a test added to meson.build appears in ctest
# without this file being touched. Mesa's own unrelated tests are left out: every Terakan test is
# named terakan_*, which is the same set bin/terakan-test runs.
#
# Meson is left to rebuild before each test rather than passing --no-rebuild. The SPIR-V a GPU test
# loads is a test dependency that `meson compile` does not build by default, and without this the
# twenty-three tests that take shader arguments fail on a missing .spv rather than on the driver.
execute_process(
  COMMAND "${TERAKAN_MESON_EXECUTABLE}" test -C "${TERAKAN_MESON_BUILD_DIR}" --list
  OUTPUT_VARIABLE _terakan_test_list
  ERROR_VARIABLE _terakan_test_list_error
  RESULT_VARIABLE _terakan_test_list_result)
if(NOT _terakan_test_list_result EQUAL 0)
  message(WARNING "meson test --list failed, so no tests were registered:\n"
                  "${_terakan_test_list_error}")
else()
  string(REPLACE "\n" ";" _terakan_test_lines "${_terakan_test_list}")
  set(_terakan_test_count 0)
  foreach(_terakan_test_line IN LISTS _terakan_test_lines)
    string(STRIP "${_terakan_test_line}" _terakan_test_line)
    if(_terakan_test_line MATCHES "^mesa:(terakan_[A-Za-z0-9_]+)$")
      set(_terakan_test_name "${CMAKE_MATCH_1}")
      add_test(NAME ${_terakan_test_name}
               COMMAND ${CMAKE_COMMAND} -E env ${TERAKAN_MESON_ENV}
                       "${TERAKAN_MESON_EXECUTABLE}" test -C "${TERAKAN_MESON_BUILD_DIR}"
                       --print-errorlogs --num-processes 1 ${_terakan_test_name})
      # Most of these drive the real GPU through the freshly built ICD and cannot share it, which
      # is why bin/terakan-test runs the GPU set with --num-processes 1. One lock across all of
      # them keeps ctest -j honest at the cost of serialising the thirteen CPU tests too.
      set_tests_properties(${_terakan_test_name} PROPERTIES RESOURCE_LOCK terakan_gpu)
      math(EXPR _terakan_test_count "${_terakan_test_count} + 1")
    endif()
  endforeach()
  message(STATUS "Registered ${_terakan_test_count} Terakan tests with ctest")
endif()

# --- Install ---------------------------------------------------------------------------------

# The prefix is fixed when Meson is configured, above, so this only has to honour DESTDIR.
install(CODE "
  set(_destdir \"\$ENV{DESTDIR}\")
  if(_destdir)
    set(_destdir_argument --destdir \"\${_destdir}\")
  else()
    set(_destdir_argument)
  endif()
  execute_process(
    COMMAND \"${TERAKAN_MESON_EXECUTABLE}\" install -C \"${TERAKAN_MESON_BUILD_DIR}\"
            \${_destdir_argument}
    RESULT_VARIABLE _install_result)
  if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR \"meson install failed\")
  endif()")
