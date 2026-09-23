# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Test suite. Every test is a real CTest test; none of them rely on a timeout,
# a sleep or a retry loop to pass.

add_library(latobs_test_support STATIC
  support/support.cpp
  support/test_framework.cpp)
target_link_libraries(latobs_test_support PUBLIC latobs::runtime latobs::store latobs::attribute latobs::baseline latobs::ingest latobs::stats latobs::model latobs::core)
target_include_directories(latobs_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_definitions(latobs_test_support PUBLIC
  LATOBS_TOOL_PATH="$<TARGET_FILE:latobs>")
latobs_configure_target(latobs_test_support)

# The tool path is only meaningful once the tool exists.
if(TARGET latobs)
  add_dependencies(latobs_test_support latobs)
endif()

function(latobs_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE latobs_test_support)
  latobs_configure_target(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    TIMEOUT 0)
endfunction()

latobs_add_test(t_unit_core t_unit_core.cpp)
latobs_add_test(t_unit_stats t_unit_stats.cpp)
latobs_add_test(t_model_clocks t_model_clocks.cpp)
latobs_add_test(t_ingest_fences t_ingest_fences.cpp)
latobs_add_test(t_missing_hops t_missing_hops.cpp)
latobs_add_test(t_incomparable_clocks t_incomparable_clocks.cpp)
latobs_add_test(t_baselines t_baselines.cpp)
latobs_add_test(t_attribution t_attribution.cpp)
latobs_add_test(t_determinism t_determinism.cpp)
latobs_add_test(t_store_restart t_store_restart.cpp)
latobs_add_test(t_concurrency t_concurrency.cpp)
latobs_add_test(t_service_protocol t_service_protocol.cpp)
latobs_add_test(t_adversarial t_adversarial.cpp)
latobs_add_test(t_transport_e2e t_transport_e2e.cpp)
latobs_add_test(t_cli_e2e t_cli_e2e.cpp)

# ---------------------------------------------------------------------------
# Install and downstream consumption proof.
#
# The package is installed into a scratch prefix and an independent CMake
# project is configured against it, built and run. The consumer does not share a
# single build file with this project.
# ---------------------------------------------------------------------------
add_test(NAME package_install
  COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix
          ${CMAKE_BINARY_DIR}/package-prefix)
set_tests_properties(package_install PROPERTIES FIXTURES_SETUP package TIMEOUT 0)

add_test(NAME downstream_find_package
  COMMAND ${CMAKE_COMMAND}
          -DBUILD_DIR=${CMAKE_BINARY_DIR}
          -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -DWORK_DIR=${CMAKE_BINARY_DIR}/downstream-work
          -DCONFIG=${CMAKE_BUILD_TYPE}
          -DCONSUMER_ASAN=${LATOBS_ENABLE_ASAN}
          -DGENERATOR=${CMAKE_GENERATOR}
          -DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/run_downstream_test.cmake)
set_tests_properties(downstream_find_package PROPERTIES
  FIXTURES_REQUIRED package
  TIMEOUT 0)
