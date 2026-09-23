# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

add_executable(latobs latobs.cpp)
target_link_libraries(latobs PRIVATE latobs::runtime)
latobs_configure_target(latobs)

install(TARGETS latobs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
