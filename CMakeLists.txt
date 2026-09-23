# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Latency Observatory - standalone, vendor-neutral Fabric OS runtime for
# end-to-end and per-hop latency observation and attribution.
cmake_minimum_required(VERSION 3.25)

project(latency_observatory
  VERSION 1.0.0
  DESCRIPTION "Latency Observatory: latency observation and attribution runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Latency-Observatory"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(LATOBS_BUILD_TOOLS "Build command line tooling" ON)
option(LATOBS_BUILD_EXAMPLES "Build examples" ON)
option(LATOBS_BUILD_BENCHMARKS "Build benchmarks" ON)
option(LATOBS_BUILD_TESTS "Build tests" ON)
option(LATOBS_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(LATOBS_ENABLE_ASAN "Enable AddressSanitizer for first-party targets" OFF)
option(LATOBS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (clang/gcc only)" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CompilerWarnings)
include(Sanitizers)

if(MSVC)
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_subdirectory(src)

if(LATOBS_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(LATOBS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(LATOBS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(LATOBS_BUILD_TESTS AND BUILD_TESTING)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
set(LATOBS_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/LatencyObservatory"
    CACHE STRING "Install location for CMake package files")

install(EXPORT LatencyObservatoryTargets
  FILE LatencyObservatoryTargets.cmake
  NAMESPACE latobs::
  DESTINATION "${LATOBS_INSTALL_CMAKEDIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LatencyObservatoryConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/LatencyObservatoryConfig.cmake"
  INSTALL_DESTINATION "${LATOBS_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/LatencyObservatoryConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/LatencyObservatoryConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/LatencyObservatoryConfigVersion.cmake"
  DESTINATION "${LATOBS_INSTALL_CMAKEDIR}")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

message(STATUS "latency_observatory ${PROJECT_VERSION} | build type: ${CMAKE_BUILD_TYPE} | tests: ${LATOBS_BUILD_TESTS} | asan: ${LATOBS_ENABLE_ASAN}")
