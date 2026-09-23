# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# First-party warning policy. Every first-party target is compiled with a strict
# warning set and warnings-as-errors. The runtime has no external dependencies,
# so there is no third-party warning surface to suppress.

set(LATOBS_MSVC_WARNINGS
  /W4
  /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546
  /w14547 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)

set(LATOBS_GCC_WARNINGS
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
  -Wnull-dereference -Wdouble-promotion -Wformat=2)

function(latobs_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE ${LATOBS_MSVC_WARNINGS})
    if(LATOBS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE ${LATOBS_GCC_WARNINGS})
    if(LATOBS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  set_target_properties(${target} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Release uses the DLL runtime, Debug uses its debug variant: a library
    # built against one and consumed by the other is a link error, which is
    # exactly what should happen.
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

  if(LATOBS_ENABLE_ASAN)
    target_compile_definitions(${target} PRIVATE LATOBS_ASAN=1)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
  if(LATOBS_ENABLE_UBSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=undefined)
  endif()
endfunction()
