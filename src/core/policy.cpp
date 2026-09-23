// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/core/policy.hpp"

#include <string>

#include "latency_observatory/core/digest.hpp"

namespace latobs::core {
namespace {

[[nodiscard]] Status require(bool condition, std::string message) {
  if (!condition) {
    return Error(ErrorCode::InvalidArgument, std::move(message));
  }
  return ok_status();
}

void append_line(std::string& out, const char* key, std::uint64_t value) {
  out.append(key);
  out.push_back('=');
  out.append(std::to_string(value));
  out.push_back('\n');
}

void append_line_signed(std::string& out, const char* key, std::int64_t value) {
  out.append(key);
  out.push_back('=');
  out.append(std::to_string(value));
  out.push_back('\n');
}

void append_flag(std::string& out, const char* key, bool value) {
  out.append(key);
  out.push_back('=');
  out.append(value ? "true" : "false");
  out.push_back('\n');
}

}  // namespace

Status Limits::validate() const {
  LATOBS_TRY_STATUS(require(max_hops_per_path >= 1 && max_hops_per_path <= 4096,
                            "limits: max_hops_per_path must be within [1, 4096]"));
  LATOBS_TRY_STATUS(require(max_endpoints >= 1 && max_endpoints <= 1000000,
                            "limits: max_endpoints must be within [1, 1000000]"));
  LATOBS_TRY_STATUS(require(max_links >= 1 && max_links <= 1000000,
                            "limits: max_links must be within [1, 1000000]"));
  LATOBS_TRY_STATUS(require(max_queues >= 1 && max_queues <= 1000000,
                            "limits: max_queues must be within [1, 1000000]"));
  LATOBS_TRY_STATUS(require(max_paths >= 1 && max_paths <= 1000000,
                            "limits: max_paths must be within [1, 1000000]"));
  LATOBS_TRY_STATUS(require(max_sources >= 1 && max_sources <= 1000000,
                            "limits: max_sources must be within [1, 1000000]"));
  LATOBS_TRY_STATUS(require(max_generations >= 1 && max_generations <= 1000000,
                            "limits: max_generations must be within [1, 1000000]"));
  LATOBS_TRY_STATUS(require(max_clock_domains >= 2 && max_clock_domains <= 1000000,
                            "limits: max_clock_domains must be within [2, 1000000]"));
  LATOBS_TRY_STATUS(require(max_batch_samples >= 1 && max_batch_samples <= 1000000,
                            "limits: max_batch_samples must be within [1, 1000000]"));
  LATOBS_TRY_STATUS(require(max_samples_per_path >= max_batch_samples,
                            "limits: max_samples_per_path must be at least max_batch_samples"));
  LATOBS_TRY_STATUS(require(max_total_samples >= max_samples_per_path,
                            "limits: max_total_samples must be at least max_samples_per_path"));
  LATOBS_TRY_STATUS(require(max_baselines >= 1, "limits: max_baselines must be at least 1"));
  LATOBS_TRY_STATUS(require(max_export_records >= 1, "limits: max_export_records must be at least 1"));
  LATOBS_TRY_STATUS(require(max_history_buckets >= 1 && max_history_buckets <= 100000,
                            "limits: max_history_buckets must be within [1, 100000]"));
  LATOBS_TRY_STATUS(require(max_histogram_buckets >= 1 && max_histogram_buckets <= 4096,
                            "limits: max_histogram_buckets must be within [1, 4096]"));
  LATOBS_TRY_STATUS(require(max_quantile_probes >= 1 && max_quantile_probes <= 64,
                            "limits: max_quantile_probes must be within [1, 64]"));
  LATOBS_TRY_STATUS(require(max_json_depth >= 1 && max_json_depth <= 128,
                            "limits: max_json_depth must be within [1, 128]"));
  LATOBS_TRY_STATUS(require(max_json_bytes >= 1024, "limits: max_json_bytes must be at least 1024"));
  LATOBS_TRY_STATUS(require(max_transport_line_bytes >= 1024,
                            "limits: max_transport_line_bytes must be at least 1024"));
  LATOBS_TRY_STATUS(require(max_transport_line_bytes <= max_json_bytes,
                            "limits: a transport line must not exceed max_json_bytes"));
  LATOBS_TRY_STATUS(require(max_connections >= 1 && max_connections <= 1024,
                            "limits: max_connections must be within [1, 1024]"));
  LATOBS_TRY_STATUS(require(max_workers >= 1 && max_workers <= 256,
                            "limits: max_workers must be within [1, 256]"));
  LATOBS_TRY_STATUS(require(max_queue_depth >= 1 && max_queue_depth <= 1000000,
                            "limits: max_queue_depth must be within [1, 1000000]"));
  LATOBS_TRY_STATUS(require(max_pending_operations >= max_queue_depth,
                            "limits: max_pending_operations must be at least max_queue_depth"));
  LATOBS_TRY_STATUS(require(max_segment_records >= 1, "limits: max_segment_records must be at least 1"));
  LATOBS_TRY_STATUS(require(max_segment_bytes >= 4096, "limits: max_segment_bytes must be at least 4096"));
  LATOBS_TRY_STATUS(require(max_store_bytes >= max_segment_bytes,
                            "limits: max_store_bytes must be at least max_segment_bytes"));
  LATOBS_TRY_STATUS(require(sequence_replay_window >= 1,
                            "limits: sequence_replay_window must be at least 1"));
  LATOBS_TRY_STATUS(require(max_sessions_per_source >= 1 && max_sessions_per_source <= 4096,
                            "limits: max_sessions_per_source must be within [1, 4096]"));
  return ok_status();
}

std::string Limits::canonical_text() const {
  std::string out;
  append_line(out, "limits.max_hops_per_path", max_hops_per_path);
  append_line(out, "limits.max_endpoints", max_endpoints);
  append_line(out, "limits.max_links", max_links);
  append_line(out, "limits.max_queues", max_queues);
  append_line(out, "limits.max_paths", max_paths);
  append_line(out, "limits.max_sources", max_sources);
  append_line(out, "limits.max_generations", max_generations);
  append_line(out, "limits.max_clock_domains", max_clock_domains);
  append_line(out, "limits.max_batch_samples", max_batch_samples);
  append_line(out, "limits.max_samples_per_path", max_samples_per_path);
  append_line(out, "limits.max_total_samples", max_total_samples);
  append_line(out, "limits.max_baselines", max_baselines);
  append_line(out, "limits.max_export_records", max_export_records);
  append_line(out, "limits.max_history_buckets", max_history_buckets);
  append_line(out, "limits.max_histogram_buckets", max_histogram_buckets);
  append_line(out, "limits.max_quantile_probes", max_quantile_probes);
  append_line(out, "limits.max_json_depth", max_json_depth);
  append_line(out, "limits.max_json_bytes", max_json_bytes);
  append_line(out, "limits.max_transport_line_bytes", max_transport_line_bytes);
  append_line(out, "limits.max_connections", max_connections);
  append_line(out, "limits.max_workers", max_workers);
  append_line(out, "limits.max_queue_depth", max_queue_depth);
  append_line(out, "limits.max_pending_operations", max_pending_operations);
  append_line(out, "limits.max_segment_records", max_segment_records);
  append_line(out, "limits.max_segment_bytes", max_segment_bytes);
  append_line(out, "limits.max_store_bytes", max_store_bytes);
  append_line(out, "limits.sequence_replay_window", sequence_replay_window);
  append_line(out, "limits.max_sessions_per_source", max_sessions_per_source);
  return out;
}

Status FreshnessPolicy::validate() const {
  LATOBS_TRY_STATUS(require(fresh_horizon_ns > 0, "freshness: fresh_horizon_ns must be positive"));
  LATOBS_TRY_STATUS(require(aging_horizon_ns > fresh_horizon_ns,
                            "freshness: aging_horizon_ns must exceed fresh_horizon_ns"));
  LATOBS_TRY_STATUS(require(stale_horizon_ns > aging_horizon_ns,
                            "freshness: stale_horizon_ns must exceed aging_horizon_ns"));
  return ok_status();
}

Freshness FreshnessPolicy::classify(Nanos age_ns) const {
  if (age_ns < 0) {
    // Evidence observed in the future relative to the receiver is not fresh:
    // it cannot be classified without resolving the clock relationship.
    return Freshness::Unknown;
  }
  if (age_ns <= fresh_horizon_ns) return Freshness::Fresh;
  if (age_ns <= aging_horizon_ns) return Freshness::Aging;
  if (age_ns <= stale_horizon_ns) return Freshness::Stale;
  return Freshness::Expired;
}

std::string FreshnessPolicy::canonical_text() const {
  std::string out;
  append_line_signed(out, "freshness.fresh_horizon_ns", fresh_horizon_ns);
  append_line_signed(out, "freshness.aging_horizon_ns", aging_horizon_ns);
  append_line_signed(out, "freshness.stale_horizon_ns", stale_horizon_ns);
  return out;
}

Status ComparabilityPolicy::validate() const {
  LATOBS_TRY_STATUS(require(max_uncertainty_ns >= 0,
                            "comparability: max_uncertainty_ns must not be negative"));
  LATOBS_TRY_STATUS(require(!allow_transitive,
                            "comparability: transitive clock chains are not supported by design"));
  return ok_status();
}

std::string ComparabilityPolicy::canonical_text() const {
  std::string out;
  append_line_signed(out, "comparability.max_uncertainty_ns", max_uncertainty_ns);
  append_flag(out, "comparability.allow_holdover", allow_holdover);
  append_flag(out, "comparability.require_generation_match", require_generation_match);
  append_flag(out, "comparability.require_epoch_match", require_epoch_match);
  append_flag(out, "comparability.require_incarnation_match", require_incarnation_match);
  append_flag(out, "comparability.allow_transitive", allow_transitive);
  return out;
}

Status AttributionPolicy::validate() const {
  LATOBS_TRY_STATUS(require(residual_tolerance_ns >= 0,
                            "attribution: residual_tolerance_ns must not be negative"));
  LATOBS_TRY_STATUS(require(min_samples_for_trend >= 1,
                            "attribution: min_samples_for_trend must be at least 1"));
  return ok_status();
}

std::string AttributionPolicy::canonical_text() const {
  std::string out;
  append_line_signed(out, "attribution.residual_tolerance_ns", residual_tolerance_ns);
  append_flag(out, "attribution.allow_partial_decomposition", allow_partial_decomposition);
  append_flag(out, "attribution.allow_degraded_confidence", allow_degraded_confidence);
  append_line(out, "attribution.min_samples_for_trend", min_samples_for_trend);
  return out;
}

Status AnomalyPolicy::validate() const {
  LATOBS_TRY_STATUS(require(watch_delta_ns >= 0, "anomaly: watch_delta_ns must not be negative"));
  LATOBS_TRY_STATUS(require(elevated_delta_ns >= watch_delta_ns,
                            "anomaly: elevated_delta_ns must be at least watch_delta_ns"));
  LATOBS_TRY_STATUS(require(min_samples >= 1, "anomaly: min_samples must be at least 1"));
  return ok_status();
}

std::string AnomalyPolicy::canonical_text() const {
  std::string out;
  append_line_signed(out, "anomaly.watch_delta_ns", watch_delta_ns);
  append_line_signed(out, "anomaly.elevated_delta_ns", elevated_delta_ns);
  append_line(out, "anomaly.min_samples", min_samples);
  return out;
}

Status RuntimePolicy::validate() const {
  LATOBS_TRY_STATUS(limits.validate());
  LATOBS_TRY_STATUS(freshness.validate());
  LATOBS_TRY_STATUS(comparability.validate());
  LATOBS_TRY_STATUS(attribution.validate());
  LATOBS_TRY_STATUS(anomaly.validate());
  LATOBS_TRY_STATUS(require(limits.max_quantile_probes <= 64,
                            "policy: quantile probe count exceeds the supported maximum"));
  return ok_status();
}

std::string RuntimePolicy::canonical_text() const {
  std::string out;
  out.append("policy.version=1\n");
  out.append(limits.canonical_text());
  out.append(freshness.canonical_text());
  out.append(comparability.canonical_text());
  out.append(attribution.canonical_text());
  out.append(anomaly.canonical_text());
  return out;
}

std::string RuntimePolicy::digest() const { return sha256_hex(canonical_text()); }

RuntimePolicy default_policy() {
  RuntimePolicy policy;
  LATOBS_ASSERT_MSG(policy.validate().ok(), "default policy must be valid");
  return policy;
}

}  // namespace latobs::core
