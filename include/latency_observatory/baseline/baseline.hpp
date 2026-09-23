// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/baseline/vocabulary.hpp"
#include "latency_observatory/core/json.hpp"
#include "latency_observatory/core/policy.hpp"
#include "latency_observatory/stats/summary.hpp"

namespace latobs::baseline {

/// A generation bound reference distribution. A baseline is never applied to a
/// different generation, path, clock domain or histogram bucketing: every such
/// condition is reported as an explicit mismatch instead of a silent
/// comparison.
struct Baseline {
  BaselineId id;
  Name name;
  PathId path;
  GenerationId generation;
  ClockDomainId domain;
  Revision revision;
  stats::HistogramSpec histogram;
  std::vector<stats::QuantileProbe> probes;
  SourceId source;  // provenance: which source produced the referenced evidence
  bool synthetic = false;
  Timestamp created_at;
  stats::TimeWindow window;
  std::uint64_t exchange_count = 0;
  stats::Distribution end_to_end;
  std::vector<stats::HopSummary> hops;
  core::Evidence evidence;
  std::string policy_digest;
};

enum class MismatchKind : std::uint8_t {
  None = 0,
  Missing,
  Path,
  Generation,
  ClockDomain,
  Histogram,
  Revision,
  Probes,
  Stale,
  Expired,
  NotUsable,
};
std::string_view to_string(MismatchKind kind) noexcept;

struct BaselineMismatch {
  MismatchKind kind = MismatchKind::None;
  std::string detail;
};

/// Anomaly classification: a statistical statement about observed values
/// relative to a generation bound baseline. It is never a causal claim.
enum class AnomalyClass : std::uint8_t {
  None,
  Watch,
  Elevated,
  Suppressed,
  SuppressedElevated,
  Unknown,
};
std::string_view to_string(AnomalyClass value) noexcept;

struct AnomalyEvidence {
  HopIndex index;  // invalid for the end to end statement
  HopId hop;      // invalid for the end to end statement
  AnomalyClass classification = AnomalyClass::Unknown;
  std::optional<Nanos> mean_delta_ns;
  std::optional<Nanos> p99_delta_ns;
  std::uint64_t ratio_ppm = 0;
  std::uint64_t current_count = 0;
  std::uint64_t baseline_count = 0;
  core::Evidence evidence;
};

struct BaselineComparison {
  BaselineId baseline;
  bool applicable = false;
  std::vector<BaselineMismatch> mismatches;
  std::optional<Nanos> mean_delta_ns;
  std::optional<Nanos> min_delta_ns;
  std::optional<Nanos> max_delta_ns;
  std::uint64_t mean_ratio_ppm = 0;
  std::vector<AnomalyEvidence> anomalies;
  core::Evidence evidence;
};

/// Compares a current summary against a baseline. Every incompatibility is
/// reported with its own mismatch kind; when any mismatch is present no delta
/// is produced at all.
[[nodiscard]] BaselineComparison compare(const Baseline& reference,
                                         const stats::PathSummary& current,
                                         const core::RuntimePolicy& policy);

/// Builds an anomaly statement for one pair of distributions.
[[nodiscard]] AnomalyEvidence classify_anomaly(const stats::Distribution& current,
                                               const stats::Distribution& reference,
                                               HopIndex index, HopId hop,
                                               const core::RuntimePolicy& policy);

/// The baseline registry. Baselines are keyed by identity and looked up by
/// (path, generation, clock domain, histogram); a lookup that only finds
/// baselines for another generation fails explicitly.
class BaselineStore {
 public:
  BaselineStore(const core::Limits& limits, const core::RuntimePolicy& policy)
      : limits_(limits), policy_(policy) {}

  [[nodiscard]] Result<BaselineId> add(Baseline baseline);
  [[nodiscard]] Result<const Baseline*> get(BaselineId id) const;
  /// Most recent revision for the exact key, or an explicit mismatch error.
  [[nodiscard]] Result<const Baseline*> latest_for(PathId path, GenerationId generation,
                                                   ClockDomainId domain,
                                                   const stats::HistogramSpec& histogram) const;
  [[nodiscard]] std::vector<const Baseline*> list() const;
  [[nodiscard]] std::size_t size() const noexcept { return baselines_.size(); }
  [[nodiscard]] std::uint64_t generation_mismatch_refusals() const noexcept {
    return generation_mismatch_refusals_;
  }

 private:
  core::Limits limits_;
  core::RuntimePolicy policy_;
  std::map<BaselineId, Baseline> baselines_;
  std::map<PathId, std::vector<BaselineId>> by_path_;
  std::uint64_t generation_mismatch_refusals_ = 0;
};

void write_json(core::JsonWriter& writer, const AnomalyEvidence& evidence);
void write_json(core::JsonWriter& writer, const BaselineComparison& comparison);
void write_json(core::JsonWriter& writer, const Baseline& baseline);

}  // namespace latobs::baseline
