// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/model/clock.hpp"
#include "latency_observatory/model/entities.hpp"
#include "latency_observatory/model/measurement.hpp"
#include "latency_observatory/stats/distribution.hpp"

namespace latobs::stats {

/// Current aggregation only uses evidence that is usable now; historical
/// aggregation reports everything that matches the window while still stating
/// the freshness of each contribution. History is never relabelled as current.
enum class AggregationMode : std::uint8_t { Current, Historical };
std::string_view to_string(AggregationMode mode) noexcept;

/// A half open interval [from, to) in one clock domain.
struct TimeWindow {
  Timestamp from;
  Timestamp to;

  [[nodiscard]] static Result<TimeWindow> make(const Timestamp& from, const Timestamp& to);
  [[nodiscard]] bool contains(const Timestamp& point) const noexcept {
    return point.domain == from.domain && point.ns >= from.ns && point.ns < to.ns;
  }
  [[nodiscard]] bool valid() const noexcept {
    return from.valid() && to.valid() && from.domain == to.domain && from.ns < to.ns;
  }
};

struct HopSummary {
  HopIndex index;
  HopId hop;
  std::uint64_t exchanges_included = 0;
  std::uint64_t dwells_observed = 0;
  std::uint64_t dwells_missing = 0;      // the hop was not reported: unknown, not zero
  std::uint64_t dwells_unsupported = 0;  // reported without a usable dwell
  std::uint64_t queue_dwells_observed = 0;
  std::uint64_t queue_dwells_unsupported = 0;
  std::uint64_t coverage_ppm = 0;
  Distribution dwell;
  Distribution queue_dwell;
  core::Evidence evidence;
};

struct PathSummary {
  PathId path;
  GenerationId generation;
  ClockDomainId domain;
  AggregationMode mode = AggregationMode::Current;
  TimeWindow window;
  Timestamp as_of;
  std::uint64_t exchanges_considered = 0;
  std::uint64_t exchanges_included = 0;
  std::uint64_t excluded_other_path = 0;
  std::uint64_t excluded_other_generation = 0;
  std::uint64_t excluded_out_of_window = 0;
  std::uint64_t excluded_stale = 0;
  std::uint64_t excluded_conflicting = 0;
  std::uint64_t excluded_unsupported = 0;
  std::uint64_t excluded_clock_domain = 0;
  Distribution end_to_end;
  std::vector<HopSummary> hops;
  /// The sources that contributed at least one included exchange, ordered by
  /// identity. Attribution uses them for the declared semantics gate.
  std::vector<SourceId> contributing_sources;
  core::Evidence evidence;
  std::string policy_digest;

  /// True when the window covers no exchange at all.
  [[nodiscard]] bool empty() const noexcept { return exchanges_included == 0; }
};

/// Why a record did or did not contribute to an aggregate. One definition is
/// used by summaries, history and attribution so the three can never disagree.
enum class RecordClass : std::uint8_t {
  Included = 0,
  OtherPath,
  OtherGeneration,
  OtherSource,
  ClockDomainMismatch,
  OutOfWindow,
  Stale,
  Conflicting,
  Unsupported,
};
std::string_view to_string(RecordClass value) noexcept;

[[nodiscard]] RecordClass classify_record(const model::MeasurementRecord& record, PathId path,
                                          GenerationId generation, AggregationMode mode,
                                          const TimeWindow& window, SourceId restrict_to_source);

struct SummaryRequest {
  PathId path;
  GenerationId generation;
  AggregationMode mode = AggregationMode::Current;
  TimeWindow window;
  HistogramSpec histogram = HistogramSpec::latency_default();
  std::vector<QuantileProbe> probes = default_quantile_probes();
  /// When set, only records from this source contribute.
  SourceId restrict_to_source;
};

/// Deterministic aggregation of stored records for one path and generation.
/// The input order does not affect the result: the distributions are computed
/// from sorted values and the counters are order independent.
[[nodiscard]] Result<PathSummary> summarize(
    const std::vector<const model::MeasurementRecord*>& records, const model::Catalog& catalog,
    const SummaryRequest& request, const core::RuntimePolicy& policy);

void write_json(core::JsonWriter& writer, const HopSummary& summary);
void write_json(core::JsonWriter& writer, const PathSummary& summary);

}  // namespace latobs::stats
