# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# An independent downstream consumer. It shares no build files with the
# repository: it only calls find_package(LatencyObservatory) and links the
# exported targets, which is what proves the install and export rules.

cmake_minimum_required(VERSION 3.25)
project(latency_observatory_consumer LANGUAGES CXX)

find_package(LatencyObservatory 1.0 REQUIRED CONFIG)

# A static library built with AddressSanitizer must be consumed by a binary that
# also uses AddressSanitizer. The package does not hide its configuration: the
# consumer is told which one it is consuming.
option(LATOBS_CONSUMER_ASAN "Build the consumer with AddressSanitizer" OFF)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE latobs::runtime)
# The package exports its version to consumers; the consumer turns it into a
# compile definition to prove the variable is really visible from outside.
target_compile_definitions(consumer PRIVATE
  LATOBS_CONSUMER_VERSION="${LatencyObservatory_VERSION}")
set_target_properties(consumer PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

if(LATOBS_CONSUMER_ASAN)
  if(MSVC)
    target_compile_options(consumer PRIVATE /fsanitize=address)
    target_link_options(consumer PRIVATE /fsanitize=address)
  else()
    target_compile_options(consumer PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(consumer PRIVATE -fsanitize=address)
  endif()
endif()

message(STATUS "consumer: LatencyObservatory ${LatencyObservatory_VERSION} found")
