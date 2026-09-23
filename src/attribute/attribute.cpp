// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/attribute/attribute.hpp"

#include <algorithm>
#include <utility>

#include "latency_observatory/core/checked.hpp"

namespace latobs::attribute {
namespace {

constexpr std::pair<AttributionKind, std::string_view> kKinds[] = {
    {AttributionKind::ObservedDecomposition, "observed_decomposition"},
    {AttributionKind::PartialDecomposition, "partial_decomposition"},
    {AttributionKind::Refused, "refused"},
    {AttributionKind::Unsupported, "unsupported"},
};

constexpr std::uint64_t kPpmScale = 1000000ULL;

[[nodiscard]] std::uint64_t ratio_ppm(std::uint64_t numerator, std::uint64_t denominator) noexcept {
  if (denominator == 0) return 0;
  const core::UInt128 product = core::mul_u64_to_u128(numerator, kPpmScale);
  std::uint64_t remainder = 0;
  const core::UInt128 quotient = core::divmod_u128_u64(product, denominator, remainder);
  if (!quotient.fits_u64()) return kPpmScale;
  return quotient.lo > kPpmScale ? kPpmScale : quotient.lo;
}

/// True when the evidence explains that a comparison across clock domains was
/// impossible, refused or contradictory.
[[nodiscard]] bool clock_refusal_reason(const core::Evidence& evidence) noexcept {
  for (const core::Reason& reason : evidence.reasons()) {
    switch (reason.code) {
      case core::ReasonCode::IncomparableClocks:
      case core::ReasonCode::ClockOffsetUnavailable:
      case core::ReasonCode::ClockUnsynced:
      case core::ReasonCode::ClockSyncExpired:
      case core::ReasonCode::ClockUncertaintyExceeded:
      case core::ReasonCode::ClockReferenceMismatch:
      case core::ReasonCode::ClockDomainMissing:
        return true;
      default:
        break;
    }
  }
  return false;
}

[[nodiscard]] bool absolute_at_most(const Nanos value, const Nanos bound) noexcept {
  if (value >= 0) return value <= bound;
  if (value == std::numeric_limits<Nanos>::min()) return false;
  return -value <= bound;
}

}  // namespace

std::string_view to_string(AttributionKind kind) noexcept {
  for (const auto& entry : kKinds) {
    if (entry.first == kind) return entry.second;
  }
  return "refused";
}

Result<AttributionResult> attribute(const std::vector<const model::MeasurementRecord*>& records,
                                    const model::Catalog& catalog,
                                    const model::ClockRegistry& clocks,
                                    const baseline::BaselineStore* baselines,
                                    const AttributionRequest& request,
                                    const core::RuntimePolicy& policy) {
  if (!request.window.valid()) {
    return Error(ErrorCode::InvalidArgument, "attribution requires a valid time window");
  }
  if (!request.generation.valid()) {
    return Error(ErrorCode::InvalidArgument, "attribution requires an explicit generation");
  }
  LATOBS_TRY(path_definition, catalog.path(request.path));

  AttributionResult result;
  result.path = request.path;
  result.generation = request.generation;
  result.domain = request.window.from.domain;
  result.window = request.window;
  result.as_of = request.window.to;
  result.policy_digest = policy.digest();
  result.evidence.add_reason(core::ReasonCode::NoCausalInference);

  const std::size_t hop_count = path_definition->hops.size();
  std::vector<Nanos> end_to_end_values;
  std::vector<std::vector<Nanos>> hop_values(hop_count);
  std::vector<std::vector<Nanos>> queue_values(hop_count);
  std::vector<std::uint64_t> hop_observed(hop_count, 0);
  std::vector<std::uint64_t> hop_missing(hop_count, 0);
  std::vector<std::optional<Nanos>> hop_uncertainty(hop_count);
  std::vector<Nanos> residual_values;
  std::vector<SourceId> contributing_sources;
  bool clocks_refused = false;
  core::Evidence refusal_evidence;

  for (const model::MeasurementRecord* record : records) {
    if (record == nullptr) continue;
    if (record->path != request.path) continue;
    ++result.exchanges_considered;
    if (record->generation != request.generation) continue;
    if (record->stamp.received_at.domain != request.window.from.domain) continue;
    if (!request.window.contains(record->stamp.received_at)) continue;
    const core::EvidenceState state = record->evidence.state();
    if (state == core::EvidenceState::Conflicting || state == core::EvidenceState::Unsupported ||
        state == core::EvidenceState::Refused || state == core::EvidenceState::Unknown ||
        state == core::EvidenceState::Missing) {
      continue;
    }
    if (request.mode == stats::AggregationMode::Current && !record->evidence.usable()) continue;
    ++result.exchanges_included;
    if (std::find(contributing_sources.begin(), contributing_sources.end(), record->source) ==
        contributing_sources.end()) {
      contributing_sources.push_back(record->source);
      std::sort(contributing_sources.begin(), contributing_sources.end());
    }

    // Every participating domain must be comparable with the exchange domain:
    // otherwise the decomposition would add values on incompatible timelines.
    Nanos exchange_offset = 0;
    bool comparable = true;
    if (record->domain != core::reference_clock_domain()) {
      const Result<model::ClockComparability> exchange_comparability = clocks.compare(
          record->domain, core::reference_clock_domain(), record->stamp.received_at,
          record->generation, record->epoch, record->incarnation);
      if (!exchange_comparability.has_value() || !exchange_comparability.value().comparable) {
        comparable = false;
        clocks_refused = true;
        refusal_evidence = core::Evidence::merge(
            refusal_evidence,
            exchange_comparability.has_value()
                ? exchange_comparability.value().evidence
                : core::Evidence::refused(core::ReasonCode::IncomparableClocks,
                                          record->domain.to_hex()));
      } else {
        exchange_offset = exchange_comparability.value().offset_first_ns;
      }
    }

    std::vector<std::optional<Nanos>> hop_timeline(hop_count);
    std::vector<std::optional<Nanos>> queue_timeline(hop_count);
    bool complete = comparable;
    for (std::size_t index = 0; index < hop_count; ++index) {
      const Result<HopIndex> hop_index = HopIndex::from_value(static_cast<std::uint32_t>(index));
      if (!hop_index.has_value()) {
        complete = false;
        break;
      }
      const model::HopObservation* observation = record->hop_at(*hop_index);
      if (observation == nullptr || !observation->dwell_ns.has_value()) {
        ++hop_missing[index];
        complete = false;
        if (observation != nullptr) {
          // The hop exists but carries no usable dwell: its evidence explains
          // why (incomparable clocks, unsupported semantics, conflict).
          refusal_evidence = core::Evidence::merge(refusal_evidence, observation->evidence);
          if (observation->evidence.state() == core::EvidenceState::Refused ||
              observation->evidence.state() == core::EvidenceState::Conflicting ||
              clock_refusal_reason(observation->evidence)) {
            clocks_refused = true;
          }
        }
        continue;
      }
      const bool same_domain = observation->entry.domain == record->domain;
      if (!same_domain) {
        const Result<model::ClockComparability> hop_comparability = clocks.compare(
            observation->entry.domain, record->domain, record->stamp.received_at,
            record->generation, record->epoch, record->incarnation);
        if (!hop_comparability.has_value() || !hop_comparability.value().comparable) {
          comparable = false;
          complete = false;
          clocks_refused = true;
          refusal_evidence = core::Evidence::merge(
              refusal_evidence,
              hop_comparability.has_value()
                  ? hop_comparability.value().evidence
                  : core::Evidence::refused(core::ReasonCode::IncomparableClocks,
                                            observation->hop.to_hex()));
          continue;
        }
      }
      if (hop_uncertainty[index].has_value() && observation->dwell_uncertainty_ns.has_value()) {
        hop_uncertainty[index] = std::max(*hop_uncertainty[index],
                                          *observation->dwell_uncertainty_ns);
      } else if (observation->dwell_uncertainty_ns.has_value()) {
        hop_uncertainty[index] = observation->dwell_uncertainty_ns;
      }
      hop_timeline[index] = observation->dwell_ns;
      if (observation->queue_dwell_ns.has_value()) {
        queue_timeline[index] = observation->queue_dwell_ns;
      }
    }

    if (!complete) {
      ++result.exchanges_incomplete;
      continue;
    }

    const Nanos request_reference = record->request.ns - exchange_offset;
    const Nanos response_reference = record->response.ns - exchange_offset;
    const Nanos end_to_end = response_reference - request_reference;
    if (end_to_end < 0) {
      ++result.exchanges_incomplete;
      continue;
    }
    Nanos accounted = 0;
    bool overflow = false;
    for (std::size_t index = 0; index < hop_count; ++index) {
      const std::optional<Nanos> value = hop_timeline[index];
      if (!value.has_value()) {
        overflow = true;
        break;
      }
      const std::optional<Nanos> sum = core::checked_add_i64(accounted, *value);
      if (!sum.has_value()) {
        overflow = true;
        break;
      }
      accounted = *sum;
    }
    if (overflow) {
      ++result.exchanges_incomplete;
      result.evidence.add_reason(core::ReasonCode::ArithmeticOverflow);
      continue;
    }

    ++result.exchanges_complete;
    end_to_end_values.push_back(end_to_end);
    residual_values.push_back(end_to_end - accounted);
    if (end_to_end - accounted < 0) ++result.negative_residuals;
    for (std::size_t index = 0; index < hop_count; ++index) {
      hop_values[index].push_back(*hop_timeline[index]);
      ++hop_observed[index];
      if (queue_timeline[index].has_value()) {
        queue_values[index].push_back(*queue_timeline[index]);
      }
    }
  }

  // --- semantics gate ---
  bool hop_semantics_declared = false;
  for (const SourceId source_id : contributing_sources) {
    const Result<const model::SourceDescriptor*> source = catalog.source(source_id);
    if (!source) continue;
    if (model::has_semantics(source.value()->semantics, model::SemanticsProfile::HopDwell)) {
      hop_semantics_declared = true;
      break;
    }
  }

  auto finalize_distribution = [&](std::vector<Nanos> values) -> Result<stats::Distribution> {
    return stats::aggregate(std::move(values), request.histogram, request.probes, policy.limits);
  };

  LATOBS_TRY(end_to_end_distribution, finalize_distribution(end_to_end_values));
  LATOBS_TRY(residual_distribution, finalize_distribution(residual_values));

  result.end_to_end_mean_ns = end_to_end_distribution.mean_ns;
  result.residual_min_ns = residual_distribution.min_ns;
  result.residual_max_ns = residual_distribution.max_ns;
  result.residual_mean_ns = residual_distribution.mean_ns;
  result.contributions.reserve(hop_count);
  std::uint64_t accounted_sum = 0;
  bool accounted_overflow = false;
  std::vector<std::optional<Nanos>> hop_sums(hop_count);
  for (std::size_t index = 0; index < hop_count; ++index) {
    LATOBS_TRY(distribution, finalize_distribution(hop_values[index]));
    LATOBS_TRY(queue_distribution, finalize_distribution(queue_values[index]));
    const Result<HopIndex> hop_index = HopIndex::from_value(static_cast<std::uint32_t>(index));
    if (!hop_index.has_value()) {
      return Error(ErrorCode::OutOfRange, "hop index exceeds the supported range");
    }
    HopContribution contribution;
    contribution.index = *hop_index;
    contribution.hop = path_definition->hops[index];
    contribution.mean_dwell_ns = distribution.mean_ns;
    contribution.sum_dwell_ns = distribution.sum_ns;
    contribution.mean_queue_dwell_ns = queue_distribution.mean_ns;
    contribution.uncertainty_ns = hop_uncertainty[index];
    contribution.coverage_ppm = ratio_ppm(hop_observed[index],
                                          hop_observed[index] + hop_missing[index]);
    if (distribution.sum_ns.has_value() && *distribution.sum_ns >= 0) {
      const std::optional<std::uint64_t> sum =
          core::checked_add_u64(accounted_sum, static_cast<std::uint64_t>(*distribution.sum_ns));
      if (!sum.has_value()) {
        accounted_overflow = true;
      } else {
        accounted_sum = *sum;
      }
      hop_sums[index] = distribution.sum_ns;
    }
    contribution.evidence = core::Evidence::observed(core::Freshness::Fresh,
                                                     core::Confidence::High);
    contribution.evidence.add_reason(core::ReasonCode::NoCausalInference);
    if (hop_observed[index] == 0) {
      contribution.evidence = core::Evidence::merge(
          contribution.evidence,
          core::Evidence::unknown(core::ReasonCode::NoSamples, contribution.hop.to_hex()));
    } else if (hop_missing[index] != 0) {
      contribution.evidence = core::Evidence::merge(
          contribution.evidence,
          core::Evidence::incomplete(core::ReasonCode::PartialCoverage,
                                     contribution.hop.to_hex()));
    }
    result.contributions.push_back(std::move(contribution));
  }
  if (accounted_overflow) {
    result.evidence.add_reason(core::ReasonCode::ArithmeticOverflow);
  }
  // Shares are a statement about a completed decomposition. While the verdict is
  // still open they are provisionally zero, and they are only published when the
  // decomposition is actually produced below.
  for (HopContribution& contribution : result.contributions) {
    if (!contribution.sum_dwell_ns.has_value() || *contribution.sum_dwell_ns < 0 ||
        accounted_sum == 0) {
      continue;
    }
    contribution.share_ppm =
        ratio_ppm(static_cast<std::uint64_t>(*contribution.sum_dwell_ns), accounted_sum);
  }

  // --- verdict ---
  AttributionKind kind = AttributionKind::ObservedDecomposition;
  core::Evidence evidence = result.evidence;
  evidence.set_state(core::EvidenceState::Observed);
  evidence.set_freshness(core::Freshness::Fresh);
  evidence.set_confidence(core::Confidence::High);

  if (result.exchanges_included == 0) {
    kind = AttributionKind::Refused;
    evidence = core::Evidence::merge(
        evidence, core::Evidence::unknown(core::ReasonCode::NoSamples, "no included exchanges"));
  } else if (!hop_semantics_declared) {
    kind = AttributionKind::Unsupported;
    evidence = core::Evidence::merge(
        evidence, core::Evidence::unsupported(core::ReasonCode::SemanticsUnsupported,
                                              "no contributing source declares hop dwell semantics"));
  } else if (clocks_refused) {
    kind = AttributionKind::Refused;
    evidence = core::Evidence::merge(evidence, refusal_evidence);
    evidence.set_state(core::EvidenceState::Refused);
  } else if (result.exchanges_complete == 0) {
    kind = AttributionKind::Refused;
    evidence = core::Evidence::merge(evidence, refusal_evidence);
    evidence = core::Evidence::merge(
        evidence, core::Evidence::refused(core::ReasonCode::NonTilingCoverage,
                                          "no exchange was observed with every hop present"));
  } else if (result.exchanges_incomplete != 0) {
    evidence.add_reason(core::ReasonCode::PartialCoverage,
                        std::to_string(result.exchanges_incomplete));
    evidence.add_reason(core::ReasonCode::NonTilingCoverage);
    evidence.set_state(core::EvidenceState::Incomplete);
    evidence.set_confidence(core::Confidence::Low);
    if (!policy.attribution.allow_partial_decomposition) {
      kind = AttributionKind::Refused;
      evidence = core::Evidence::merge(
          evidence, core::Evidence::refused(core::ReasonCode::RefusedByPolicy,
                                            "incomplete hop coverage is refused by policy"));
    } else {
      kind = AttributionKind::PartialDecomposition;
    }
  }

  if (kind != AttributionKind::ObservedDecomposition &&
      kind != AttributionKind::PartialDecomposition) {
    // A refused or unsupported decomposition publishes no shares at all: a
    // share without a decomposition would be an attribution the semantics do
    // not permit.
    for (HopContribution& contribution : result.contributions) {
      contribution.share_ppm = 0;
    }
    accounted_sum = 0;
  }

  if (kind == AttributionKind::ObservedDecomposition || kind == AttributionKind::PartialDecomposition) {
    // The accounted sum is accumulated as an unsigned quantity so that the
    // additions themselves are checked; the mean is only published when it is
    // representable as a signed latency.
    if (accounted_sum != 0 && result.exchanges_complete != 0) {
      const std::uint64_t mean = accounted_sum / result.exchanges_complete;
      const std::uint64_t ceiling =
          static_cast<std::uint64_t>(std::numeric_limits<Nanos>::max());
      if (mean <= ceiling) {
        result.accounted_mean_ns = static_cast<Nanos>(mean);
      } else {
        evidence.add_reason(core::ReasonCode::ArithmeticOverflow,
                            "the accounted mean exceeds the representable range");
      }
    }
    if (result.end_to_end_mean_ns.has_value() && result.accounted_mean_ns.has_value()) {
      result.unaccounted_mean_ns =
          core::checked_sub_i64(*result.end_to_end_mean_ns, *result.accounted_mean_ns);
      if (result.unaccounted_mean_ns.has_value()) {
        if (absolute_at_most(*result.unaccounted_mean_ns, policy.attribution.residual_tolerance_ns)) {
          evidence.add_reason(core::ReasonCode::ResidualWithinTolerance,
                              std::to_string(*result.unaccounted_mean_ns));
        } else {
          evidence.add_reason(core::ReasonCode::UnaccountedResidual,
                              std::to_string(*result.unaccounted_mean_ns));
        }
      }
    }
    evidence.add_reason(kind == AttributionKind::ObservedDecomposition
                            ? core::ReasonCode::AttributionComplete
                            : core::ReasonCode::AttributionPartial);
  }
  if (result.negative_residuals != 0) {
    evidence.add_reason(core::ReasonCode::HopOverlap,
                        std::to_string(result.negative_residuals));
  }
  if (end_to_end_distribution.sum_overflowed) {
    // The decomposition can still be produced from the complete exchanges, but
    // the end to end aggregate is not representable and the explanation says so
    // instead of leaving a silently missing residual.
    evidence.add_reason(core::ReasonCode::ArithmeticOverflow,
                        "the end to end sum exceeds the representable range");
  }
  evidence.add_reason(core::ReasonCode::NoCausalInference);
  result.kind = kind;
  result.evidence = evidence;

  if (baselines != nullptr && request.baseline.valid()) {
    const Result<const baseline::Baseline*> reference = baselines->get(request.baseline);
    if (!reference.has_value()) {
      result.evidence.add_reason(core::ReasonCode::BaselineMissing,
                                 request.baseline.to_hex());
    } else {
      stats::SummaryRequest summary_request;
      summary_request.path = request.path;
      summary_request.generation = request.generation;
      summary_request.mode = request.mode;
      summary_request.window = request.window;
      summary_request.histogram = request.histogram;
      summary_request.probes = request.probes;
      LATOBS_TRY(summary, stats::summarize(records, catalog, summary_request, policy));
      result.comparison = baseline::compare(*reference.value(), summary, policy);
    }
  }
  return result;
}

void write_json(core::JsonWriter& writer, const HopContribution& contribution) {
  writer.begin_object();
  writer.field("index", static_cast<std::uint64_t>(contribution.index.value()));
  writer.field("hop", contribution.hop.to_hex());
  writer.field_optional_int("mean_dwell_ns", contribution.mean_dwell_ns);
  writer.field_optional_int("sum_dwell_ns", contribution.sum_dwell_ns);
  writer.field_optional_int("mean_queue_dwell_ns", contribution.mean_queue_dwell_ns);
  writer.field_optional_int("uncertainty_ns", contribution.uncertainty_ns);
  writer.field("share_ppm", contribution.share_ppm);
  writer.field("coverage_ppm", contribution.coverage_ppm);
  writer.field("evidence_state", core::to_string(contribution.evidence.state()));
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const AttributionResult& result) {
  writer.begin_object();
  writer.field("path", result.path.to_hex());
  writer.field("generation", result.generation.to_hex());
  writer.field("clock_domain", result.domain.to_hex());
  writer.field("kind", to_string(result.kind));
  writer.field("as_of_ns", result.as_of.ns);
  writer.field_object("window");
  writer.field("from_ns", result.window.from.ns);
  writer.field("to_ns", result.window.to.ns);
  writer.end_object();
  writer.field("exchanges_considered", result.exchanges_considered);
  writer.field("exchanges_included", result.exchanges_included);
  writer.field("exchanges_complete", result.exchanges_complete);
  writer.field("exchanges_incomplete", result.exchanges_incomplete);
  writer.field("negative_residuals", result.negative_residuals);
  writer.field_optional_int("end_to_end_mean_ns", result.end_to_end_mean_ns);
  writer.field_optional_int("accounted_mean_ns", result.accounted_mean_ns);
  writer.field_optional_int("unaccounted_mean_ns", result.unaccounted_mean_ns);
  writer.field_optional_int("residual_min_ns", result.residual_min_ns);
  writer.field_optional_int("residual_max_ns", result.residual_max_ns);
  writer.field_optional_int("residual_mean_ns", result.residual_mean_ns);
  writer.field_array("contributions");
  for (const HopContribution& contribution : result.contributions) {
    write_json(writer, contribution);
  }
  writer.end_array();
  writer.field("policy_digest", result.policy_digest);
  writer.field("evidence_state", core::to_string(result.evidence.state()));
  writer.field("confidence", core::to_string(result.evidence.confidence()));
  writer.field_array("reasons");
  for (const core::Reason& reason : result.evidence.reasons()) {
    writer.begin_object();
    writer.field("code", core::to_string(reason.code));
    writer.field("detail", reason.detail);
    writer.end_object();
  }
  writer.end_array();
  if (result.comparison.has_value()) {
    writer.key("baseline_comparison");
    baseline::write_json(writer, *result.comparison);
  } else {
    writer.field_null("baseline_comparison");
  }
  writer.end_object();
}

}  // namespace latobs::attribute
