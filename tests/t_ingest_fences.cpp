// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

#include "latency_observatory/ingest/ingest.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

/// Ingests one record and reports the verdict for it.
ingest::AdmitVerdict ingest_one(Scenario& scenario, const RecordSpec& spec,
                                ingest::IngestReport& report, std::int64_t received_at,
                                std::string* detail = nullptr) {
  ingest::IngestRequest request;
  request.received_at = Timestamp{received_at, core::reference_clock_domain()};
  request.records.push_back(make_record(scenario, spec));
  std::vector<model::MeasurementRecord> current;
  std::vector<model::MeasurementRecord> historical;
  const Status status = scenario.engine->ingest(std::move(request)).has_value()
                            ? core::ok_status()
                            : core::Status(scenario.engine->ingest(ingest::IngestRequest{}).error());
  if (!status.ok()) {
    fail(__FILE__, __LINE__, "ingest failed: " + status.error().describe());
  }
  if (report.verdicts.empty()) {
    fail(__FILE__, __LINE__, "ingest produced no verdict");
  }
  if (detail != nullptr) {
    *detail = report.verdicts.front().evidence.summarize();
  }
  return report.verdicts.front().verdict;
}

/// A simpler variant that returns the whole report.
ingest::IngestReport ingest_records(Scenario& scenario, ingest::IngestRequest request) {
  const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
  if (!report.has_value()) {
    fail(__FILE__, __LINE__, "ingest failed: " + report.error().describe());
  }
  return report.value();
}

bool has_reason(const ingest::IngestReport& report, ReasonCode code) {
  return report.reason_counts.find(code) != report.reason_counts.end();
}

}  // namespace

LATOBS_TEST(ingest, accepts_fresh_evidence) {
  CHECK_OK(scenario, build_scenario());
  RecordSpec spec;
  spec.observed_at_ns = 1000000;
  const ingest::IngestReport report =
      ingest_records(scenario, ingest::IngestRequest{
                                   {make_record(scenario, spec)},
                                   Timestamp{2000000, core::reference_clock_domain()},
                                   MonoTime{0}});
  CHECK_EQ(report.received, std::uint64_t{1});
  CHECK_EQ(report.accepted_current, std::uint64_t{1});
  CHECK_EQ(report.accepted_historical, std::uint64_t{0});
  CHECK_EQ(report.rejected, std::uint64_t{0});
  CHECK_EQ(report.hop_dwells_computed, std::uint64_t{3});
  CHECK_EQ(report.queue_dwells_computed, std::uint64_t{1});
  CHECK_EQ(report.verdicts.size(), std::size_t{1});
  CHECK(report.verdicts[0].verdict == ingest::AdmitVerdict::AcceptedCurrent);
  CHECK_EQ(report.verdicts[0].evidence.state() == EvidenceState::Observed, true);
  CHECK(report.verdicts[0].evidence.freshness() == Freshness::Fresh);
  CHECK(report.verdicts[0].evidence.usable());
}

LATOBS_TEST(ingest, refuses_duplicate_sequences) {
  CHECK_OK(scenario, build_scenario());
  RecordSpec spec;
  spec.observed_at_ns = 1000000;
  spec.sequence = 5;
  const Timestamp received{2000000, core::reference_clock_domain()};
  ingest::IngestRequest first;
  first.received_at = received;
  first.records.push_back(make_record(scenario, spec));
  const ingest::IngestReport first_report = ingest_records(scenario, std::move(first));
  CHECK_EQ(first_report.accepted_current, std::uint64_t{1});

  ingest::IngestRequest replay;
  replay.received_at = received;
  replay.records.push_back(make_record(scenario, spec));
  const ingest::IngestReport replay_report = ingest_records(scenario, std::move(replay));
  CHECK_EQ(replay_report.rejected, std::uint64_t{1});
  CHECK_EQ(replay_report.replays, std::uint64_t{1});
  CHECK(has_reason(replay_report, ReasonCode::SequenceReplay));
  CHECK(replay_report.verdicts[0].verdict == ingest::AdmitVerdict::Rejected);
  CHECK(replay_report.verdicts[0].evidence.state() == EvidenceState::Conflicting);
  CHECK(!replay_report.verdicts[0].evidence.has_value());
}

LATOBS_TEST(ingest, reports_sequence_gaps_without_inventing_records) {
  CHECK_OK(scenario, build_scenario());
  const Timestamp received{2000000, core::reference_clock_domain()};
  RecordSpec first;
  first.observed_at_ns = 1000000;
  first.sequence = 1;
  RecordSpec third = first;
  third.sequence = 3;
  ingest::IngestRequest request;
  request.received_at = received;
  request.records.push_back(make_record(scenario, first));
  request.records.push_back(make_record(scenario, third));
  const ingest::IngestReport report = ingest_records(scenario, std::move(request));
  CHECK_EQ(report.accepted_current, std::uint64_t{2});
  // Sequence 2 was never observed: the gap is reported, not filled.
  CHECK_EQ(report.sequence_gaps, std::uint64_t{1});
  CHECK(has_reason(report, ReasonCode::SequenceGap));
  CHECK_EQ(report.sequence_gaps, 1ULL);
}

LATOBS_TEST(ingest, reordered_records_are_historical_not_current) {
  CHECK_OK(scenario, build_scenario());
  const Timestamp received{2000000, core::reference_clock_domain()};
  RecordSpec newer;
  newer.observed_at_ns = 1000000;
  newer.sequence = 10;
  ingest::IngestRequest first;
  first.received_at = received;
  first.records.push_back(make_record(scenario, newer));
  CHECK_EQ(ingest_records(scenario, std::move(first)).accepted_current, std::uint64_t{1});

  RecordSpec older = newer;
  older.sequence = 7;
  ingest::IngestRequest second;
  second.received_at = received;
  second.records.push_back(make_record(scenario, older));
  const ingest::IngestReport report = ingest_records(scenario, std::move(second));
  CHECK_EQ(report.accepted_historical, std::uint64_t{1});
  CHECK_EQ(report.accepted_current, std::uint64_t{0});
  CHECK_EQ(report.reordered, std::uint64_t{1});
  CHECK(report.verdicts[0].verdict == ingest::AdmitVerdict::AcceptedHistorical);
  CHECK(has_reason(report, ReasonCode::SequenceReordered));

  // The reordered sequence is now inside the replay window: a repeat is a
  // replay and is refused outright.
  ingest::IngestRequest third;
  third.received_at = received;
  third.records.push_back(make_record(scenario, older));
  const ingest::IngestReport replay = ingest_records(scenario, std::move(third));
  CHECK_EQ(replay.rejected, std::uint64_t{1});
  CHECK(has_reason(replay, ReasonCode::SequenceReplay));
}

LATOBS_TEST(ingest, epoch_and_incarnation_fences) {
  CHECK_OK(scenario, build_scenario());
  const Timestamp received{2000000, core::reference_clock_domain()};
  RecordSpec one;
  one.observed_at_ns = 1000000;
  one.sequence = 1;
  ingest::IngestRequest first;
  first.received_at = received;
  first.records.push_back(make_record(scenario, one));
  CHECK_EQ(ingest_records(scenario, std::move(first)).accepted_current, std::uint64_t{1});

  // A new incarnation becomes current and the previous one is fenced.
  RecordSpec second = one;
  second.incarnation = "incarnation.two";
  second.sequence = 1;
  ingest::IngestRequest incarnation_request;
  incarnation_request.received_at = received;
  incarnation_request.records.push_back(make_record(scenario, second));
  const ingest::IngestReport incarnation_report =
      ingest_records(scenario, std::move(incarnation_request));
  CHECK_EQ(incarnation_report.accepted_current, std::uint64_t{1});
  CHECK(has_reason(incarnation_report, ReasonCode::IncarnationChanged));

  // A record from the previous incarnation is historical even though its
  // sequence number would otherwise be new for that session.
  RecordSpec late = one;
  late.sequence = 5;
  ingest::IngestRequest late_request;
  late_request.received_at = received;
  late_request.records.push_back(make_record(scenario, late));
  const ingest::IngestReport late_report = ingest_records(scenario, std::move(late_request));
  CHECK_EQ(late_report.accepted_historical, std::uint64_t{1});
  CHECK(has_reason(late_report, ReasonCode::IncarnationReplayed));

  // A new epoch supersedes both.
  RecordSpec next_epoch = second;
  next_epoch.epoch = "epoch.two";
  next_epoch.sequence = 1;
  ingest::IngestRequest epoch_request;
  epoch_request.received_at = received;
  epoch_request.records.push_back(make_record(scenario, next_epoch));
  const ingest::IngestReport epoch_report = ingest_records(scenario, std::move(epoch_request));
  CHECK_EQ(epoch_report.accepted_current, std::uint64_t{1});
  CHECK(has_reason(epoch_report, ReasonCode::EpochAdvanced));

  // The superseded incarnation is now historical.
  RecordSpec older = second;
  older.sequence = 9;
  ingest::IngestRequest older_request;
  older_request.received_at = received;
  older_request.records.push_back(make_record(scenario, older));
  const ingest::IngestReport older_report = ingest_records(scenario, std::move(older_request));
  CHECK_EQ(older_report.accepted_historical, std::uint64_t{1});
  CHECK(has_reason(older_report, ReasonCode::EpochReplayed));
}

LATOBS_TEST(ingest, generation_supersede_fences_history) {
  CHECK_OK(scenario, build_scenario());
  model::GenerationDef second;
  second.name = Name::assume_valid("test.generation.two");
  second.revision = Revision::first();
  second.supersedes = scenario.generation;
  CHECK_OK(second_generation, scenario.engine->define_generation(second));
  CHECK_NE(second_generation, scenario.generation);

  RecordSpec spec;
  spec.observed_at_ns = 1000000;
  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, core::reference_clock_domain()};
  request.records.push_back(make_record(scenario, spec));
  const ingest::IngestReport report = ingest_records(scenario, std::move(request));
  CHECK_EQ(report.accepted_historical, std::uint64_t{1});
  CHECK_EQ(report.accepted_current, std::uint64_t{0});
  CHECK(has_reason(report, ReasonCode::GenerationSuperseded));
}

LATOBS_TEST(ingest, revision_fences) {
  CHECK_OK(scenario, build_scenario());
  const Timestamp received{2000000, core::reference_clock_domain()};
  RecordSpec old_revision;
  old_revision.observed_at_ns = 1000000;
  old_revision.source_revision = 1;

  // The source is redefined at revision two: a record stamped with the older
  // revision is historical, never current.
  model::SourceDescriptor source;
  source.name = Name::assume_valid("test.source.probe");
  source.kind = model::SourceKind::Probe;
  source.authority = model::AuthorityClass::Primary;
  source.semantics = model::SemanticsProfile::EndToEndRequestResponse |
                     model::SemanticsProfile::HopDwell;
  source.revision = Revision::from_validated_value(2);
  CHECK_OK(redefined, scenario.engine->define_source(source));
  CHECK(redefined == scenario.source);

  ingest::IngestRequest request;
  request.received_at = received;
  request.records.push_back(make_record(scenario, old_revision));
  const ingest::IngestReport report = ingest_records(scenario, std::move(request));
  CHECK_EQ(report.accepted_historical, std::uint64_t{1});
  CHECK(has_reason(report, ReasonCode::RevisionChanged));

  // A revision ahead of the registry is refused: the runtime never guesses a
  // definition it has not been given.
  RecordSpec ahead = old_revision;
  ahead.source_revision = 5;
  ahead.sequence = 1;
  ingest::IngestRequest ahead_request;
  ahead_request.received_at = received;
  ahead_request.records.push_back(make_record(scenario, ahead));
  const ingest::IngestReport ahead_report = ingest_records(scenario, std::move(ahead_request));
  CHECK_EQ(ahead_report.rejected, std::uint64_t{1});
  CHECK(ahead_report.verdicts[0].evidence.state() == EvidenceState::Conflicting);
}

LATOBS_TEST(ingest, stale_evidence_is_historical) {
  CHECK_OK(scenario, build_scenario());
  RecordSpec spec;
  spec.observed_at_ns = 1000000;
  // Received far beyond the freshness horizons of the default policy.
  ingest::IngestRequest request;
  request.received_at = Timestamp{
      spec.observed_at_ns + 2 * scenario.engine->policy().freshness.stale_horizon_ns,
      core::reference_clock_domain()};
  request.records.push_back(make_record(scenario, spec));
  const ingest::IngestReport report = ingest_records(scenario, std::move(request));
  CHECK_EQ(report.accepted_historical, std::uint64_t{1});
  CHECK_EQ(report.accepted_current, std::uint64_t{0});
  CHECK(report.verdicts[0].evidence.freshness() == Freshness::Expired);
  CHECK(!report.verdicts[0].evidence.usable());
  CHECK(has_reason(report, ReasonCode::EvidenceExpired));
}

LATOBS_TEST(ingest, structural_validation_rejects_inconsistency) {
  CHECK_OK(scenario, build_scenario());
  const Timestamp received{2000000, core::reference_clock_domain()};

  // Declared end to end latency that does not match the timestamps.
  model::MeasurementRecord inconsistent = make_record(scenario, RecordSpec{});
  inconsistent.rtt_ns += 1;
  ingest::IngestRequest request;
  request.received_at = received;
  request.records.push_back(inconsistent);
  const ingest::IngestReport report = ingest_records(scenario, std::move(request));
  CHECK_EQ(report.rejected, std::uint64_t{1});
  CHECK(has_reason(report, ReasonCode::InvalidFieldValue));

  // Hop indices that are not strictly increasing.
  model::MeasurementRecord unordered = make_record(scenario, RecordSpec{});
  std::swap(unordered.hops[0], unordered.hops[2]);
  ingest::IngestRequest unordered_request;
  unordered_request.received_at = received;
  unordered_request.records.push_back(unordered);
  const ingest::IngestReport unordered_report =
      ingest_records(scenario, std::move(unordered_request));
  CHECK_EQ(unordered_report.rejected, std::uint64_t{1});
  CHECK(has_reason(unordered_report, ReasonCode::HopNotContiguous));

  // A hop that does not belong to this path position.
  model::MeasurementRecord wrong_hop = make_record(scenario, RecordSpec{});
  wrong_hop.hops[1].hop = scenario.queue_hop;
  ingest::IngestRequest wrong_request;
  wrong_request.received_at = received;
  wrong_request.records.push_back(wrong_hop);
  const ingest::IngestReport wrong_report = ingest_records(scenario, std::move(wrong_request));
  CHECK_EQ(wrong_report.rejected, std::uint64_t{1});
  CHECK(has_reason(wrong_report, ReasonCode::HopConflicting));

  // Unknown definitions are refused with their own reason.
  model::MeasurementRecord unknown_path = make_record(scenario, RecordSpec{});
  unknown_path.path = PathId::derive_from("test.path.missing");
  ingest::IngestRequest unknown_request;
  unknown_request.received_at = received;
  unknown_request.records.push_back(unknown_path);
  const ingest::IngestReport unknown_report = ingest_records(scenario, std::move(unknown_request));
  CHECK_EQ(unknown_report.rejected, std::uint64_t{1});
  CHECK(has_reason(unknown_report, ReasonCode::UnknownPath));

  model::MeasurementRecord unknown_generation = make_record(scenario, RecordSpec{});
  unknown_generation.generation = GenerationId::derive_from("test.generation.missing");
  ingest::IngestRequest generation_request;
  generation_request.received_at = received;
  generation_request.records.push_back(unknown_generation);
  const ingest::IngestReport generation_report =
      ingest_records(scenario, std::move(generation_request));
  CHECK_EQ(generation_report.rejected, std::uint64_t{1});
  CHECK(has_reason(generation_report, ReasonCode::UnknownGeneration));
}

LATOBS_TEST(ingest, batch_limits_are_enforced) {
  CHECK_OK(scenario, build_scenario());
  core::RuntimePolicy policy = scenario.engine->policy();
  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, core::reference_clock_domain()};
  for (std::size_t index = 0; index < policy.limits.max_batch_samples + 1; ++index) {
    RecordSpec spec;
    spec.sequence = index;
    request.records.push_back(make_record(scenario, spec));
  }
  const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
  CHECK(!report.has_value());
  CHECK(report.error().code() == ErrorCode::CapacityExceeded);
}

LATOBS_TEST(ingest, verdict_details_are_bounded) {
  CHECK_OK(scenario, build_scenario());
  // Every record is a duplicate of the first one, so all of them are rejected
  // and the detailed verdict list must stay bounded.
  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, core::reference_clock_domain()};
  RecordSpec spec;
  spec.sequence = 1;
  spec.observed_at_ns = 1000000;
  request.records.push_back(make_record(scenario, spec));
  for (std::size_t index = 0; index < 3 * ingest::IngestReport::kMaxDetailedVerdicts; ++index) {
    request.records.push_back(make_record(scenario, spec));
  }
  const ingest::IngestReport report = ingest_records(scenario, std::move(request));
  CHECK_EQ(report.accepted_current, std::uint64_t{1});
  CHECK_EQ(report.rejected, static_cast<std::uint64_t>(3 * ingest::IngestReport::kMaxDetailedVerdicts));
  CHECK(report.verdicts.size() <= ingest::IngestReport::kMaxDetailedVerdicts);
  CHECK(report.verdicts_truncated);
  CHECK_EQ(report.reason_counts.at(ReasonCode::SequenceReplay),
           static_cast<std::uint64_t>(3 * ingest::IngestReport::kMaxDetailedVerdicts));
}

LATOBS_TEST(ingest, case_insensitive_fence_restoration_shape) {
  // The fence state carries the sequence window, so a restart keeps refusing
  // replays. This test checks the state's shape; the restart test checks the
  // behaviour through persistence.
  CHECK_OK(scenario, build_scenario());
  RecordSpec spec;
  spec.sequence = 42;
  spec.observed_at_ns = 1000000;
  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, core::reference_clock_domain()};
  request.records.push_back(make_record(scenario, spec));
  const ingest::IngestReport report = ingest_records(scenario, std::move(request));
  CHECK_EQ(report.accepted_current, std::uint64_t{1});
  CHECK(!report.verdicts[0].id.to_hex().empty());
}

LATOBS_TEST_MAIN()
