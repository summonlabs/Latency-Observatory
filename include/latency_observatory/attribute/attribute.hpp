// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/attribute/vocabulary.hpp"
#include "latency_observatory/baseline/baseline.hpp"
#include "latency_observatory/core/json.hpp"
#include "latency_observatory/core/policy.hpp"
#include "latency_observatory/model/clock.hpp"
#include "latency_observatory/model/entities.hpp"
#include "latency_observatory/model/measurement.hpp"
#include "latency_observatory/stats/summary.hpp"

namespace latobs::attribute {

/// What kind of statement was produced. A decomposition is only produced when
/// the clocks are comparable, the hops tile the exchange, and the sources
/// declare the semantics. Otherwise the result says exactly why not.
enum class AttributionKind : std::uint8_t {
  ObservedDecomposition,
  PartialDecomposition,
  Refused,
  Unsupported,
};
std::string_view to_string(AttributionKind kind) noexcept;

/// The observed share of one hop. A share is a statement about measured time,
/// never a statement about cause.
struct HopContribution {
  HopIndex index;
  HopId hop;
  std::optional<Nanos> mean_dwell_ns;
  std::optional<Nanos> sum_dwell_ns;
  std::optional<Nanos> mean_queue_dwell_ns;
  std::optional<Nanos> uncertainty_ns;
  std::uint64_t share_ppm = 0;
  std::uint64_t coverage_ppm = 0;
  core::Evidence evidence;
};

struct AttributionRequest {
  PathId path;
  GenerationId generation;
  stats::AggregationMode mode = stats::AggregationMode::Current;
  stats::TimeWindow window;
  stats::HistogramSpec histogram = stats::HistogramSpec::latency_default();
  std::vector<stats::QuantileProbe> probes = stats::default_quantile_probes();
  BaselineId baseline;  // invalid when no baseline comparison was requested
};

struct AttributionResult {
  PathId path;
  GenerationId generation;
  ClockDomainId domain;
  AttributionKind kind = AttributionKind::Refused;
  stats::TimeWindow window;
  Timestamp as_of;
  std::uint64_t exchanges_considered = 0;
  std::uint64_t exchanges_included = 0;
  std::uint64_t exchanges_complete = 0;
  std::uint64_t exchanges_incomplete = 0;
  std::uint64_t negative_residuals = 0;
  std::optional<Nanos> end_to_end_mean_ns;
  std::optional<Nanos> accounted_mean_ns;
  std::optional<Nanos> unaccounted_mean_ns;
  std::optional<Nanos> residual_min_ns;
  std::optional<Nanos> residual_max_ns;
  std::optional<Nanos> residual_mean_ns;
  std::optional<Nanos> uncertainty_ns;
  std::vector<HopContribution> contributions;
  core::Evidence evidence;
  std::optional<baseline::BaselineComparison> comparison;
  std::string policy_digest;
};

/// Decomposes observed end to end latency into per hop contributions.
///
/// The algorithm is deliberately conservative:
///   * only exchanges whose declared hops were all observed with a dwell are
///     decomposed; the rest are counted as incomplete;
///   * every participating clock domain must be comparable with the exchange
///     domain, otherwise nothing is produced;
///   * the residual (end to end minus the sum of hop dwells) is always reported,
///     including when it is negative;
///   * no statement about cause is ever produced.
[[nodiscard]] Result<AttributionResult> attribute(
    const std::vector<const model::MeasurementRecord*>& records, const model::Catalog& catalog,
    const model::ClockRegistry& clocks, const baseline::BaselineStore* baselines,
    const AttributionRequest& request, const core::RuntimePolicy& policy);

void write_json(core::JsonWriter& writer, const HopContribution& contribution);
void write_json(core::JsonWriter& writer, const AttributionResult& result);

}  // namespace latobs::attribute
