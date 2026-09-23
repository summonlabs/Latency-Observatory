// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/core/evidence.hpp"

#include <algorithm>
#include <cstddef>

namespace latobs::core {
namespace {

struct NamedState {
  EvidenceState value;
  std::string_view name;
};
constexpr NamedState kStates[] = {
    {EvidenceState::Observed, "observed"},   {EvidenceState::Incomplete, "incomplete"},
    {EvidenceState::Missing, "missing"},     {EvidenceState::Stale, "stale"},
    {EvidenceState::Conflicting, "conflicting"}, {EvidenceState::Unknown, "unknown"},
    {EvidenceState::Unsupported, "unsupported"}, {EvidenceState::Refused, "refused"},
};

struct NamedFreshness {
  Freshness value;
  std::string_view name;
};
constexpr NamedFreshness kFreshness[] = {
    {Freshness::Fresh, "fresh"}, {Freshness::Aging, "aging"}, {Freshness::Stale, "stale"},
    {Freshness::Expired, "expired"}, {Freshness::Unknown, "unknown"},
};

struct NamedConfidence {
  Confidence value;
  std::string_view name;
};
constexpr NamedConfidence kConfidence[] = {
    {Confidence::High, "high"}, {Confidence::Moderate, "moderate"}, {Confidence::Low, "low"},
    {Confidence::Degraded, "degraded"}, {Confidence::None, "none"},
};

struct NamedReason {
  ReasonCode value;
  std::string_view name;
};
constexpr NamedReason kReasons[] = {
    {ReasonCode::MalformedPayload, "malformed_payload"},
    {ReasonCode::LengthLimitExceeded, "length_limit_exceeded"},
    {ReasonCode::DepthLimitExceeded, "depth_limit_exceeded"},
    {ReasonCode::MissingField, "missing_field"},
    {ReasonCode::UnexpectedField, "unexpected_field"},
    {ReasonCode::InvalidFieldValue, "invalid_field_value"},
    {ReasonCode::DuplicateField, "duplicate_field"},
    {ReasonCode::UnsupportedSchema, "unsupported_schema"},
    {ReasonCode::UnknownSource, "unknown_source"},
    {ReasonCode::UnknownPath, "unknown_path"},
    {ReasonCode::UnknownHop, "unknown_hop"},
    {ReasonCode::UnknownLink, "unknown_link"},
    {ReasonCode::UnknownQueue, "unknown_queue"},
    {ReasonCode::UnknownGeneration, "unknown_generation"},
    {ReasonCode::UnknownClockDomain, "unknown_clock_domain"},
    {ReasonCode::UnknownBaseline, "unknown_baseline"},
    {ReasonCode::IdCollision, "id_collision"},
    {ReasonCode::NameConflict, "name_conflict"},
    {ReasonCode::EpochAdvanced, "epoch_advanced"},
    {ReasonCode::EpochReplayed, "epoch_replayed"},
    {ReasonCode::IncarnationChanged, "incarnation_changed"},
    {ReasonCode::IncarnationReplayed, "incarnation_replayed"},
    {ReasonCode::SequenceReplay, "sequence_replay"},
    {ReasonCode::SequenceGap, "sequence_gap"},
    {ReasonCode::SequenceAdvanced, "sequence_advanced"},
    {ReasonCode::SequenceReordered, "sequence_reordered"},
    {ReasonCode::SessionEvicted, "session_evicted"},
    {ReasonCode::GenerationSuperseded, "generation_superseded"},
    {ReasonCode::RevisionChanged, "revision_changed"},
    {ReasonCode::AuthorityConflict, "authority_conflict"},
    {ReasonCode::AuthorityDowngrade, "authority_downgrade"},
    {ReasonCode::ClockDomainMissing, "clock_domain_missing"},
    {ReasonCode::ClockSelfComparable, "clock_self_comparable"},
    {ReasonCode::ClockUnsynced, "clock_unsynced"},
    {ReasonCode::ClockSyncStale, "clock_sync_stale"},
    {ReasonCode::ClockSyncExpired, "clock_sync_expired"},
    {ReasonCode::ClockSyncGenerationMismatch, "clock_sync_generation_mismatch"},
    {ReasonCode::ClockSyncEpochMismatch, "clock_sync_epoch_mismatch"},
    {ReasonCode::ClockSyncIncarnationMismatch, "clock_sync_incarnation_mismatch"},
    {ReasonCode::ClockReferenceMismatch, "clock_reference_mismatch"},
    {ReasonCode::ClockUncertaintyExceeded, "clock_uncertainty_exceeded"},
    {ReasonCode::ClockHoldover, "clock_holdover"},
    {ReasonCode::ClockOffsetUnavailable, "clock_offset_unavailable"},
    {ReasonCode::ClockComparable, "clock_comparable"},
    {ReasonCode::EvidenceFresh, "evidence_fresh"},
    {ReasonCode::EvidenceAging, "evidence_aging"},
    {ReasonCode::EvidenceStale, "evidence_stale"},
    {ReasonCode::EvidenceExpired, "evidence_expired"},
    {ReasonCode::FreshnessUnknown, "freshness_unknown"},
    {ReasonCode::RestartLoadedEvidence, "restart_loaded_evidence"},
    {ReasonCode::HopMissing, "hop_missing"},
    {ReasonCode::HopUnsupported, "hop_unsupported"},
    {ReasonCode::HopConflicting, "hop_conflicting"},
    {ReasonCode::HopNotContiguous, "hop_not_contiguous"},
    {ReasonCode::HopOutOfOrder, "hop_out_of_order"},
    {ReasonCode::HopOverlap, "hop_overlap"},
    {ReasonCode::HopGap, "hop_gap"},
    {ReasonCode::PartialCoverage, "partial_coverage"},
    {ReasonCode::NoSamples, "no_samples"},
    {ReasonCode::EmptyWindow, "empty_window"},
    {ReasonCode::ArithmeticOverflow, "arithmetic_overflow"},
    {ReasonCode::AggregateDeterministic, "aggregate_deterministic"},
    {ReasonCode::InsufficientSamples, "insufficient_samples"},
    {ReasonCode::HistogramUnderflow, "histogram_underflow"},
    {ReasonCode::HistogramOverflowValue, "histogram_overflow_value"},
    {ReasonCode::BaselineMissing, "baseline_missing"},
    {ReasonCode::BaselineGenerationMismatch, "baseline_generation_mismatch"},
    {ReasonCode::BaselinePathMismatch, "baseline_path_mismatch"},
    {ReasonCode::BaselineClockDomainMismatch, "baseline_clock_domain_mismatch"},
    {ReasonCode::BaselineHistogramMismatch, "baseline_histogram_mismatch"},
    {ReasonCode::BaselineRevisionMismatch, "baseline_revision_mismatch"},
    {ReasonCode::BaselineStale, "baseline_stale"},
    {ReasonCode::BaselineExpired, "baseline_expired"},
    {ReasonCode::BaselineMatched, "baseline_matched"},
    {ReasonCode::IncomparableClocks, "incomparable_clocks"},
    {ReasonCode::NonTilingCoverage, "non_tiling_coverage"},
    {ReasonCode::SemanticsUnsupported, "semantics_unsupported"},
    {ReasonCode::QueueSemanticsUndeclared, "queue_semantics_undeclared"},
    {ReasonCode::RefusedByPolicy, "refused_by_policy"},
    {ReasonCode::NoCausalInference, "no_causal_inference"},
    {ReasonCode::UnaccountedResidual, "unaccounted_residual"},
    {ReasonCode::ResidualWithinTolerance, "residual_within_tolerance"},
    {ReasonCode::AttributionComplete, "attribution_complete"},
    {ReasonCode::AttributionPartial, "attribution_partial"},
    {ReasonCode::DeviationNone, "deviation_none"},
    {ReasonCode::DeviationWatch, "deviation_watch"},
    {ReasonCode::DeviationElevated, "deviation_elevated"},
    {ReasonCode::DeviationSuppressed, "deviation_suppressed"},
    {ReasonCode::BaselineAbsent, "baseline_absent"},
    {ReasonCode::StoreOpenFailed, "store_open_failed"},
    {ReasonCode::StoreFormatUnsupported, "store_format_unsupported"},
    {ReasonCode::RecordIntegrityFailure, "record_integrity_failure"},
    {ReasonCode::SegmentTruncated, "segment_truncated"},
    {ReasonCode::SegmentRejected, "segment_rejected"},
    {ReasonCode::PersistenceCapacityExceeded, "persistence_capacity_exceeded"},
    {ReasonCode::RecoveryConservative, "recovery_conservative"},
    {ReasonCode::RecoveredEvidence, "recovered_evidence"},
    {ReasonCode::OperationCancelled, "operation_cancelled"},
    {ReasonCode::RuntimeShuttingDown, "runtime_shutting_down"},
    {ReasonCode::WorkerPoolSaturated, "worker_pool_saturated"},
    {ReasonCode::QueueFull, "queue_full"},
    {ReasonCode::BatchLimitExceeded, "batch_limit_exceeded"},
    {ReasonCode::ExportTruncated, "export_truncated"},
};

/// Severity ordering used by merge: higher wins.
[[nodiscard]] int state_rank(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::Observed: return 0;
    case EvidenceState::Incomplete: return 1;
    case EvidenceState::Missing: return 2;
    case EvidenceState::Stale: return 3;
    case EvidenceState::Conflicting: return 4;
    case EvidenceState::Unknown: return 5;
    case EvidenceState::Unsupported: return 6;
    case EvidenceState::Refused: return 7;
  }
  return 5;
}

[[nodiscard]] int freshness_rank(Freshness freshness) noexcept {
  switch (freshness) {
    case Freshness::Fresh: return 0;
    case Freshness::Aging: return 1;
    case Freshness::Stale: return 2;
    case Freshness::Expired: return 3;
    case Freshness::Unknown: return 4;
  }
  return 4;
}

[[nodiscard]] int confidence_rank(Confidence confidence) noexcept {
  switch (confidence) {
    case Confidence::High: return 0;
    case Confidence::Moderate: return 1;
    case Confidence::Low: return 2;
    case Confidence::Degraded: return 3;
    case Confidence::None: return 4;
  }
  return 4;
}

}  // namespace

std::string_view to_string(EvidenceState state) noexcept {
  for (const NamedState& entry : kStates) {
    if (entry.value == state) return entry.name;
  }
  return "unknown";
}

bool parse_evidence_state(std::string_view text, EvidenceState& out) noexcept {
  for (const NamedState& entry : kStates) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::string_view to_string(Freshness freshness) noexcept {
  for (const NamedFreshness& entry : kFreshness) {
    if (entry.value == freshness) return entry.name;
  }
  return "unknown";
}

bool parse_freshness(std::string_view text, Freshness& out) noexcept {
  for (const NamedFreshness& entry : kFreshness) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::string_view to_string(Confidence confidence) noexcept {
  for (const NamedConfidence& entry : kConfidence) {
    if (entry.value == confidence) return entry.name;
  }
  return "none";
}

bool parse_confidence(std::string_view text, Confidence& out) noexcept {
  for (const NamedConfidence& entry : kConfidence) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::string_view to_string(ReasonCode code) noexcept {
  for (const NamedReason& entry : kReasons) {
    if (entry.value == code) return entry.name;
  }
  return "unknown_reason";
}

bool parse_reason_code(std::string_view text, ReasonCode& out) noexcept {
  for (const NamedReason& entry : kReasons) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

Evidence Evidence::observed(Freshness freshness, Confidence confidence) {
  Evidence evidence;
  evidence.state_ = EvidenceState::Observed;
  evidence.freshness_ = freshness;
  evidence.confidence_ = confidence;
  return evidence;
}

Evidence Evidence::observed_fresh() {
  Evidence evidence = observed(Freshness::Fresh, Confidence::High);
  evidence.add_reason(ReasonCode::EvidenceFresh);
  return evidence;
}

Evidence Evidence::missing(ReasonCode code, std::string detail) {
  Evidence evidence;
  evidence.state_ = EvidenceState::Missing;
  evidence.freshness_ = Freshness::Unknown;
  evidence.confidence_ = Confidence::None;
  evidence.add_reason(code, std::move(detail));
  return evidence;
}

Evidence Evidence::incomplete(ReasonCode code, std::string detail) {
  Evidence evidence;
  evidence.state_ = EvidenceState::Incomplete;
  evidence.confidence_ = Confidence::Low;
  evidence.add_reason(code, std::move(detail));
  return evidence;
}

Evidence Evidence::stale(ReasonCode code, std::string detail) {
  Evidence evidence;
  evidence.state_ = EvidenceState::Stale;
  evidence.freshness_ = Freshness::Stale;
  evidence.confidence_ = Confidence::None;
  evidence.add_reason(code, std::move(detail));
  return evidence;
}

Evidence Evidence::conflicting(ReasonCode code, std::string detail) {
  Evidence evidence;
  evidence.state_ = EvidenceState::Conflicting;
  evidence.confidence_ = Confidence::None;
  evidence.add_reason(code, std::move(detail));
  return evidence;
}

Evidence Evidence::unknown(ReasonCode code, std::string detail) {
  Evidence evidence;
  evidence.state_ = EvidenceState::Unknown;
  evidence.confidence_ = Confidence::None;
  evidence.add_reason(code, std::move(detail));
  return evidence;
}

Evidence Evidence::unsupported(ReasonCode code, std::string detail) {
  Evidence evidence;
  evidence.state_ = EvidenceState::Unsupported;
  evidence.confidence_ = Confidence::None;
  evidence.add_reason(code, std::move(detail));
  return evidence;
}

Evidence Evidence::refused(ReasonCode code, std::string detail) {
  Evidence evidence;
  evidence.state_ = EvidenceState::Refused;
  evidence.confidence_ = Confidence::None;
  evidence.add_reason(code, std::move(detail));
  return evidence;
}

bool Evidence::usable() const noexcept {
  return state_ == EvidenceState::Observed &&
         (freshness_ == Freshness::Fresh || freshness_ == Freshness::Aging);
}

Evidence& Evidence::add_reason(ReasonCode code, std::string detail) {
  Reason reason{code, std::move(detail)};
  if (std::find(reasons_.begin(), reasons_.end(), reason) != reasons_.end()) {
    return *this;  // identical reason already recorded: keep output stable
  }
  if (reasons_.size() >= kMaxReasons) {
    reasons_truncated_ = true;
    return *this;
  }
  reasons_.push_back(std::move(reason));
  return *this;
}

Evidence& Evidence::set_state(EvidenceState state) noexcept {
  state_ = state;
  return *this;
}

Evidence& Evidence::set_freshness(Freshness freshness) noexcept {
  freshness_ = freshness;
  return *this;
}

Evidence& Evidence::set_confidence(Confidence confidence) noexcept {
  confidence_ = confidence;
  return *this;
}

Evidence Evidence::merge(const Evidence& left, const Evidence& right) {
  Evidence result;
  const bool left_coverage = left.state_ == EvidenceState::Observed ||
                             left.state_ == EvidenceState::Incomplete ||
                             left.state_ == EvidenceState::Missing;
  const bool right_coverage = right.state_ == EvidenceState::Observed ||
                              right.state_ == EvidenceState::Incomplete ||
                              right.state_ == EvidenceState::Missing;
  if (left_coverage && right_coverage) {
    const bool any_missing = left.state_ == EvidenceState::Missing ||
                             right.state_ == EvidenceState::Missing;
    const bool any_incomplete = left.state_ == EvidenceState::Incomplete ||
                                right.state_ == EvidenceState::Incomplete;
    result.state_ = (any_missing || any_incomplete) ? EvidenceState::Incomplete
                                                    : EvidenceState::Observed;
  } else {
    result.state_ = state_rank(left.state_) >= state_rank(right.state_) ? left.state_ : right.state_;
  }
  result.freshness_ = freshness_rank(left.freshness_) >= freshness_rank(right.freshness_)
                          ? left.freshness_
                          : right.freshness_;
  result.confidence_ = confidence_rank(left.confidence_) >= confidence_rank(right.confidence_)
                           ? left.confidence_
                           : right.confidence_;
  for (const Reason& reason : left.reasons_) result.add_reason(reason.code, reason.detail);
  for (const Reason& reason : right.reasons_) result.add_reason(reason.code, reason.detail);
  result.reasons_truncated_ = left.reasons_truncated_ || right.reasons_truncated_ ||
                              result.reasons_truncated_;
  result.normalize();
  return result;
}

void Evidence::normalize() {
  std::sort(reasons_.begin(), reasons_.end());
  reasons_.erase(std::unique(reasons_.begin(), reasons_.end()), reasons_.end());
}

std::string Evidence::summarize() const {
  std::string out;
  out.append(to_string(state_));
  out.append("/");
  out.append(to_string(freshness_));
  out.append("/");
  out.append(to_string(confidence_));
  for (const Reason& reason : reasons_) {
    out.append(" ");
    out.append(to_string(reason.code));
    if (!reason.detail.empty()) {
      out.push_back('(');
      out.append(reason.detail);
      out.push_back(')');
    }
  }
  if (reasons_truncated_) out.append(" reasons_truncated");
  return out;
}

bool Evidence::operator==(const Evidence& other) const noexcept {
  return state_ == other.state_ && freshness_ == other.freshness_ &&
         confidence_ == other.confidence_ && reasons_ == other.reasons_ &&
         reasons_truncated_ == other.reasons_truncated_;
}

}  // namespace latobs::core
