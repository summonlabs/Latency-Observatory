// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace latobs::core {

/// What the runtime knows about a piece of evidence. The states are distinct on
/// purpose: absence of evidence is never represented as a zero value, and
/// "unsupported" (semantics do not permit the computation) is different from
/// "refused" (policy declined the computation) and from "unknown".
enum class EvidenceState : std::uint8_t {
  Observed = 0,
  Incomplete,
  Missing,
  Stale,
  Conflicting,
  Unknown,
  Unsupported,
  Refused,
};

std::string_view to_string(EvidenceState state) noexcept;
bool parse_evidence_state(std::string_view text, EvidenceState& out) noexcept;

/// How old the evidence is relative to the observation that uses it.
enum class Freshness : std::uint8_t {
  Fresh = 0,
  Aging,
  Stale,
  Expired,
  Unknown,
};

std::string_view to_string(Freshness freshness) noexcept;
bool parse_freshness(std::string_view text, Freshness& out) noexcept;

/// Ordinal confidence, never a fabricated probability.
enum class Confidence : std::uint8_t {
  High = 0,
  Moderate,
  Low,
  Degraded,
  None,
};

std::string_view to_string(Confidence confidence) noexcept;
bool parse_confidence(std::string_view text, Confidence& out) noexcept;

/// Stable reason vocabulary. Every result carries the reasons that produced it,
/// so explanations are derived from data rather than reconstructed by prose.
enum class ReasonCode : std::uint16_t {
  // Structural validation
  MalformedPayload = 0,
  LengthLimitExceeded,
  DepthLimitExceeded,
  MissingField,
  UnexpectedField,
  InvalidFieldValue,
  DuplicateField,
  UnsupportedSchema,

  // Registry / identity
  UnknownSource,
  UnknownPath,
  UnknownHop,
  UnknownLink,
  UnknownQueue,
  UnknownGeneration,
  UnknownClockDomain,
  UnknownBaseline,
  IdCollision,
  NameConflict,

  // Fences
  EpochAdvanced,
  EpochReplayed,
  IncarnationChanged,
  IncarnationReplayed,
  SequenceReplay,
  SequenceGap,
  SequenceAdvanced,
  SequenceReordered,
  SessionEvicted,
  GenerationSuperseded,
  RevisionChanged,
  AuthorityConflict,
  AuthorityDowngrade,

  // Clocks
  ClockDomainMissing,
  ClockSelfComparable,
  ClockUnsynced,
  ClockSyncStale,
  ClockSyncExpired,
  ClockSyncGenerationMismatch,
  ClockSyncEpochMismatch,
  ClockSyncIncarnationMismatch,
  ClockReferenceMismatch,
  ClockUncertaintyExceeded,
  ClockHoldover,
  ClockOffsetUnavailable,
  ClockComparable,

  // Freshness
  EvidenceFresh,
  EvidenceAging,
  EvidenceStale,
  EvidenceExpired,
  FreshnessUnknown,
  RestartLoadedEvidence,

  // Coverage
  HopMissing,
  HopUnsupported,
  HopConflicting,
  HopNotContiguous,
  HopOutOfOrder,
  HopOverlap,
  HopGap,
  PartialCoverage,
  NoSamples,
  EmptyWindow,

  // Aggregation
  ArithmeticOverflow,
  AggregateDeterministic,
  InsufficientSamples,
  HistogramUnderflow,
  HistogramOverflowValue,

  // Baselines
  BaselineMissing,
  BaselineGenerationMismatch,
  BaselinePathMismatch,
  BaselineClockDomainMismatch,
  BaselineHistogramMismatch,
  BaselineRevisionMismatch,
  BaselineStale,
  BaselineExpired,
  BaselineMatched,

  // Attribution
  IncomparableClocks,
  NonTilingCoverage,
  SemanticsUnsupported,
  QueueSemanticsUndeclared,
  RefusedByPolicy,
  NoCausalInference,
  UnaccountedResidual,
  ResidualWithinTolerance,
  AttributionComplete,
  AttributionPartial,

  // Anomaly evidence
  DeviationNone,
  DeviationWatch,
  DeviationElevated,
  DeviationSuppressed,
  BaselineAbsent,

  // Persistence
  StoreOpenFailed,
  StoreFormatUnsupported,
  RecordIntegrityFailure,
  SegmentTruncated,
  SegmentRejected,
  PersistenceCapacityExceeded,
  RecoveryConservative,
  RecoveredEvidence,

  // Runtime
  OperationCancelled,
  RuntimeShuttingDown,
  WorkerPoolSaturated,
  QueueFull,
  BatchLimitExceeded,
  ExportTruncated,
};

std::string_view to_string(ReasonCode code) noexcept;
bool parse_reason_code(std::string_view text, ReasonCode& out) noexcept;

/// A reason is a code plus a bounded, deterministic detail string.
struct Reason {
  ReasonCode code = ReasonCode::MalformedPayload;
  std::string detail;

  [[nodiscard]] bool operator==(const Reason& other) const noexcept {
    return code == other.code && detail == other.detail;
  }
  [[nodiscard]] bool operator<(const Reason& other) const noexcept {
    if (code != other.code) return static_cast<std::uint16_t>(code) < static_cast<std::uint16_t>(other.code);
    return detail < other.detail;
  }
};

/// The evidence attached to every observation, aggregate and derived result.
class Evidence {
 public:
  static constexpr std::size_t kMaxReasons = 24;

  Evidence() = default;

  [[nodiscard]] static Evidence observed(Freshness freshness, Confidence confidence);
  [[nodiscard]] static Evidence observed_fresh();
  [[nodiscard]] static Evidence missing(ReasonCode code, std::string detail = {});
  [[nodiscard]] static Evidence incomplete(ReasonCode code, std::string detail = {});
  [[nodiscard]] static Evidence stale(ReasonCode code, std::string detail = {});
  [[nodiscard]] static Evidence conflicting(ReasonCode code, std::string detail = {});
  [[nodiscard]] static Evidence unknown(ReasonCode code, std::string detail = {});
  [[nodiscard]] static Evidence unsupported(ReasonCode code, std::string detail = {});
  [[nodiscard]] static Evidence refused(ReasonCode code, std::string detail = {});

  [[nodiscard]] EvidenceState state() const noexcept { return state_; }
  [[nodiscard]] Freshness freshness() const noexcept { return freshness_; }
  [[nodiscard]] Confidence confidence() const noexcept { return confidence_; }
  [[nodiscard]] const std::vector<Reason>& reasons() const noexcept { return reasons_; }
  [[nodiscard]] bool reasons_truncated() const noexcept { return reasons_truncated_; }

  /// True only when the value may be used as current evidence: the state is
  /// observed and the freshness is fresh or aging.
  [[nodiscard]] bool usable() const noexcept;
  /// True when a numeric value exists at all (observed, even if stale).
  [[nodiscard]] bool has_value() const noexcept {
    return state_ == EvidenceState::Observed || state_ == EvidenceState::Incomplete ||
           state_ == EvidenceState::Stale;
  }

  Evidence& add_reason(ReasonCode code, std::string detail = {});
  Evidence& set_state(EvidenceState state) noexcept;
  Evidence& set_freshness(Freshness freshness) noexcept;
  Evidence& set_confidence(Confidence confidence) noexcept;

  /// Deterministic, order independent merge. Coverage states combine into
  /// Incomplete; anything else takes the most severe state.
  [[nodiscard]] static Evidence merge(const Evidence& left, const Evidence& right);

  /// Deterministic single-line rendering used by explain output.
  [[nodiscard]] std::string summarize() const;

  [[nodiscard]] bool operator==(const Evidence& other) const noexcept;

 private:
  void normalize();

  EvidenceState state_ = EvidenceState::Unknown;
  Freshness freshness_ = Freshness::Unknown;
  Confidence confidence_ = Confidence::None;
  std::vector<Reason> reasons_;
  bool reasons_truncated_ = false;
};

}  // namespace latobs::core
