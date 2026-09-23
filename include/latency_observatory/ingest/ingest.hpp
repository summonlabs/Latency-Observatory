// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/core/evidence.hpp"
#include "latency_observatory/core/json.hpp"
#include "latency_observatory/core/policy.hpp"
#include "latency_observatory/ingest/vocabulary.hpp"
#include "latency_observatory/model/clock.hpp"
#include "latency_observatory/model/entities.hpp"
#include "latency_observatory/model/measurement.hpp"

namespace latobs::ingest {

/// The verdict for one submitted record. "Historical" means the record is
/// retained and visible in history, but it is never eligible for the current
/// aggregation: it belongs to an older epoch, incarnation, generation,
/// revision, or it is no longer fresh.
enum class AdmitVerdict : std::uint8_t { AcceptedCurrent, AcceptedHistorical, Rejected };
std::string_view to_string(AdmitVerdict verdict) noexcept;

struct SampleVerdict {
  MeasurementId id;
  AdmitVerdict verdict = AdmitVerdict::Rejected;
  core::Evidence evidence;
};

/// Bounded ingestion report. Detailed verdicts are kept for the first records
/// only; the complete picture is always in the reason counters.
struct IngestReport {
  static constexpr std::size_t kMaxDetailedVerdicts = 64;

  std::uint64_t received = 0;
  std::uint64_t accepted_current = 0;
  std::uint64_t accepted_historical = 0;
  std::uint64_t rejected = 0;
  std::uint64_t hop_dwells_computed = 0;
  std::uint64_t hop_dwells_unknown = 0;
  std::uint64_t queue_dwells_computed = 0;
  std::uint64_t queue_dwells_unknown = 0;
  std::uint64_t sequence_gaps = 0;
  std::uint64_t reordered = 0;
  std::uint64_t replays = 0;
  bool verdicts_truncated = false;
  std::vector<SampleVerdict> verdicts;
  std::map<core::ReasonCode, std::uint64_t> reason_counts;
  core::Evidence evidence;

  void note(const core::Evidence& source_evidence);
};

struct IngestRequest {
  std::vector<model::MeasurementRecord> records;
  /// Local reference time at which the batch was received.
  Timestamp received_at;
  MonoTime received_mono;
};

/// Fence state of one (source, epoch, incarnation) session. Sessions are
/// ordered by first observation: a record from an older session is historical,
/// never current, and a repeated sequence number is refused outright.
struct SessionState {
  std::uint64_t ordinal = 0;
  Revision source_revision;
  bool has_last_sequence = false;
  Sequence last_sequence;
  std::set<std::uint64_t> recent_sequences;
  std::deque<std::uint64_t> recent_order;
  std::uint64_t accepted_current = 0;
  std::uint64_t accepted_historical = 0;
  std::uint64_t rejected = 0;
};

struct SourceFenceState {
  std::uint64_t next_ordinal = 1;
  std::map<std::pair<EpochId, IncarnationId>, SessionState> sessions;
  EpochId current_epoch;
  IncarnationId current_incarnation;
  std::uint64_t current_ordinal = 0;
  std::uint64_t evicted_sessions = 0;
  Revision seen_revision;
};

/// The admission gate. It is the only place where evidence becomes current:
/// every fence (clock comparability, freshness, epoch, incarnation, generation,
/// revision, sequence) is applied here and recorded as a typed reason.
///
/// The gate is deliberately not internally synchronized: the runtime owns it
/// and serialises access, which keeps the fence decisions totally ordered.
class IngestGate {
 public:
  IngestGate(const core::RuntimePolicy& policy, const model::Catalog& catalog,
             const model::ClockRegistry& clocks)
      : policy_(policy), catalog_(catalog), clocks_(clocks) {}

  /// Validates, fences and (when accepted) enriches the submitted records.
  /// Accepted records are appended to \p accepted_current / \p accepted_history
  /// in submission order. Rejected records are reported but never returned.
  [[nodiscard]] Status admit(IngestRequest request,
                             std::vector<model::MeasurementRecord>& accepted_current,
                             std::vector<model::MeasurementRecord>& accepted_history,
                             IngestReport& report);

  [[nodiscard]] const std::map<SourceId, SourceFenceState>& fences() const noexcept {
    return fences_;
  }

  /// Restores fence state loaded from persistence. Fence state is what makes a
  /// restarted runtime continue to refuse replays rather than re-accept them.
  void restore_fence(SourceId source, SourceFenceState state);

 private:
  [[nodiscard]] Status classify(model::MeasurementRecord& record, const Timestamp& now,
                                AdmitVerdict& verdict, core::Evidence& evidence,
                                IngestReport& report);
  [[nodiscard]] Status derive_dwells(model::MeasurementRecord& record, IngestReport& report);
  [[nodiscard]] Status apply_session_fence(model::MeasurementRecord& record, AdmitVerdict& verdict,
                                           core::Evidence& evidence, IngestReport& report);
  void record_reason(IngestReport& report, core::ReasonCode code, std::uint64_t count = 1);

  core::RuntimePolicy policy_;
  const model::Catalog& catalog_;
  const model::ClockRegistry& clocks_;
  std::map<SourceId, SourceFenceState> fences_;
};

/// Canonical JSON rendering of a report.
void write_json(core::JsonWriter& writer, const IngestReport& report);

}  // namespace latobs::ingest
