// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/ingest/ingest.hpp"

#include <algorithm>
#include <utility>

#include "latency_observatory/core/checked.hpp"

namespace latobs::ingest {
namespace {

constexpr std::pair<AdmitVerdict, std::string_view> kVerdicts[] = {
    {AdmitVerdict::AcceptedCurrent, "accepted_current"},
    {AdmitVerdict::AcceptedHistorical, "accepted_historical"},
    {AdmitVerdict::Rejected, "rejected"},
};

[[nodiscard]] bool strictly_increasing_hop_indices(const std::vector<model::HopObservation>& hops) {
  for (std::size_t index = 1; index < hops.size(); ++index) {
    if (!(hops[index - 1].index < hops[index].index)) return false;
  }
  return true;
}

[[nodiscard]] core::ReasonCode freshness_reason(core::Freshness freshness) noexcept {
  switch (freshness) {
    case core::Freshness::Fresh: return core::ReasonCode::EvidenceFresh;
    case core::Freshness::Aging: return core::ReasonCode::EvidenceAging;
    case core::Freshness::Stale: return core::ReasonCode::EvidenceStale;
    case core::Freshness::Expired: return core::ReasonCode::EvidenceExpired;
    case core::Freshness::Unknown: return core::ReasonCode::FreshnessUnknown;
  }
  return core::ReasonCode::FreshnessUnknown;
}

}  // namespace

std::string_view to_string(AdmitVerdict verdict) noexcept {
  for (const auto& entry : kVerdicts) {
    if (entry.first == verdict) return entry.second;
  }
  return "rejected";
}

void IngestReport::note(const core::Evidence& source_evidence) {
  for (const core::Reason& reason : source_evidence.reasons()) {
    ++reason_counts[reason.code];
  }
}

void write_json(core::JsonWriter& writer, const IngestReport& report) {
  writer.begin_object();
  writer.field("received", report.received);
  writer.field("accepted_current", report.accepted_current);
  writer.field("accepted_historical", report.accepted_historical);
  writer.field("rejected", report.rejected);
  writer.field("hop_dwells_computed", report.hop_dwells_computed);
  writer.field("hop_dwells_unknown", report.hop_dwells_unknown);
  writer.field("queue_dwells_computed", report.queue_dwells_computed);
  writer.field("queue_dwells_unknown", report.queue_dwells_unknown);
  writer.field("sequence_gaps", report.sequence_gaps);
  writer.field("reordered", report.reordered);
  writer.field("replays", report.replays);
  writer.field("verdicts_truncated", report.verdicts_truncated);
  writer.field_array("verdicts");
  for (const SampleVerdict& verdict : report.verdicts) {
    writer.begin_object();
    writer.field("measurement", verdict.id.to_hex());
    writer.field("verdict", to_string(verdict.verdict));
    writer.field("evidence_state", core::to_string(verdict.evidence.state()));
    writer.field("freshness", core::to_string(verdict.evidence.freshness()));
    writer.field_array("reasons");
    for (const core::Reason& reason : verdict.evidence.reasons()) {
      writer.begin_object();
      writer.field("code", core::to_string(reason.code));
      writer.field("detail", reason.detail);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();
  writer.field_array("reason_counts");
  for (const auto& entry : report.reason_counts) {
    writer.begin_object();
    writer.field("code", core::to_string(entry.first));
    writer.field("count", entry.second);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

Status IngestGate::admit(IngestRequest request,
                         std::vector<model::MeasurementRecord>& accepted_current,
                         std::vector<model::MeasurementRecord>& accepted_history,
                         IngestReport& report) {
  report = IngestReport{};
  if (!request.received_at.valid()) {
    return Error(ErrorCode::InvalidArgument, "ingest requires a valid receive time");
  }
  if (request.records.size() > policy_.limits.max_batch_samples) {
    return Error(ErrorCode::CapacityExceeded, "ingest batch exceeds the configured limit",
                 std::to_string(request.records.size()));
  }
  report.received = request.records.size();

  for (model::MeasurementRecord& record : request.records) {
    record.stamp.received_at = request.received_at;
    record.stamp.received_mono = request.received_mono;
    record.id = model::derive_measurement_id(record.source, record.epoch, record.incarnation,
                                            record.sequence);

    AdmitVerdict verdict = AdmitVerdict::AcceptedCurrent;
    core::Evidence evidence;
    LATOBS_TRY_STATUS(classify(record, request.received_at, verdict, evidence, report));
    record.evidence = evidence;

    SampleVerdict sample_verdict;
    sample_verdict.id = record.id;
    sample_verdict.verdict = verdict;
    sample_verdict.evidence = evidence;
    if (report.verdicts.size() < IngestReport::kMaxDetailedVerdicts) {
      report.verdicts.push_back(std::move(sample_verdict));
    } else {
      report.verdicts_truncated = true;
    }
    report.note(evidence);

    switch (verdict) {
      case AdmitVerdict::Rejected:
        ++report.rejected;
        break;
      case AdmitVerdict::AcceptedHistorical:
        ++report.accepted_historical;
        accepted_history.push_back(std::move(record));
        break;
      case AdmitVerdict::AcceptedCurrent:
        ++report.accepted_current;
        accepted_current.push_back(std::move(record));
        break;
    }
  }

  report.evidence.set_state(core::EvidenceState::Observed);
  report.evidence.set_freshness(core::Freshness::Fresh);
  report.evidence.set_confidence(core::Confidence::High);
  if (report.rejected != 0) {
    report.evidence.set_state(core::EvidenceState::Incomplete);
    report.evidence.set_confidence(core::Confidence::Low);
  }
  if (report.replays != 0) {
    report.evidence.add_reason(core::ReasonCode::SequenceReplay);
  }
  if (report.sequence_gaps != 0) {
    report.evidence.add_reason(core::ReasonCode::SequenceGap,
                               std::to_string(report.sequence_gaps));
  }
  return core::ok_status();
}

Status IngestGate::classify(model::MeasurementRecord& record, const Timestamp& now,
                            AdmitVerdict& verdict, core::Evidence& evidence, IngestReport& report) {
  evidence = core::Evidence::observed(core::Freshness::Unknown, core::Confidence::High);
  // A hop observation starts as observed evidence: it is only downgraded by a
  // specific reason. Merging into an "unknown" placeholder would swallow the
  // precise states (conflicting, refused, unsupported) collected below.
  for (model::HopObservation& hop : record.hops) {
    hop.evidence = core::Evidence::observed(core::Freshness::Fresh, core::Confidence::High);
  }
  bool fatal = false;
  auto reject = [&](core::ReasonCode code, std::string detail) {
    evidence.set_state(core::EvidenceState::Conflicting);
    evidence.add_reason(code, std::move(detail));
    fatal = true;
  };

  // --- registry ---
  const Result<const model::PathDef*> path = catalog_.path(record.path);
  if (!path) {
    evidence.set_state(core::EvidenceState::Conflicting);
    evidence.add_reason(core::ReasonCode::UnknownPath, record.path.to_hex());
    verdict = AdmitVerdict::Rejected;
    return core::ok_status();
  }
  const Result<const model::SourceDescriptor*> source = catalog_.source(record.source);
  if (!source) {
    evidence.set_state(core::EvidenceState::Conflicting);
    evidence.add_reason(core::ReasonCode::UnknownSource, record.source.to_hex());
    verdict = AdmitVerdict::Rejected;
    return core::ok_status();
  }
  const Result<const model::GenerationDef*> generation = catalog_.generation(record.generation);
  if (!generation) {
    evidence.set_state(core::EvidenceState::Conflicting);
    evidence.add_reason(core::ReasonCode::UnknownGeneration, record.generation.to_hex());
    verdict = AdmitVerdict::Rejected;
    return core::ok_status();
  }
  if (!record.epoch.valid() || !record.incarnation.valid()) {
    reject(core::ReasonCode::InvalidFieldValue, "epoch and incarnation are required");
  }
  if (!record.source_revision.valid()) {
    reject(core::ReasonCode::RevisionChanged, "source revision is required");
  }

  // --- structural consistency of the exchange ---
  if (!record.domain.valid()) {
    reject(core::ReasonCode::ClockDomainMissing, "the exchange has no clock domain");
  }
  if (record.request.domain != record.domain || record.response.domain != record.domain) {
    reject(core::ReasonCode::ClockReferenceMismatch,
           "request and response must belong to the declared exchange clock domain");
  }
  if (record.response.ns < record.request.ns) {
    reject(core::ReasonCode::InvalidFieldValue, "response precedes the request");
  } else {
    const Nanos expected = record.response.ns - record.request.ns;
    if (record.rtt_ns != expected) {
      reject(core::ReasonCode::InvalidFieldValue, "declared end to end latency is inconsistent");
    }
  }
  if (!record.hops.empty() && !strictly_increasing_hop_indices(record.hops)) {
    reject(core::ReasonCode::HopNotContiguous, "hop indices must be strictly increasing");
  }

  for (model::HopObservation& hop : record.hops) {
    const Result<const model::HopDef*> definition = catalog_.hop(hop.hop);
    if (!definition) {
      hop.evidence = core::Evidence::merge(
          hop.evidence, core::Evidence::unsupported(core::ReasonCode::UnknownHop, hop.hop.to_hex()));
      continue;
    }
    if (!hop.entry.domain.valid() || !hop.exit.domain.valid()) {
      hop.evidence = core::Evidence::merge(
          hop.evidence,
          core::Evidence::unsupported(core::ReasonCode::ClockDomainMissing, hop.hop.to_hex()));
      continue;
    }
    const std::size_t position = static_cast<std::size_t>(hop.index.value());
    if (position >= path.value()->hops.size() || !(path.value()->hops[position] == hop.hop)) {
      reject(core::ReasonCode::HopConflicting, "hop does not match the generation bound path");
    }
  }

  // --- freshness ---
  const model::ClockRegistry::AgeEstimate age =
      clocks_.estimate_age(record.stamp.observed_at, now);
  evidence = core::Evidence::merge(evidence, age.evidence);
  if (age.age_ns.has_value()) {
    const core::Freshness freshness = policy_.freshness.classify(*age.age_ns);
    evidence.set_freshness(freshness);
    evidence.add_reason(freshness_reason(freshness));
  } else {
    evidence.set_freshness(core::Freshness::Unknown);
    evidence.add_reason(core::ReasonCode::FreshnessUnknown, "observation age is unknown");
  }

  // Hops inherit the freshness of the exchange they belong to.
  for (model::HopObservation& hop : record.hops) {
    if (hop.evidence.state() == core::EvidenceState::Observed) {
      hop.evidence.set_freshness(evidence.freshness());
      hop.evidence.set_confidence(evidence.confidence());
    }
  }

  LATOBS_TRY_STATUS(derive_dwells(record, report));

  if (fatal) {
    verdict = AdmitVerdict::Rejected;
    return core::ok_status();
  }

  if (record.generation != path.value()->generation) {
    evidence.add_reason(core::ReasonCode::GenerationSuperseded,
                        "record generation differs from the path generation");
    verdict = AdmitVerdict::AcceptedHistorical;
  }
  if (catalog_.is_superseded(record.generation)) {
    evidence.add_reason(core::ReasonCode::GenerationSuperseded, record.generation.to_hex());
    verdict = AdmitVerdict::AcceptedHistorical;
  }

  LATOBS_TRY_STATUS(apply_session_fence(record, verdict, evidence, report));

  // --- source revision / authority fence ---
  if (record.source_revision > source.value()->revision) {
    evidence.set_state(core::EvidenceState::Conflicting);
    evidence.add_reason(core::ReasonCode::RevisionChanged,
                        "record revision is ahead of the registered source revision");
    verdict = AdmitVerdict::Rejected;
  } else if (record.source_revision < source.value()->revision) {
    evidence.add_reason(core::ReasonCode::RevisionChanged,
                        "record was produced by an older source revision");
    verdict = AdmitVerdict::AcceptedHistorical;
  }

  if (verdict == AdmitVerdict::AcceptedCurrent && !evidence.usable()) {
    verdict = AdmitVerdict::AcceptedHistorical;
  }
  return core::ok_status();
}

Status IngestGate::apply_session_fence(model::MeasurementRecord& record, AdmitVerdict& verdict,
                                      core::Evidence& evidence, IngestReport& report) {
  SourceFenceState& fence = fences_[record.source];
  const std::pair<EpochId, IncarnationId> key{record.epoch, record.incarnation};
  auto session = fence.sessions.find(key);
  if (session == fence.sessions.end()) {
    SessionState state;
    state.ordinal = fence.next_ordinal++;
    state.source_revision = record.source_revision;
    auto inserted = fence.sessions.emplace(key, std::move(state));
    session = inserted.first;
    if (fence.current_ordinal != 0) {
      evidence.add_reason(record.epoch != fence.current_epoch ? core::ReasonCode::EpochAdvanced
                                                             : core::ReasonCode::IncarnationChanged,
                          record.epoch.to_hex());
    }
    fence.current_epoch = record.epoch;
    fence.current_incarnation = record.incarnation;
    fence.current_ordinal = session->second.ordinal;

    // Bounded session memory: the oldest non current session is evicted and the
    // eviction is reported rather than hidden.
    while (fence.sessions.size() > policy_.limits.max_sessions_per_source) {
      auto oldest = fence.sessions.end();
      for (auto entry = fence.sessions.begin(); entry != fence.sessions.end(); ++entry) {
        if (entry->second.ordinal == fence.current_ordinal) continue;
        if (oldest == fence.sessions.end() || entry->second.ordinal < oldest->second.ordinal) {
          oldest = entry;
        }
      }
      if (oldest == fence.sessions.end()) break;
      evidence.add_reason(core::ReasonCode::SessionEvicted, oldest->first.first.to_hex());
      fence.sessions.erase(oldest);
      ++fence.evicted_sessions;
    }
  } else if (session->second.ordinal < fence.current_ordinal) {
    evidence.add_reason(record.epoch != fence.current_epoch ? core::ReasonCode::EpochReplayed
                                                           : core::ReasonCode::IncarnationReplayed,
                        record.epoch.to_hex());
    verdict = AdmitVerdict::AcceptedHistorical;
  }

  SessionState& state = session->second;
  if (state.source_revision.valid() && state.source_revision != record.source_revision) {
    const bool advanced = record.source_revision.value() > state.source_revision.value();
    evidence.add_reason(core::ReasonCode::RevisionChanged,
                        advanced ? "session revision advanced" : "session revision moved backwards");
    if (!advanced) verdict = AdmitVerdict::AcceptedHistorical;
  }
  if (!state.source_revision.valid() ||
      record.source_revision.value() > state.source_revision.value()) {
    state.source_revision = record.source_revision;
  }

  const std::uint64_t sequence = record.sequence.value();
  bool replayed = false;
  if (!state.has_last_sequence) {
    state.has_last_sequence = true;
    state.last_sequence = record.sequence;
  } else if (record.sequence > state.last_sequence) {
    const std::uint64_t gap = sequence - state.last_sequence.value() - 1ULL;
    if (gap != 0) {
      report.sequence_gaps += gap;
      evidence.add_reason(core::ReasonCode::SequenceGap, std::to_string(gap));
    }
    state.last_sequence = record.sequence;
  } else if (record.sequence == state.last_sequence ||
             state.recent_sequences.count(sequence) != 0) {
    replayed = true;
  } else {
    evidence.add_reason(core::ReasonCode::SequenceReordered, std::to_string(sequence));
    verdict = AdmitVerdict::AcceptedHistorical;
    ++report.reordered;
  }

  if (replayed) {
    ++report.replays;
    evidence.set_state(core::EvidenceState::Conflicting);
    evidence.set_freshness(core::Freshness::Unknown);
    evidence.add_reason(core::ReasonCode::SequenceReplay, std::to_string(sequence));
    verdict = AdmitVerdict::Rejected;
    ++state.rejected;
    return core::ok_status();
  }

  // Bounded replay window: only the most recent sequences are remembered.
  state.recent_sequences.insert(sequence);
  state.recent_order.push_back(sequence);
  while (state.recent_order.size() > policy_.limits.sequence_replay_window) {
    state.recent_sequences.erase(state.recent_order.front());
    state.recent_order.pop_front();
  }

  switch (verdict) {
    case AdmitVerdict::AcceptedCurrent: ++state.accepted_current; break;
    case AdmitVerdict::AcceptedHistorical: ++state.accepted_historical; break;
    case AdmitVerdict::Rejected: ++state.rejected; break;
  }
  return core::ok_status();
}

Status IngestGate::derive_dwells(model::MeasurementRecord& record, IngestReport& report) {
  for (model::HopObservation& hop : record.hops) {
    const Result<const model::HopDef*> definition = catalog_.hop(hop.hop);
    if (!definition) continue;

    if (hop.entry.domain == hop.exit.domain) {
      if (hop.exit.ns < hop.entry.ns) {
        hop.dwell_ns.reset();
        hop.evidence = core::Evidence::merge(
            hop.evidence,
            core::Evidence::conflicting(core::ReasonCode::HopOutOfOrder, hop.hop.to_hex()));
      } else {
        hop.dwell_ns = hop.exit.ns - hop.entry.ns;
        hop.dwell_uncertainty_ns = 0;
        ++report.hop_dwells_computed;
      }
    } else {
      const Result<model::ClockComparability> comparability = clocks_.compare(
          hop.entry.domain, hop.exit.domain, record.stamp.received_at, record.generation,
          record.epoch, record.incarnation);
      if (!comparability.has_value()) {
        hop.dwell_ns.reset();
        hop.evidence = core::Evidence::merge(
            hop.evidence,
            core::Evidence::refused(core::ReasonCode::IncomparableClocks, hop.hop.to_hex()));
      } else if (!comparability.value().comparable) {
        // Incomparable clocks: no dwell is fabricated. The hop stays unknown and
        // the evidence records exactly which condition blocked the comparison.
        hop.dwell_ns.reset();
        hop.evidence = core::Evidence::merge(hop.evidence, comparability.value().evidence);
      } else {
        const Nanos entry_reference = hop.entry.ns - comparability.value().offset_first_ns;
        const Nanos exit_reference = hop.exit.ns - comparability.value().offset_second_ns;
        if (exit_reference < entry_reference) {
          hop.dwell_ns.reset();
          hop.evidence = core::Evidence::merge(
              hop.evidence,
              core::Evidence::conflicting(core::ReasonCode::HopOutOfOrder, hop.hop.to_hex()));
        } else {
          hop.dwell_ns = exit_reference - entry_reference;
          hop.dwell_uncertainty_ns = comparability.value().uncertainty_ns;
          hop.evidence = core::Evidence::merge(hop.evidence, comparability.value().evidence);
          ++report.hop_dwells_computed;
        }
      }
    }
    if (!hop.dwell_ns.has_value()) ++report.hop_dwells_unknown;

    // Queue residency is only attributed when the queue declares the semantics.
    if (definition.value()->has_queue()) {
      const QueueId declared_queue = definition.value()->queue;
      if (!hop.queue.valid() || hop.queue != declared_queue) {
        hop.queue_dwell_ns.reset();
        hop.evidence = core::Evidence::merge(
            hop.evidence, core::Evidence::unsupported(core::ReasonCode::QueueSemanticsUndeclared,
                                                      hop.hop.to_hex()));
      } else {
        const Result<const model::QueueDef*> queue = catalog_.queue(hop.queue);
        if (!queue) {
          hop.queue_dwell_ns.reset();
          hop.evidence = core::Evidence::merge(
              hop.evidence,
              core::Evidence::unsupported(core::ReasonCode::UnknownQueue, hop.queue.to_hex()));
        } else if (!model::has_semantics(queue.value()->semantics,
                                         model::SemanticsProfile::QueueDwell)) {
          hop.queue_dwell_ns.reset();
          hop.evidence = core::Evidence::merge(
              hop.evidence, core::Evidence::unsupported(core::ReasonCode::QueueSemanticsUndeclared,
                                                        queue.value()->name.str()));
        } else if (!hop.queue_entry.valid() || !hop.queue_exit.valid()) {
          hop.queue_dwell_ns.reset();
          hop.evidence = core::Evidence::merge(
              hop.evidence, core::Evidence::unsupported(core::ReasonCode::HopUnsupported,
                                                        "queue residency timestamps are absent"));
        } else if (hop.queue_entry.domain == hop.queue_exit.domain) {
          if (hop.queue_exit.ns < hop.queue_entry.ns) {
            hop.queue_dwell_ns.reset();
            hop.evidence = core::Evidence::merge(
                hop.evidence, core::Evidence::conflicting(core::ReasonCode::HopOutOfOrder,
                                                          queue.value()->name.str()));
          } else {
            hop.queue_dwell_ns = hop.queue_exit.ns - hop.queue_entry.ns;
            hop.queue_dwell_uncertainty_ns = 0;
            ++report.queue_dwells_computed;
          }
        } else {
          const Result<model::ClockComparability> comparability = clocks_.compare(
              hop.queue_entry.domain, hop.queue_exit.domain, record.stamp.received_at,
              record.generation, record.epoch, record.incarnation);
          if (!comparability.has_value() || !comparability.value().comparable) {
            hop.queue_dwell_ns.reset();
            hop.evidence = core::Evidence::merge(
                hop.evidence,
                comparability.has_value()
                    ? comparability.value().evidence
                    : core::Evidence::refused(core::ReasonCode::IncomparableClocks,
                                              queue.value()->name.str()));
          } else {
            const Nanos entry_reference =
                hop.queue_entry.ns - comparability.value().offset_first_ns;
            const Nanos exit_reference =
                hop.queue_exit.ns - comparability.value().offset_second_ns;
            if (exit_reference < entry_reference) {
              hop.queue_dwell_ns.reset();
              hop.evidence = core::Evidence::merge(
                  hop.evidence, core::Evidence::conflicting(core::ReasonCode::HopOutOfOrder,
                                                            queue.value()->name.str()));
            } else {
              hop.queue_dwell_ns = exit_reference - entry_reference;
              hop.queue_dwell_uncertainty_ns = comparability.value().uncertainty_ns;
              hop.evidence = core::Evidence::merge(hop.evidence, comparability.value().evidence);
              ++report.queue_dwells_computed;
            }
          }
        }
      }
    } else if (hop.queue.valid()) {
      hop.queue_dwell_ns.reset();
      hop.evidence = core::Evidence::merge(
          hop.evidence, core::Evidence::unsupported(core::ReasonCode::QueueSemanticsUndeclared,
                                                    hop.queue.to_hex()));
    }
    if (!hop.queue_dwell_ns.has_value()) ++report.queue_dwells_unknown;
  }
  return core::ok_status();
}

void IngestGate::record_reason(IngestReport& report, core::ReasonCode code, std::uint64_t count) {
  report.reason_counts[code] += count;
}

void IngestGate::restore_fence(SourceId source, SourceFenceState state) {
  fences_[source] = std::move(state);
}

}  // namespace latobs::ingest
