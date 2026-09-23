// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/model/clock.hpp"

#include <utility>

#include "latency_observatory/core/checked.hpp"

namespace latobs::model {
namespace {

constexpr std::pair<ClockSyncState, std::string_view> kClockSyncStates[] = {
    {ClockSyncState::Unknown, "unknown"},
    {ClockSyncState::Unsynced, "unsynced"},
    {ClockSyncState::Synchronized, "synchronized"},
    {ClockSyncState::Holdover, "holdover"},
};

[[nodiscard]] ClockComparability incomparable(core::EvidenceState state, core::ReasonCode code,
                                              std::string detail) {
  ClockComparability result;
  result.comparable = false;
  result.uncertainty_ns = 0;
  result.confidence = core::Confidence::None;
  result.evidence = core::Evidence{}.set_state(state);
  result.evidence.add_reason(code, std::move(detail));
  result.evidence.add_reason(core::ReasonCode::IncomparableClocks);
  return result;
}

}  // namespace

std::string_view to_string(ClockSyncState state) noexcept {
  for (const auto& entry : kClockSyncStates) {
    if (entry.first == state) return entry.second;
  }
  return "unknown";
}

bool parse_clock_sync_state(std::string_view text, ClockSyncState& out) noexcept {
  for (const auto& entry : kClockSyncStates) {
    if (entry.second == text) {
      out = entry.first;
      return true;
    }
  }
  return false;
}

Result<ClockDomainId> ClockRegistry::define_domain(ClockDomainDef definition) {
  const core::Name name = definition.name;
  const ClockDomainId id = ClockDomainId::derive_from(name.view());
  if (definition.id.valid() && definition.id != id) {
    return core::Error(core::ErrorCode::Conflict,
                       "clock domain id does not match its canonical name", name.str());
  }
  definition.id = id;
  auto found = domains_.find(id);
  if (found == domains_.end()) {
    if (domains_.size() >= policy_.limits.max_clock_domains) {
      return core::Error(core::ErrorCode::CapacityExceeded, "clock domain registry is at capacity",
                         name.str());
    }
    domains_.emplace(id, std::move(definition));
    return id;
  }
  if (!(found->second == definition)) {
    return core::Error(core::ErrorCode::Conflict,
                       "clock domain is already defined with different content", name.str());
  }
  return id;
}

core::Status ClockRegistry::record_sync(ClockSync sync) {
  if (!sync.domain.valid()) {
    return core::Error(core::ErrorCode::InvalidArgument, "clock sync requires a domain");
  }
  if (domains_.find(sync.domain) == domains_.end()) {
    return core::Error(core::ErrorCode::NotFound, "clock sync references an unknown domain",
                       sync.domain.to_hex());
  }
  if (sync.reference.valid() && domains_.find(sync.reference) == domains_.end()) {
    return core::Error(core::ErrorCode::NotFound, "clock sync references an unknown reference domain",
                       sync.reference.to_hex());
  }
  if (sync.reference == sync.domain) {
    return core::Error(core::ErrorCode::InvalidArgument,
                       "a clock domain cannot be its own reference", sync.domain.to_hex());
  }
  if (sync.uncertainty_ns < 0) {
    return core::Error(core::ErrorCode::InvalidArgument,
                       "clock sync uncertainty must not be negative", sync.domain.to_hex());
  }
  if (sync.valid_for_ns <= 0) {
    return core::Error(core::ErrorCode::InvalidArgument,
                       "clock sync validity window must be positive", sync.domain.to_hex());
  }
  auto found = syncs_.find(sync.domain);
  if (found != syncs_.end()) {
    if (sync.revision < found->second.revision) {
      // A replay of an older synchronization report: the newer report stays in
      // force and the stale record is refused rather than applied.
      return core::Error(core::ErrorCode::Conflict,
                         "clock sync revision is older than the recorded revision",
                         sync.domain.to_hex());
    }
    if (sync.revision == found->second.revision) {
      if (sync == found->second) {
        return core::ok_status();  // idempotent re-report of identical content
      }
      if (sync.observed_at.ns <= found->second.observed_at.ns) {
        return core::Error(core::ErrorCode::Conflict,
                           "clock sync observation time moved backwards at the same revision",
                           sync.domain.to_hex());
      }
      return core::Error(core::ErrorCode::Conflict,
                         "clock sync content changed without a revision bump",
                         sync.domain.to_hex());
    }
  }
  syncs_[sync.domain] = std::move(sync);
  return core::ok_status();
}

const ClockDomainDef* ClockRegistry::domain(ClockDomainId id) const noexcept {
  const auto found = domains_.find(id);
  return found == domains_.end() ? nullptr : &found->second;
}

const ClockSync* ClockRegistry::sync(ClockDomainId id) const noexcept {
  const auto found = syncs_.find(id);
  return found == syncs_.end() ? nullptr : &found->second;
}

std::vector<const ClockDomainDef*> ClockRegistry::domains() const {
  std::vector<const ClockDomainDef*> out;
  out.reserve(domains_.size());
  for (const auto& entry : domains_) out.push_back(&entry.second);
  return out;
}

std::vector<const ClockSync*> ClockRegistry::syncs() const {
  std::vector<const ClockSync*> out;
  out.reserve(syncs_.size());
  for (const auto& entry : syncs_) out.push_back(&entry.second);
  return out;
}

Result<ClockComparability> ClockRegistry::compare_single(ClockDomainId id, const Timestamp& now,
                                                         GenerationId generation, EpochId epoch,
                                                         IncarnationId incarnation) const {
  const ClockDomainDef* definition = domain(id);
  if (definition == nullptr) {
    return incomparable(core::EvidenceState::Unknown, core::ReasonCode::ClockDomainMissing,
                        id.to_hex());
  }
  const ClockSync* report = sync(id);
  if (report == nullptr) {
    return incomparable(core::EvidenceState::Unknown, core::ReasonCode::ClockOffsetUnavailable,
                        definition->name.str());
  }
  if (report->state == ClockSyncState::Unknown || report->state == ClockSyncState::Unsynced) {
    return incomparable(core::EvidenceState::Refused, core::ReasonCode::ClockUnsynced,
                        definition->name.str());
  }
  if (report->state == ClockSyncState::Holdover && !policy_.comparability.allow_holdover) {
    return incomparable(core::EvidenceState::Refused, core::ReasonCode::ClockHoldover,
                        definition->name.str());
  }
  if (policy_.comparability.require_generation_match && report->generation.valid() &&
      generation.valid() && report->generation != generation) {
    return incomparable(core::EvidenceState::Conflicting,
                        core::ReasonCode::ClockSyncGenerationMismatch, definition->name.str());
  }
  if (policy_.comparability.require_epoch_match && report->epoch.valid() && epoch.valid() &&
      report->epoch != epoch) {
    return incomparable(core::EvidenceState::Conflicting, core::ReasonCode::ClockSyncEpochMismatch,
                        definition->name.str());
  }
  if (policy_.comparability.require_incarnation_match && report->incarnation.valid() &&
      incarnation.valid() && report->incarnation != incarnation) {
    return incomparable(core::EvidenceState::Conflicting,
                        core::ReasonCode::ClockSyncIncarnationMismatch, definition->name.str());
  }
  if (!report->observed_at.valid()) {
    return incomparable(core::EvidenceState::Unknown, core::ReasonCode::ClockOffsetUnavailable,
                        definition->name.str());
  }
  if (report->observed_at.domain != now.domain) {
    return incomparable(core::EvidenceState::Conflicting, core::ReasonCode::ClockReferenceMismatch,
                        definition->name.str());
  }
  const Nanos age = report->age_ns(now);
  if (age < 0) {
    // The synchronization report claims to come from the future: the local
    // reference reading and the report cannot be reconciled.
    return incomparable(core::EvidenceState::Conflicting,
                        core::ReasonCode::ClockOffsetUnavailable, definition->name.str());
  }
  if (age > report->valid_for_ns) {
    return incomparable(core::EvidenceState::Stale, core::ReasonCode::ClockSyncExpired,
                        definition->name.str());
  }

  ClockComparability result;
  result.comparable = true;
  result.uncertainty_ns = report->uncertainty_ns;
  const bool aging = age > report->valid_for_ns / 2;
  if (report->state == ClockSyncState::Holdover) {
    result.confidence = core::Confidence::Degraded;
    result.evidence.add_reason(core::ReasonCode::ClockHoldover, definition->name.str());
  } else if (aging) {
    result.confidence = core::Confidence::Moderate;
    result.evidence.add_reason(core::ReasonCode::ClockSyncStale, definition->name.str());
  } else {
    result.confidence = core::Confidence::High;
  }
  result.evidence.set_state(core::EvidenceState::Observed);
  result.evidence.set_freshness(aging ? core::Freshness::Aging : core::Freshness::Fresh);
  result.evidence.set_confidence(result.confidence);
  return result;
}

ClockRegistry::AgeEstimate ClockRegistry::estimate_age(const Timestamp& observed,
                                                         const Timestamp& now_reference) const {
  AgeEstimate estimate;
  if (!observed.valid() || !now_reference.valid()) {
    estimate.evidence = core::Evidence::unknown(core::ReasonCode::ClockDomainMissing,
                                                "missing clock domain identity");
    return estimate;
  }
  if (observed.domain == now_reference.domain) {
    // Same domain: the difference is meaningful without any synchronization.
    estimate.age_ns = now_reference.ns - observed.ns;
    estimate.evidence.set_state(core::EvidenceState::Observed);
    estimate.evidence.set_confidence(core::Confidence::High);
    estimate.evidence.add_reason(core::ReasonCode::ClockSelfComparable);
    return estimate;
  }
  const ClockSync* report = sync(observed.domain);
  if (report == nullptr || !report->reference.valid()) {
    estimate.evidence = core::Evidence::unknown(core::ReasonCode::ClockOffsetUnavailable,
                                                observed.domain.to_hex());
    return estimate;
  }
  if (report->reference != now_reference.domain) {
    estimate.evidence = core::Evidence::conflicting(core::ReasonCode::ClockReferenceMismatch,
                                                    observed.domain.to_hex());
    return estimate;
  }
  if (report->state == ClockSyncState::Unknown || report->state == ClockSyncState::Unsynced) {
    estimate.evidence = core::Evidence::refused(core::ReasonCode::ClockUnsynced,
                                                observed.domain.to_hex());
    return estimate;
  }
  const Nanos report_age = report->age_ns(now_reference);
  if (report_age < 0 || report_age > report->valid_for_ns) {
    estimate.evidence = core::Evidence::stale(core::ReasonCode::ClockSyncExpired,
                                              observed.domain.to_hex());
    return estimate;
  }
  // reference_reading = domain_reading - offset
  estimate.age_ns = now_reference.ns - (observed.ns - report->offset_ns);
  estimate.evidence.set_state(core::EvidenceState::Observed);
  estimate.evidence.set_confidence(report->state == ClockSyncState::Holdover
                                       ? core::Confidence::Degraded
                                       : core::Confidence::Moderate);
  estimate.evidence.set_freshness(report_age > report->valid_for_ns / 2 ? core::Freshness::Aging
                                                                       : core::Freshness::Fresh);
  estimate.evidence.add_reason(core::ReasonCode::ClockComparable, observed.domain.to_hex());
  if (report->state == ClockSyncState::Holdover) {
    estimate.evidence.add_reason(core::ReasonCode::ClockHoldover, observed.domain.to_hex());
  }
  return estimate;
}

Result<ClockComparability> ClockRegistry::compare(ClockDomainId a, ClockDomainId b,
                                                  const Timestamp& now, GenerationId generation,
                                                  EpochId epoch, IncarnationId incarnation) const {
  if (!a.valid() || !b.valid()) {
    return incomparable(core::EvidenceState::Unknown, core::ReasonCode::ClockDomainMissing,
                        "missing clock domain identity");
  }
  if (a == b) {
    // Differences between two readings of the same clock are always meaningful,
    // even when the clock is free running: no cross-domain conversion happens.
    ClockComparability result;
    result.comparable = true;
    result.uncertainty_ns = 0;
    result.confidence = core::Confidence::High;
    result.evidence.set_state(core::EvidenceState::Observed);
    result.evidence.set_freshness(core::Freshness::Fresh);
    result.evidence.set_confidence(core::Confidence::High);
    result.evidence.add_reason(core::ReasonCode::ClockSelfComparable);
    return result;
  }

  // The local reference domain has no synchronization record of its own: a
  // domain that is synchronized to it is directly comparable with it.
  const ClockDomainId reference = core::reference_clock_domain();
  if (a == reference || b == reference) {
    const ClockDomainId remote = (a == reference) ? b : a;
    LATOBS_TRY(single, compare_single(remote, now, generation, epoch, incarnation));
    if (!single.comparable) return single;
    const ClockSync* remote_sync = sync(remote);
    if (remote_sync == nullptr || remote_sync->reference != reference) {
      return incomparable(core::EvidenceState::Conflicting,
                          core::ReasonCode::ClockReferenceMismatch,
                          "synchronization does not point at the local reference domain");
    }
    // The uncertainty ceiling applies to the reference comparison as well: a
    // synchronized but imprecise domain is still not usable for derivation.
    if (single.uncertainty_ns > policy_.comparability.max_uncertainty_ns) {
      ClockComparability refused =
          incomparable(core::EvidenceState::Refused, core::ReasonCode::ClockUncertaintyExceeded,
                       std::to_string(single.uncertainty_ns));
      refused.uncertainty_ns = single.uncertainty_ns;
      return refused;
    }
    ClockComparability result;
    result.comparable = true;
    result.uncertainty_ns = single.uncertainty_ns;
    result.offset_first_ns = (a == reference) ? 0 : remote_sync->offset_ns;
    result.offset_second_ns = (b == reference) ? 0 : remote_sync->offset_ns;
    result.confidence = single.confidence;
    result.evidence = single.evidence;
    result.evidence.add_reason(core::ReasonCode::ClockComparable);
    return result;
  }

  LATOBS_TRY(left, compare_single(a, now, generation, epoch, incarnation));
  if (!left.comparable) return left;
  LATOBS_TRY(right, compare_single(b, now, generation, epoch, incarnation));
  if (!right.comparable) return right;

  const ClockSync* left_sync = sync(a);
  const ClockSync* right_sync = sync(b);
  left.offset_first_ns = left_sync->offset_ns;
  left.offset_second_ns = right_sync->offset_ns;

  if (left_sync->reference != right_sync->reference) {
    return incomparable(core::EvidenceState::Conflicting, core::ReasonCode::ClockReferenceMismatch,
                        "sync records do not share a reference domain");
  }
  if (!left_sync->reference.valid()) {
    return incomparable(core::EvidenceState::Refused, core::ReasonCode::ClockReferenceMismatch,
                        "sync records do not declare a reference domain");
  }

  const Nanos combined = left.uncertainty_ns + right.uncertainty_ns;
  if (combined < 0) {
    return incomparable(core::EvidenceState::Refused, core::ReasonCode::ArithmeticOverflow,
                        "combined clock uncertainty overflowed");
  }
  if (combined > policy_.comparability.max_uncertainty_ns) {
    ClockComparability result =
        incomparable(core::EvidenceState::Refused, core::ReasonCode::ClockUncertaintyExceeded,
                     std::to_string(combined));
    result.uncertainty_ns = combined;
    return result;
  }

  ClockComparability result;
  result.comparable = true;
  result.uncertainty_ns = combined;
  result.offset_first_ns = left.offset_first_ns;
  result.offset_second_ns = left.offset_second_ns;
  const int left_rank = static_cast<int>(left.confidence);
  const int right_rank = static_cast<int>(right.confidence);
  result.confidence = static_cast<core::Confidence>(left_rank > right_rank ? left_rank : right_rank);
  result.evidence = core::Evidence::merge(left.evidence, right.evidence);
  result.evidence.add_reason(core::ReasonCode::ClockComparable);
  return result;
}

}  // namespace latobs::model
