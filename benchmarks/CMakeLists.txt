# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

add_executable(latobs_bench latobs_bench.cpp)
target_link_libraries(latobs_bench PRIVATE latobs::runtime latobs::store)
latobs_configure_target(latobs_bench)
