// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/baseline/baseline.hpp"

#include <algorithm>
#include <utility>

#include "latency_observatory/core/checked.hpp"
#include "latency_observatory/core/digest.hpp"
#include "latency_observatory/stats/codec.hpp"

namespace latobs::baseline {
namespace {

constexpr std::pair<MismatchKind, std::string_view> kMismatchKinds[] = {
    {MismatchKind::None, "none"},
    {MismatchKind::Missing, "missing"},
    {MismatchKind::Path, "path"},
    {MismatchKind::Generation, "generation"},
    {MismatchKind::ClockDomain, "clock_domain"},
    {MismatchKind::Histogram, "histogram"},
    {MismatchKind::Revision, "revision"},
    {MismatchKind::Probes, "probes"},
    {MismatchKind::Stale, "stale"},
    {MismatchKind::Expired, "expired"},
    {MismatchKind::NotUsable, "not_usable"},
};

constexpr std::pair<AnomalyClass, std::string_view> kAnomalyClasses[] = {
    {AnomalyClass::None, "none"},
    {AnomalyClass::Watch, "watch"},
    {AnomalyClass::Elevated, "elevated"},
    {AnomalyClass::Suppressed, "suppressed"},
    {AnomalyClass::SuppressedElevated, "suppressed_elevated"},
    {AnomalyClass::Unknown, "unknown"},
};

[[nodiscard]] core::ReasonCode mismatch_reason(MismatchKind kind) noexcept {
  switch (kind) {
    case MismatchKind::Missing: return core::ReasonCode::BaselineMissing;
    case MismatchKind::Path: return core::ReasonCode::BaselinePathMismatch;
    case MismatchKind::Generation: return core::ReasonCode::BaselineGenerationMismatch;
    case MismatchKind::ClockDomain: return core::ReasonCode::BaselineClockDomainMismatch;
    case MismatchKind::Histogram: return core::ReasonCode::BaselineHistogramMismatch;
    case MismatchKind::Revision: return core::ReasonCode::BaselineRevisionMismatch;
    case MismatchKind::Probes: return core::ReasonCode::BaselineHistogramMismatch;
    case MismatchKind::Stale: return core::ReasonCode::BaselineStale;
    case MismatchKind::Expired: return core::ReasonCode::BaselineExpired;
    case MismatchKind::NotUsable: return core::ReasonCode::InsufficientSamples;
    case MismatchKind::None: break;
  }
  return core::ReasonCode::BaselineMissing;
}

[[nodiscard]] std::optional<Nanos> delta(const std::optional<Nanos>& current,
                                         const std::optional<Nanos>& reference) {
  if (!current.has_value() || !reference.has_value()) return std::nullopt;
  return core::checked_sub_i64(*current, *reference);
}

[[nodiscard]] std::uint64_t ratio_ppm(const std::optional<Nanos>& current,
                                      const std::optional<Nanos>& reference) {
  if (!current.has_value() || !reference.has_value()) return 0;
  if (*reference <= 0) return 0;
  const std::uint64_t numerator = static_cast<std::uint64_t>(*current < 0 ? 0 : *current);
  const core::UInt128 product = core::mul_u64_to_u128(numerator, 1000000ULL);
  std::uint64_t remainder = 0;
  const core::UInt128 quotient =
      core::divmod_u128_u64(product, static_cast<std::uint64_t>(*reference), remainder);
  if (!quotient.fits_u64()) return 0;
  return quotient.lo;
}

[[nodiscard]] std::optional<Nanos> quantile_at(const stats::Distribution& distribution,
                                               std::uint32_t numerator,
                                               std::uint32_t denominator) {
  for (std::size_t index = 0; index < distribution.probes.size(); ++index) {
    if (distribution.probes[index].numerator() == numerator &&
        distribution.probes[index].denominator() == denominator) {
      if (index < distribution.quantiles.size()) return distribution.quantiles[index];
      return std::nullopt;
    }
  }
  if (!distribution.quantiles.empty()) return distribution.quantiles.back();
  return std::nullopt;
}

}  // namespace

std::string_view to_string(MismatchKind kind) noexcept {
  for (const auto& entry : kMismatchKinds) {
    if (entry.first == kind) return entry.second;
  }
  return "none";
}

std::string_view to_string(AnomalyClass value) noexcept {
  for (const auto& entry : kAnomalyClasses) {
    if (entry.first == value) return entry.second;
  }
  return "unknown";
}

AnomalyEvidence classify_anomaly(const stats::Distribution& current,
                                 const stats::Distribution& reference, HopIndex index, HopId hop,
                                 const core::RuntimePolicy& policy) {
  AnomalyEvidence evidence;
  evidence.index = index;
  evidence.hop = hop;
  evidence.current_count = current.count;
  evidence.baseline_count = reference.count;
  evidence.mean_delta_ns = delta(current.mean_ns, reference.mean_ns);
  evidence.p99_delta_ns = delta(quantile_at(current, 99, 100), quantile_at(reference, 99, 100));
  evidence.ratio_ppm = ratio_ppm(current.mean_ns, reference.mean_ns);

  // Every anomaly statement carries the explicit reminder that the runtime does
  // not infer causality from a deviation.
  evidence.evidence.add_reason(core::ReasonCode::NoCausalInference);

  if (current.count < policy.anomaly.min_samples || reference.count < policy.anomaly.min_samples) {
    evidence.classification = AnomalyClass::Unknown;
    evidence.evidence.set_state(core::EvidenceState::Incomplete);
    evidence.evidence.set_confidence(core::Confidence::Low);
    evidence.evidence.add_reason(core::ReasonCode::InsufficientSamples);
    return evidence;
  }
  if (!evidence.mean_delta_ns.has_value()) {
    evidence.classification = AnomalyClass::Unknown;
    evidence.evidence.set_state(core::EvidenceState::Unknown);
    evidence.evidence.add_reason(core::ReasonCode::BaselineAbsent);
    return evidence;
  }

  const Nanos value = *evidence.mean_delta_ns;
  if (value >= policy.anomaly.elevated_delta_ns) {
    evidence.classification = AnomalyClass::Elevated;
    evidence.evidence.add_reason(core::ReasonCode::DeviationElevated, std::to_string(value));
  } else if (value >= policy.anomaly.watch_delta_ns) {
    evidence.classification = AnomalyClass::Watch;
    evidence.evidence.add_reason(core::ReasonCode::DeviationWatch, std::to_string(value));
  } else if (value <= -policy.anomaly.elevated_delta_ns) {
    evidence.classification = AnomalyClass::SuppressedElevated;
    evidence.evidence.add_reason(core::ReasonCode::DeviationSuppressed, std::to_string(value));
  } else if (value <= -policy.anomaly.watch_delta_ns) {
    evidence.classification = AnomalyClass::Suppressed;
    evidence.evidence.add_reason(core::ReasonCode::DeviationSuppressed, std::to_string(value));
  } else {
    evidence.classification = AnomalyClass::None;
    evidence.evidence.add_reason(core::ReasonCode::DeviationNone, std::to_string(value));
  }
  evidence.evidence.set_state(core::EvidenceState::Observed);
  evidence.evidence.set_freshness(current.histogram.spec == reference.histogram.spec
                                      ? core::Freshness::Fresh
                                      : core::Freshness::Unknown);
  evidence.evidence.set_confidence(current.count >= 4 * policy.anomaly.min_samples
                                       ? core::Confidence::High
                                       : core::Confidence::Moderate);
  return evidence;
}

BaselineComparison compare(const Baseline& reference, const stats::PathSummary& current,
                           const core::RuntimePolicy& policy) {
  BaselineComparison comparison;
  comparison.baseline = reference.id;
  auto add_mismatch = [&](MismatchKind kind, std::string detail) {
    comparison.mismatches.push_back(BaselineMismatch{kind, std::move(detail)});
    comparison.evidence.set_state(core::EvidenceState::Unsupported);
    comparison.evidence.set_confidence(core::Confidence::None);
    comparison.evidence.add_reason(mismatch_reason(kind), comparison.mismatches.back().detail);
  };

  if (!reference.id.valid()) {
    add_mismatch(MismatchKind::Missing, "no baseline was supplied");
    comparison.evidence.add_reason(core::ReasonCode::BaselineMissing);
    return comparison;
  }
  if (reference.path != current.path) {
    add_mismatch(MismatchKind::Path, "baseline belongs to a different path");
  }
  if (reference.generation != current.generation) {
    add_mismatch(MismatchKind::Generation,
                 "baseline generation differs from the observed generation");
  }
  if (reference.domain != current.domain) {
    add_mismatch(MismatchKind::ClockDomain,
                 "baseline clock domain differs from the observed clock domain");
  }
  if (!(reference.histogram == current.end_to_end.histogram.spec)) {
    add_mismatch(MismatchKind::Histogram, "baseline histogram bucketing differs");
  }
  if (!(reference.probes == current.end_to_end.probes)) {
    add_mismatch(MismatchKind::Probes, "baseline quantile probes differ");
  }
  if (reference.evidence.state() == core::EvidenceState::Conflicting) {
    add_mismatch(MismatchKind::NotUsable, "baseline evidence is conflicting");
  }
  if (!current.evidence.usable() && current.mode == stats::AggregationMode::Current) {
    add_mismatch(MismatchKind::NotUsable, "current evidence is not usable");
  }

  const Nanos baseline_age = current.as_of.ns - reference.created_at.ns;
  if (baseline_age < 0) {
    add_mismatch(MismatchKind::NotUsable, "baseline was created after the observed window");
  } else {
    const core::Freshness freshness = policy.freshness.classify(baseline_age);
    if (freshness == core::Freshness::Stale) {
      add_mismatch(MismatchKind::Stale, std::to_string(baseline_age));
    } else if (freshness == core::Freshness::Expired) {
      add_mismatch(MismatchKind::Expired, std::to_string(baseline_age));
    }
  }
  if (current.end_to_end.count < policy.anomaly.min_samples) {
    add_mismatch(MismatchKind::NotUsable, "current window has too few samples");
  }

  if (!comparison.mismatches.empty()) {
    comparison.applicable = false;
    comparison.mean_ratio_ppm = 0;
    return comparison;
  }

  comparison.applicable = true;
  comparison.evidence.set_state(core::EvidenceState::Observed);
  comparison.evidence.set_freshness(core::Freshness::Fresh);
  comparison.evidence.set_confidence(core::Confidence::High);
  comparison.evidence.add_reason(core::ReasonCode::BaselineMatched);
  comparison.evidence.add_reason(core::ReasonCode::NoCausalInference);
  comparison.mean_delta_ns = delta(current.end_to_end.mean_ns, reference.end_to_end.mean_ns);
  comparison.min_delta_ns = delta(current.end_to_end.min_ns, reference.end_to_end.min_ns);
  comparison.max_delta_ns = delta(current.end_to_end.max_ns, reference.end_to_end.max_ns);
  comparison.mean_ratio_ppm = ratio_ppm(current.end_to_end.mean_ns, reference.end_to_end.mean_ns);

  comparison.anomalies.push_back(classify_anomaly(current.end_to_end, reference.end_to_end,
                                                  HopIndex{}, HopId{}, policy));
  for (const stats::HopSummary& hop : current.hops) {
    const auto reference_hop =
        std::find_if(reference.hops.begin(), reference.hops.end(),
                     [&](const stats::HopSummary& candidate) { return candidate.index == hop.index; });
    if (reference_hop == reference.hops.end()) continue;
    comparison.anomalies.push_back(
        classify_anomaly(hop.dwell, reference_hop->dwell, hop.index, hop.hop, policy));
  }
  return comparison;
}

Result<BaselineId> BaselineStore::add(Baseline baseline) {
  if (!baseline.path.valid() || !baseline.generation.valid() || !baseline.domain.valid()) {
    return Error(ErrorCode::InvalidArgument,
                 "a baseline requires a path, a generation and a clock domain");
  }
  if (!baseline.revision.valid()) {
    return Error(ErrorCode::InvalidArgument, "a baseline requires a revision of at least 1");
  }
  if (baseline.name.empty()) {
    return Error(ErrorCode::InvalidArgument, "a baseline requires a name");
  }
  if (!baseline.id.valid()) {
    std::string canonical = "baseline|";
    canonical.append(baseline.path.to_hex());
    canonical.push_back('|');
    canonical.append(baseline.generation.to_hex());
    canonical.push_back('|');
    canonical.append(baseline.domain.to_hex());
    canonical.push_back('|');
    canonical.append(std::to_string(baseline.revision.value()));
    canonical.push_back('|');
    canonical.append(baseline.name.str());
    baseline.id = BaselineId::derive_from(canonical);
  }
  auto existing = baselines_.find(baseline.id);
  if (existing != baselines_.end()) {
    return Error(ErrorCode::AlreadyExists, "a baseline with this identity already exists",
                 baseline.name.str());
  }
  if (baselines_.size() >= limits_.max_baselines) {
    return Error(ErrorCode::CapacityExceeded, "baseline registry is at capacity",
                 baseline.name.str());
  }
  baseline.evidence.add_reason(core::ReasonCode::AggregateDeterministic);
  const BaselineId id = baseline.id;
  by_path_[baseline.path].push_back(id);
  baselines_.emplace(id, std::move(baseline));
  return id;
}

Result<const Baseline*> BaselineStore::get(BaselineId id) const {
  const auto found = baselines_.find(id);
  if (found == baselines_.end()) {
    return Error(ErrorCode::NotFound, "unknown baseline", id.to_hex());
  }
  return &found->second;
}

Result<const Baseline*> BaselineStore::latest_for(PathId path, GenerationId generation,
                                                  ClockDomainId domain,
                                                  const stats::HistogramSpec& histogram) const {
  const auto found = by_path_.find(path);
  if (found == by_path_.end()) {
    return Error(ErrorCode::NotFound, "no baseline is registered for this path", path.to_hex());
  }
  const Baseline* best = nullptr;
  bool saw_other_generation = false;
  for (const BaselineId& id : found->second) {
    const auto entry = baselines_.find(id);
    if (entry == baselines_.end()) continue;
    if (entry->second.generation != generation) {
      saw_other_generation = true;
      continue;
    }
    if (entry->second.domain != domain || !(entry->second.histogram == histogram)) continue;
    if (best == nullptr || entry->second.revision.value() > best->revision.value()) {
      best = &entry->second;
    }
  }
  if (best == nullptr) {
    if (saw_other_generation) {
      return Error(ErrorCode::Conflict,
                   "a baseline exists for this path but not for the requested generation",
                   generation.to_hex());
    }
    return Error(ErrorCode::NotFound, "no applicable baseline for this path and generation",
                 path.to_hex());
  }
  return best;
}

std::vector<const Baseline*> BaselineStore::list() const {
  std::vector<const Baseline*> out;
  out.reserve(baselines_.size());
  for (const auto& entry : baselines_) out.push_back(&entry.second);
  return out;
}

void write_json(core::JsonWriter& writer, const AnomalyEvidence& evidence) {
  writer.begin_object();
  writer.field("hop_index", static_cast<std::uint64_t>(evidence.index.value()));
  writer.field("hop", evidence.hop.valid() ? evidence.hop.to_hex() : std::string("end_to_end"));
  writer.field("classification", to_string(evidence.classification));
  writer.field_optional_int("mean_delta_ns", evidence.mean_delta_ns);
  writer.field_optional_int("p99_delta_ns", evidence.p99_delta_ns);
  writer.field("ratio_ppm", evidence.ratio_ppm);
  writer.field("current_count", evidence.current_count);
  writer.field("baseline_count", evidence.baseline_count);
  writer.field("evidence_state", core::to_string(evidence.evidence.state()));
  writer.field_array("reasons");
  for (const core::Reason& reason : evidence.evidence.reasons()) {
    writer.begin_object();
    writer.field("code", core::to_string(reason.code));
    writer.field("detail", reason.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const BaselineComparison& comparison) {
  writer.begin_object();
  writer.field("baseline", comparison.baseline.to_hex());
  writer.field("applicable", comparison.applicable);
  writer.field_optional_int("mean_delta_ns", comparison.mean_delta_ns);
  writer.field_optional_int("min_delta_ns", comparison.min_delta_ns);
  writer.field_optional_int("max_delta_ns", comparison.max_delta_ns);
  writer.field("mean_ratio_ppm", comparison.mean_ratio_ppm);
  writer.field_array("mismatches");
  for (const BaselineMismatch& mismatch : comparison.mismatches) {
    writer.begin_object();
    writer.field("kind", to_string(mismatch.kind));
    writer.field("detail", mismatch.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.field_array("anomalies");
  for (const AnomalyEvidence& anomaly : comparison.anomalies) write_json(writer, anomaly);
  writer.end_array();
  writer.field("evidence_state", core::to_string(comparison.evidence.state()));
  writer.field_array("reasons");
  for (const core::Reason& reason : comparison.evidence.reasons()) {
    writer.begin_object();
    writer.field("code", core::to_string(reason.code));
    writer.field("detail", reason.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const Baseline& baseline) {
  writer.begin_object();
  writer.field("baseline", baseline.id.to_hex());
  writer.field("name", baseline.name.str());
  writer.field("path", baseline.path.to_hex());
  writer.field("generation", baseline.generation.to_hex());
  writer.field("clock_domain", baseline.domain.to_hex());
  writer.field("revision", static_cast<std::uint64_t>(baseline.revision.value()));
  writer.field("source", baseline.source.to_hex());
  writer.field("synthetic", baseline.synthetic);
  writer.field("created_at_ns", baseline.created_at.ns);
  writer.field("exchange_count", baseline.exchange_count);
  writer.field("policy_digest", baseline.policy_digest);
  writer.key("histogram");
  stats::write_json(writer, baseline.histogram);
  writer.field_array("probes");
  for (const stats::QuantileProbe& probe : baseline.probes) writer.value_string(probe.to_string());
  writer.end_array();
  writer.key("window");
  stats::write_json(writer, baseline.window);
  writer.key("end_to_end");
  stats::write_json(writer, baseline.end_to_end);
  writer.field_array("hops");
  for (const stats::HopSummary& hop : baseline.hops) stats::write_json(writer, hop);
  writer.end_array();
  writer.field("evidence_state", core::to_string(baseline.evidence.state()));
  writer.end_object();
}

}  // namespace latobs::baseline
