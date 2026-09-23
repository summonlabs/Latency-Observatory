# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

add_executable(latobs_quickstart quickstart.cpp)
target_link_libraries(latobs_quickstart PRIVATE latobs::runtime)
latobs_configure_target(latobs_quickstart)

add_executable(latobs_service_client service_client.cpp)
target_link_libraries(latobs_service_client PRIVATE latobs::runtime)
latobs_configure_target(latobs_service_client)

install(TARGETS latobs_quickstart latobs_service_client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
