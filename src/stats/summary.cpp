// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/stats/summary.hpp"

#include <algorithm>

#include "latency_observatory/core/checked.hpp"

namespace latobs::stats {
namespace {

constexpr std::uint64_t kPpmScale = 1000000ULL;

[[nodiscard]] std::uint64_t ratio_ppm(std::uint64_t numerator, std::uint64_t denominator) noexcept {
  if (denominator == 0) return 0;
  const core::UInt128 product = core::mul_u64_to_u128(numerator, kPpmScale);
  std::uint64_t remainder = 0;
  const core::UInt128 quotient = core::divmod_u128_u64(product, denominator, remainder);
  if (!quotient.fits_u64()) return kPpmScale;
  return quotient.lo > kPpmScale ? kPpmScale : quotient.lo;
}

}  // namespace

std::string_view to_string(AggregationMode mode) noexcept {
  switch (mode) {
    case AggregationMode::Current: return "current";
    case AggregationMode::Historical: return "historical";
  }
  return "unknown";
}

std::string_view to_string(RecordClass value) noexcept {
  switch (value) {
    case RecordClass::Included: return "included";
    case RecordClass::OtherPath: return "other_path";
    case RecordClass::OtherGeneration: return "other_generation";
    case RecordClass::OtherSource: return "other_source";
    case RecordClass::ClockDomainMismatch: return "clock_domain_mismatch";
    case RecordClass::OutOfWindow: return "out_of_window";
    case RecordClass::Stale: return "stale";
    case RecordClass::Conflicting: return "conflicting";
    case RecordClass::Unsupported: return "unsupported";
  }
  return "unsupported";
}

RecordClass classify_record(const model::MeasurementRecord& record, PathId path,
                            GenerationId generation, AggregationMode mode, const TimeWindow& window,
                            SourceId restrict_to_source) {
  if (record.path != path) return RecordClass::OtherPath;
  if (record.generation != generation) return RecordClass::OtherGeneration;
  if (restrict_to_source.valid() && record.source != restrict_to_source) {
    return RecordClass::OtherSource;
  }
  if (record.stamp.received_at.domain != window.from.domain) {
    return RecordClass::ClockDomainMismatch;
  }
  if (!window.contains(record.stamp.received_at)) return RecordClass::OutOfWindow;
  const core::EvidenceState state = record.evidence.state();
  if (state == core::EvidenceState::Conflicting) return RecordClass::Conflicting;
  if (state == core::EvidenceState::Unsupported || state == core::EvidenceState::Refused ||
      state == core::EvidenceState::Unknown || state == core::EvidenceState::Missing) {
    return RecordClass::Unsupported;
  }
  if (mode == AggregationMode::Current && !record.evidence.usable()) return RecordClass::Stale;
  return RecordClass::Included;
}

Result<TimeWindow> TimeWindow::make(const Timestamp& from, const Timestamp& to) {
  if (!from.valid() || !to.valid()) {
    return Error(ErrorCode::InvalidArgument, "time window bounds must carry a clock domain");
  }
  if (from.domain != to.domain) {
    return Error(ErrorCode::InvalidArgument,
                 "time window bounds must be expressed in the same clock domain");
  }
  if (from.ns >= to.ns) {
    return Error(ErrorCode::InvalidArgument, "time window must be a non-empty half open interval");
  }
  TimeWindow window;
  window.from = from;
  window.to = to;
  return window;
}

Result<PathSummary> summarize(const std::vector<const model::MeasurementRecord*>& records,
                              const model::Catalog& catalog, const SummaryRequest& request,
                              const core::RuntimePolicy& policy) {
  if (!request.window.valid()) {
    return Error(ErrorCode::InvalidArgument, "summary requires a valid time window");
  }
  LATOBS_TRY(path_definition, catalog.path(request.path));
  if (!request.generation.valid()) {
    return Error(ErrorCode::InvalidArgument, "summary requires an explicit generation");
  }

  PathSummary summary;
  summary.path = request.path;
  summary.generation = request.generation;
  summary.domain = request.window.from.domain;
  summary.mode = request.mode;
  summary.window = request.window;
  summary.as_of = request.window.to;
  summary.policy_digest = policy.digest();
  summary.end_to_end.histogram = Histogram::empty(request.histogram);

  // Hop summaries follow the declared path order: a hop the source never
  // reported is still present in the output, marked as missing.
  summary.hops.reserve(path_definition->hops.size());
  for (std::size_t index = 0; index < path_definition->hops.size(); ++index) {
    const Result<HopIndex> hop_index = HopIndex::from_value(static_cast<std::uint32_t>(index));
    if (!hop_index.has_value()) {
      return Error(ErrorCode::OutOfRange, "path hop index exceeds the supported range");
    }
    HopSummary hop;
    hop.index = *hop_index;
    hop.hop = path_definition->hops[index];
    hop.dwell.histogram = Histogram::empty(request.histogram);
    hop.queue_dwell.histogram = Histogram::empty(request.histogram);
    // Coverage neutral starting point: merging a missing or conflicting hop
    // observation then yields Incomplete or Conflicting rather than Unknown.
    hop.evidence = core::Evidence::observed(core::Freshness::Fresh, core::Confidence::High);
    summary.hops.push_back(std::move(hop));
  }

  std::vector<Nanos> end_to_end_values;
  std::vector<std::vector<Nanos>> hop_values(summary.hops.size());
  std::vector<std::vector<Nanos>> queue_values(summary.hops.size());
  core::Evidence evidence = core::Evidence::observed(core::Freshness::Fresh, core::Confidence::High);
  bool saw_any = false;

  for (const model::MeasurementRecord* record : records) {
    if (record == nullptr) continue;
    ++summary.exchanges_considered;
    switch (classify_record(*record, request.path, request.generation, request.mode,
                            request.window, request.restrict_to_source)) {
      case RecordClass::OtherPath:
      case RecordClass::OtherSource:
        ++summary.excluded_other_path;
        continue;
      case RecordClass::OtherGeneration:
        ++summary.excluded_other_generation;
        continue;
      case RecordClass::ClockDomainMismatch:
        ++summary.excluded_clock_domain;
        continue;
      case RecordClass::OutOfWindow:
        ++summary.excluded_out_of_window;
        continue;
      case RecordClass::Conflicting:
        ++summary.excluded_conflicting;
        continue;
      case RecordClass::Unsupported:
        ++summary.excluded_unsupported;
        continue;
      case RecordClass::Stale:
        ++summary.excluded_stale;
        continue;
      case RecordClass::Included:
        break;
    }

    ++summary.exchanges_included;
    saw_any = true;
    if (std::find(summary.contributing_sources.begin(), summary.contributing_sources.end(),
                  record->source) == summary.contributing_sources.end()) {
      summary.contributing_sources.push_back(record->source);
      std::sort(summary.contributing_sources.begin(), summary.contributing_sources.end());
    }
    evidence = core::Evidence::merge(evidence, record->evidence);
    if (record->evidence.has_value()) {
      end_to_end_values.push_back(record->rtt_ns);
    }

    for (std::size_t index = 0; index < summary.hops.size(); ++index) {
      HopSummary& hop = summary.hops[index];
      ++hop.exchanges_included;
      const model::HopObservation* observation = record->hop_at(hop.index);
      if (observation == nullptr) {
        ++hop.dwells_missing;
        hop.evidence = core::Evidence::merge(
            hop.evidence, core::Evidence::missing(core::ReasonCode::HopMissing,
                                                  hop.hop.to_hex()));
        continue;
      }
      if (observation->dwell_ns.has_value()) {
        ++hop.dwells_observed;
        hop_values[index].push_back(*observation->dwell_ns);
      } else {
        ++hop.dwells_unsupported;
        hop.evidence = core::Evidence::merge(hop.evidence, observation->evidence);
      }
      if (observation->queue_dwell_ns.has_value()) {
        ++hop.queue_dwells_observed;
        queue_values[index].push_back(*observation->queue_dwell_ns);
      } else if (observation->queue.valid()) {
        ++hop.queue_dwells_unsupported;
      }
      hop.evidence = core::Evidence::merge(hop.evidence, observation->evidence);
    }

    // A record may carry hops that the generation bound path does not declare.
    for (const model::HopObservation& observation : record->hops) {
      const bool declared =
          std::any_of(summary.hops.begin(), summary.hops.end(),
                      [&](const HopSummary& hop) { return hop.index == observation.index; });
      if (!declared) {
        evidence.add_reason(core::ReasonCode::HopConflicting, observation.hop.to_hex());
      }
    }
  }

  LATOBS_TRY(end_to_end, aggregate(std::move(end_to_end_values), request.histogram, request.probes,
                                   policy.limits));
  summary.end_to_end = std::move(end_to_end);

  for (std::size_t index = 0; index < summary.hops.size(); ++index) {
    HopSummary& hop = summary.hops[index];
    LATOBS_TRY(dwell, aggregate(std::move(hop_values[index]), request.histogram, request.probes,
                                policy.limits));
    hop.dwell = std::move(dwell);
    LATOBS_TRY(queue_dwell, aggregate(std::move(queue_values[index]), request.histogram,
                                      request.probes, policy.limits));
    hop.queue_dwell = std::move(queue_dwell);
    hop.coverage_ppm = ratio_ppm(hop.dwells_observed, hop.exchanges_included);
    if (hop.dwells_observed == 0) {
      // No dwell at all: unknown when nothing was included in the window, and
      // incomplete when exchanges were included but the hop never had a value.
      // Either way the specific reasons collected above are preserved.
      const core::Evidence note =
          hop.exchanges_included == 0
              ? core::Evidence::unknown(core::ReasonCode::NoSamples, hop.hop.to_hex())
              : core::Evidence::incomplete(core::ReasonCode::NoSamples, hop.hop.to_hex());
      hop.evidence = core::Evidence::merge(hop.evidence, note);
    } else if (hop.dwells_missing != 0 || hop.dwells_unsupported != 0) {
      hop.evidence.set_state(core::EvidenceState::Incomplete);
      hop.evidence.add_reason(core::ReasonCode::PartialCoverage, hop.hop.to_hex());
    }
  }

  if (!saw_any) {
    evidence = core::Evidence::merge(
        evidence, core::Evidence::unknown(core::ReasonCode::NoSamples, "no included exchanges"));
  }
  if (summary.excluded_stale != 0) {
    evidence.add_reason(core::ReasonCode::EvidenceStale,
                        std::to_string(summary.excluded_stale));
  }
  evidence.add_reason(core::ReasonCode::AggregateDeterministic);
  summary.evidence = evidence;
  return summary;
}

void write_json(core::JsonWriter& writer, const HopSummary& summary) {
  writer.begin_object();
  writer.field("index", static_cast<std::uint64_t>(summary.index.value()));
  writer.field("hop", summary.hop.to_hex());
  writer.field("exchanges_included", summary.exchanges_included);
  writer.field("dwells_observed", summary.dwells_observed);
  writer.field("dwells_missing", summary.dwells_missing);
  writer.field("dwells_unsupported", summary.dwells_unsupported);
  writer.field("queue_dwells_observed", summary.queue_dwells_observed);
  writer.field("queue_dwells_unsupported", summary.queue_dwells_unsupported);
  writer.field("coverage_ppm", summary.coverage_ppm);
  writer.key("dwell");
  write_json(writer, summary.dwell);
  writer.key("queue_dwell");
  write_json(writer, summary.queue_dwell);
  writer.field("evidence_state", core::to_string(summary.evidence.state()));
  writer.field("freshness", core::to_string(summary.evidence.freshness()));
  writer.field("confidence", core::to_string(summary.evidence.confidence()));
  writer.field_array("reasons");
  for (const core::Reason& reason : summary.evidence.reasons()) {
    writer.begin_object();
    writer.field("code", core::to_string(reason.code));
    writer.field("detail", reason.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const PathSummary& summary) {
  writer.begin_object();
  writer.field("path", summary.path.to_hex());
  writer.field("generation", summary.generation.to_hex());
  writer.field("clock_domain", summary.domain.to_hex());
  writer.field("mode", to_string(summary.mode));
  writer.field_object("window");
  writer.field("from_ns", summary.window.from.ns);
  writer.field("to_ns", summary.window.to.ns);
  writer.field("clock_domain", summary.window.from.domain.to_hex());
  writer.end_object();
  writer.field("as_of_ns", summary.as_of.ns);
  writer.field("exchanges_considered", summary.exchanges_considered);
  writer.field("exchanges_included", summary.exchanges_included);
  writer.field("excluded_other_path", summary.excluded_other_path);
  writer.field("excluded_other_generation", summary.excluded_other_generation);
  writer.field("excluded_out_of_window", summary.excluded_out_of_window);
  writer.field("excluded_stale", summary.excluded_stale);
  writer.field("excluded_conflicting", summary.excluded_conflicting);
  writer.field("excluded_unsupported", summary.excluded_unsupported);
  writer.field("excluded_clock_domain", summary.excluded_clock_domain);
  writer.key("end_to_end");
  write_json(writer, summary.end_to_end);
  writer.field_array("hops");
  for (const HopSummary& hop : summary.hops) write_json(writer, hop);
  writer.end_array();
  writer.field("evidence_state", core::to_string(summary.evidence.state()));
  writer.field("freshness", core::to_string(summary.evidence.freshness()));
  writer.field("confidence", core::to_string(summary.evidence.confidence()));
  writer.field("policy_digest", summary.policy_digest);
  writer.field_array("reasons");
  for (const core::Reason& reason : summary.evidence.reasons()) {
    writer.begin_object();
    writer.field("code", core::to_string(reason.code));
    writer.field("detail", reason.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

}  // namespace latobs::stats
