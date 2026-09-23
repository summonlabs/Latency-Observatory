// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include "latency_observatory/core/error.hpp"
#include "latency_observatory/core/evidence.hpp"
#include "latency_observatory/core/time.hpp"

namespace latobs::core {

/// Hard bounds. Every externally derived size is checked against these before
/// memory is reserved: unbounded growth is a defect, not a configuration.
struct Limits {
  std::size_t max_hops_per_path = 64;
  std::size_t max_endpoints = 4096;
  std::size_t max_links = 4096;
  std::size_t max_queues = 4096;
  std::size_t max_paths = 4096;
  std::size_t max_sources = 1024;
  std::size_t max_generations = 256;
  std::size_t max_clock_domains = 256;

  std::size_t max_batch_samples = 4096;
  std::size_t max_samples_per_path = 200000;
  std::size_t max_total_samples = 1000000;
  std::size_t max_baselines = 1024;
  std::size_t max_export_records = 100000;
  std::size_t max_history_buckets = 512;
  std::size_t max_histogram_buckets = 512;
  std::size_t max_quantile_probes = 16;

  std::size_t max_json_depth = 24;
  std::size_t max_json_bytes = 4u * 1024u * 1024u;
  std::size_t max_transport_line_bytes = 1u * 1024u * 1024u;
  std::size_t max_connections = 16;
  std::size_t max_workers = 8;
  std::size_t max_queue_depth = 1024;
  std::size_t max_pending_operations = 4096;

  std::size_t max_segment_records = 4096;
  std::uint64_t max_segment_bytes = 8ULL * 1024ULL * 1024ULL;
  std::uint64_t max_store_bytes = 256ULL * 1024ULL * 1024ULL;
  std::uint64_t sequence_replay_window = 4096;
  std::size_t max_sessions_per_source = 64;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string canonical_text() const;
};

/// Age classification thresholds. The classification is a pure function of the
/// age of the evidence and this policy: it never depends on wall clock drift.
struct FreshnessPolicy {
  Nanos fresh_horizon_ns = 2LL * 1000LL * 1000LL * 1000LL;
  Nanos aging_horizon_ns = 10LL * 1000LL * 1000LL * 1000LL;
  Nanos stale_horizon_ns = 60LL * 1000LL * 1000LL * 1000LL;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] Freshness classify(Nanos age_ns) const;
  [[nodiscard]] std::string canonical_text() const;
};

/// Conditions under which two readings from different clock domains may be
/// compared at all. Comparability is never assumed: it is decided here.
struct ComparabilityPolicy {
  Nanos max_uncertainty_ns = 1LL * 1000LL * 1000LL;
  bool allow_holdover = true;
  bool require_generation_match = true;
  bool require_epoch_match = true;
  bool require_incarnation_match = true;
  bool allow_transitive = false;  // never: chains of sync are not evidence

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string canonical_text() const;
};

/// Attribution is only produced where the semantics permit it. These switches
/// decide whether a decomposition is produced, downgraded, or refused.
struct AttributionPolicy {
  Nanos residual_tolerance_ns = 1000;
  bool allow_partial_decomposition = false;
  bool allow_degraded_confidence = false;
  std::uint64_t min_samples_for_trend = 16;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string canonical_text() const;
};

/// Anomaly classification thresholds. An anomaly is a statistical statement
/// about observed values relative to a generation bound baseline: it is never a
/// causal claim.
struct AnomalyPolicy {
  Nanos watch_delta_ns = 1LL * 1000LL * 1000LL;
  Nanos elevated_delta_ns = 10LL * 1000LL * 1000LL;
  std::uint64_t min_samples = 8;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string canonical_text() const;
};

/// The complete deterministic policy of a runtime instance. The digest of the
/// canonical text is embedded in every explanation so that a result can be
/// reproduced against the policy that produced it.
struct RuntimePolicy {
  Limits limits{};
  FreshnessPolicy freshness{};
  ComparabilityPolicy comparability{};
  AttributionPolicy attribution{};
  AnomalyPolicy anomaly{};

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string canonical_text() const;
  [[nodiscard]] std::string digest() const;
};

[[nodiscard]] RuntimePolicy default_policy();

}  // namespace latobs::core
