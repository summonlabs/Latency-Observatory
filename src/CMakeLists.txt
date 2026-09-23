# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Module layering (each layer may only depend on the ones above it):
#   core      identities, checked arithmetic, digests, time, evidence, policy, JSON
#   model     entities, catalog, clock registry, measurement records
#   stats     distributions, histograms, deterministic aggregation
#   ingest    validation and staleness/replay fences
#   baseline  generation bound baselines and comparisons
#   attribute decomposition with semantics gates and anomaly evidence
#   store     versioned, integrity checked persistence
#   runtime   engine, workers, query operations, transport

add_library(latobs_core STATIC
  core/checked.cpp
  core/digest.cpp
  core/error.cpp
  core/evidence.cpp
  core/ids.cpp
  core/json.cpp
  core/policy.cpp
  core/time.cpp)
add_library(latobs::core ALIAS latobs_core)
set_target_properties(latobs_core PROPERTIES EXPORT_NAME core)
target_include_directories(latobs_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
latobs_configure_target(latobs_core)
set_target_properties(latobs_core PROPERTIES OUTPUT_NAME latobs_core)

install(TARGETS latobs_core EXPORT LatencyObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(latobs_model STATIC
  model/clock.cpp
  model/codec.cpp
  model/entities.cpp
  model/measurement.cpp)
add_library(latobs::model ALIAS latobs_model)
set_target_properties(latobs_model PROPERTIES EXPORT_NAME model)
target_link_libraries(latobs_model PUBLIC latobs_core)
latobs_configure_target(latobs_model)

install(TARGETS latobs_model EXPORT LatencyObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(latobs_stats STATIC
  stats/codec.cpp
  stats/distribution.cpp
  stats/summary.cpp)
add_library(latobs::stats ALIAS latobs_stats)
set_target_properties(latobs_stats PROPERTIES EXPORT_NAME stats)
target_link_libraries(latobs_stats PUBLIC latobs_model latobs_core)
latobs_configure_target(latobs_stats)

install(TARGETS latobs_stats EXPORT LatencyObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(latobs_ingest STATIC
  ingest/codec.cpp
  ingest/ingest.cpp)
add_library(latobs::ingest ALIAS latobs_ingest)
set_target_properties(latobs_ingest PROPERTIES EXPORT_NAME ingest)
target_link_libraries(latobs_ingest PUBLIC latobs_stats latobs_model latobs_core)
latobs_configure_target(latobs_ingest)

install(TARGETS latobs_ingest EXPORT LatencyObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(latobs_baseline STATIC
  baseline/baseline.cpp
  baseline/codec.cpp)
add_library(latobs::baseline ALIAS latobs_baseline)
set_target_properties(latobs_baseline PROPERTIES EXPORT_NAME baseline)
target_link_libraries(latobs_baseline PUBLIC latobs_stats latobs_ingest latobs_model latobs_core)
latobs_configure_target(latobs_baseline)

install(TARGETS latobs_baseline EXPORT LatencyObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(latobs_attribute STATIC
  attribute/attribute.cpp)
add_library(latobs::attribute ALIAS latobs_attribute)
set_target_properties(latobs_attribute PROPERTIES EXPORT_NAME attribute)
target_link_libraries(latobs_attribute PUBLIC latobs_baseline latobs_stats latobs_model latobs_core)
latobs_configure_target(latobs_attribute)

install(TARGETS latobs_attribute EXPORT LatencyObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(latobs_store STATIC
  store/store.cpp)
add_library(latobs::store ALIAS latobs_store)
set_target_properties(latobs_store PROPERTIES EXPORT_NAME store)
target_link_libraries(latobs_store PUBLIC latobs_attribute latobs_baseline latobs_ingest latobs_stats latobs_model latobs_core)
latobs_configure_target(latobs_store)

install(TARGETS latobs_store EXPORT LatencyObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(latobs_runtime STATIC
  runtime/demo.cpp
  runtime/engine.cpp
  runtime/export.cpp
  runtime/explain.cpp
  runtime/service.cpp
  runtime/transport.cpp
  runtime/worker_pool.cpp)
add_library(latobs::runtime ALIAS latobs_runtime)
set_target_properties(latobs_runtime PROPERTIES EXPORT_NAME runtime)
target_link_libraries(latobs_runtime PUBLIC latobs_store latobs_attribute latobs_baseline latobs_ingest latobs_stats latobs_model latobs_core)
if(WIN32)
  target_link_libraries(latobs_runtime PUBLIC ws2_32)
endif()
latobs_configure_target(latobs_runtime)

install(TARGETS latobs_runtime EXPORT LatencyObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
