# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Installs the package into a scratch prefix and builds an independent consumer
# against it. The consumer shares no build files with this project: the only
# contract between them is the installed CMake package.
#
# The consumer is configured with the same generator as this build, because an
# installed single configuration binary package belongs to the configuration it
# was built with.

if(NOT DEFINED BUILD_DIR OR NOT DEFINED SOURCE_DIR OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "BUILD_DIR, SOURCE_DIR and WORK_DIR must be defined")
endif()

set(prefix "${WORK_DIR}/prefix")
set(consumer_build "${WORK_DIR}/consumer-build")
file(REMOVE_RECURSE "${prefix}" "${consumer_build}")

set(config "${CONFIG}")
if(config STREQUAL "")
  set(config "Debug")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${prefix}"
                --config ${config}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed (${install_result}): ${install_output} ${install_error}")
endif()

set(configure_command "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/downstream" -B "${consumer_build}")
if(NOT GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${GENERATOR}")
endif()
if(NOT GENERATOR_PLATFORM STREQUAL "")
  list(APPEND configure_command -A "${GENERATOR_PLATFORM}")
endif()
list(APPEND configure_command -DCMAKE_PREFIX_PATH=${prefix})
list(APPEND configure_command -DLATOBS_CONSUMER_ASAN=${CONSUMER_ASAN})
if(NOT GENERATOR MATCHES "^Visual Studio")
  list(APPEND configure_command -DCMAKE_BUILD_TYPE=${config})
endif()

execute_process(COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream configure failed (${configure_result}): ${configure_output} ${configure_error}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} --build "${consumer_build}" --config ${config}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream build failed (${build_result}): ${build_output} ${build_error}")
endif()

file(GLOB_RECURSE consumer_binaries "${consumer_build}/consumer" "${consumer_build}/consumer.exe")
if(NOT consumer_binaries)
  message(FATAL_ERROR "the downstream consumer binary was not produced")
endif()
list(GET consumer_binaries 0 consumer_binary)

execute_process(COMMAND "${consumer_binary}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "downstream run failed (${run_result}): ${run_output} ${run_error}")
endif()

file(GLOB_RECURSE installed_headers "${prefix}/include/latency_observatory/*.hpp")
if(NOT installed_headers)
  message(FATAL_ERROR "no public headers were installed")
endif()
string(STRIP "${run_output}" run_output)
message(STATUS "downstream consumer: install, find_package, build and run verified")
message(STATUS "downstream consumer output: ${run_output}")
