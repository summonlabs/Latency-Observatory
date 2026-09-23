# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Sanitizer support is applied inside latobs_configure_target (see
# CompilerWarnings.cmake). Availability is reported honestly:
#   * AddressSanitizer: MSVC x64 (/fsanitize=address) and GCC/Clang.
#   * UndefinedBehaviorSanitizer: GCC/Clang only; MSVC has no equivalent.
# The build summary below states what is actually active.
message(STATUS "latobs sanitizers: asan=${LATOBS_ENABLE_ASAN} ubsan=${LATOBS_ENABLE_UBSAN}")
