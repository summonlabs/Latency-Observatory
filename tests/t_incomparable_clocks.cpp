// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proof: incomparable clocks cannot produce a fabricated attribution. Every
// path that would require comparing readings across clock domains either has a
// comparable pair of domains or produces no value at all.

#include <string>
#include <vector>

#include "latency_observatory/runtime/engine.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

RecordSpec cross_domain_spec(std::uint64_t sequence, std::int64_t observed_at) {
  RecordSpec spec;
  spec.sequence = sequence;
  spec.observed_at_ns = observed_at;
  spec.hop_dwells_ns[0] = 100;
  spec.hop_dwells_ns[1] = 200;  // measured across two clock domains
  spec.hop_dwells_ns[2] = 300;
  spec.cross_domain_hop = true;
  return spec;
}

ingest::IngestReport ingest_cross_domain(Scenario& scenario, std::size_t count,
                                         std::int64_t received_at) {
  ingest::IngestRequest request;
  request.received_at = Timestamp{received_at, core::reference_clock_domain()};
  for (std::size_t index = 0; index < count; ++index) {
    request.records.push_back(
        make_record(scenario, cross_domain_spec(index, 1000000 + static_cast<std::int64_t>(index))));
  }
  const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
  CHECK(report.has_value());
  return report.value();
}

bool has_reason(const Evidence& evidence, ReasonCode code) {
  for (const Reason& reason : evidence.reasons()) {
    if (reason.code == code) return true;
  }
  return false;
}

/// Asserts that no derived quantity was produced anywhere in the result.
void expect_no_attribution(const attribute::AttributionResult& result) {
  CHECK(result.kind == attribute::AttributionKind::Refused ||
        result.kind == attribute::AttributionKind::Unsupported);
  CHECK(!result.accounted_mean_ns.has_value());
  CHECK(!result.unaccounted_mean_ns.has_value());
  for (const attribute::HopContribution& contribution : result.contributions) {
    CHECK_EQ(contribution.share_ppm, 0ULL);
  }
}

}  // namespace

LATOBS_TEST(clocks, unsynchronized_domains_yield_no_dwell_and_no_attribution) {
  CHECK_OK(scenario, build_scenario());
  const ingest::IngestReport report = ingest_cross_domain(scenario, 4, 2000000);
  CHECK_EQ(report.accepted_current, std::uint64_t{4});
  CHECK_EQ(report.hop_dwells_unknown, std::uint64_t{4});
  CHECK_EQ(report.hop_dwells_computed, std::uint64_t{8});

  const stats::SummaryRequest summary_request =
      make_summary_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(summary, scenario.engine->summarize(summary_request));
  // The end to end evidence is observed and reported.
  CHECK_EQ(summary.end_to_end.count, std::uint64_t{4});
  CHECK_EQ(*summary.end_to_end.mean_ns, 600);
  // The cross domain hop has no value at all: unknown, not zero.
  CHECK_EQ(summary.hops[1].dwell.count, std::uint64_t{0});
  CHECK(!summary.hops[1].dwell.mean_ns.has_value());
  CHECK_EQ(summary.hops[1].dwells_unsupported, std::uint64_t{4});
  CHECK_EQ(summary.hops[1].coverage_ppm, 0ULL);
  CHECK(has_reason(summary.hops[1].evidence, ReasonCode::IncomparableClocks) ||
        summary.hops[1].evidence.state() == EvidenceState::Refused ||
        summary.hops[1].evidence.state() == EvidenceState::Unknown);

  const attribute::AttributionRequest attribution_request =
      make_attribution_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(attribution, scenario.engine->attribute(attribution_request));
  expect_no_attribution(attribution);
  CHECK(has_reason(attribution.evidence, ReasonCode::IncomparableClocks));
  CHECK(has_reason(attribution.evidence, ReasonCode::NoCausalInference));
  CHECK_EQ(attribution.exchanges_complete, std::uint64_t{0});
}

LATOBS_TEST(clocks, unsynced_state_is_refused) {
  ScenarioOptions options;
  CHECK_OK(scenario, build_scenario(options));
  // A domain that reports itself as unsynchronized can never be compared.
  CHECK_OK_STATUS(
      scenario.engine->record_clock_sync(make_edge_sync(scenario, 1999000, 0, 100000000,
                                                        model::ClockSyncState::Unsynced)));
  ingest_cross_domain(scenario, 2, 2000000);
  const attribute::AttributionRequest request =
      make_attribution_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(attribution, scenario.engine->attribute(request));
  expect_no_attribution(attribution);
  CHECK(has_reason(attribution.evidence, ReasonCode::ClockUnsynced) ||
        has_reason(attribution.evidence, ReasonCode::IncomparableClocks));
}

LATOBS_TEST(clocks, expired_synchronization_is_not_usable) {
  ScenarioOptions options;
  CHECK_OK(scenario, build_scenario(options));
  // The report is valid for one microsecond and is read long after that.
  CHECK_OK_STATUS(scenario.engine->record_clock_sync(
      make_edge_sync(scenario, 1000, 0, 1000, model::ClockSyncState::Synchronized)));
  ingest_cross_domain(scenario, 2, 2000000);
  const attribute::AttributionRequest request =
      make_attribution_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(attribution, scenario.engine->attribute(request));
  expect_no_attribution(attribution);
  CHECK(has_reason(attribution.evidence, ReasonCode::IncomparableClocks));
}

LATOBS_TEST(clocks, generation_mismatch_is_conflicting) {
  CHECK_OK(scenario, build_scenario());
  model::ClockSync sync = make_edge_sync(scenario, 1000, 0, 100000000,
                                        model::ClockSyncState::Synchronized);
  // The synchronization report belongs to a different generation of the system.
  sync.generation = GenerationId::derive_from("test.generation.other");
  CHECK_OK_STATUS(scenario.engine->record_clock_sync(sync));
  ingest_cross_domain(scenario, 2, 2000000);
  const stats::SummaryRequest summary_request =
      make_summary_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(summary, scenario.engine->summarize(summary_request));
  CHECK(summary.hops[1].evidence.state() == EvidenceState::Conflicting);
  const attribute::AttributionRequest request =
      make_attribution_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(attribution, scenario.engine->attribute(request));
  expect_no_attribution(attribution);
}

LATOBS_TEST(clocks, uncertainty_above_the_ceiling_is_refused) {
  core::RuntimePolicy policy = core::default_policy();
  policy.comparability.max_uncertainty_ns = 500;
  ScenarioOptions options;
  options.policy = policy;
  CHECK_OK(scenario, build_scenario(options));
  CHECK_OK_STATUS(scenario.engine->record_clock_sync(
      make_edge_sync(scenario, 1000, 40000, 100000000, model::ClockSyncState::Synchronized)));
  ingest_cross_domain(scenario, 2, 2000000);
  const attribute::AttributionRequest request =
      make_attribution_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(attribution, scenario.engine->attribute(request));
  expect_no_attribution(attribution);
  CHECK(has_reason(attribution.evidence, ReasonCode::ClockUncertaintyExceeded) ||
        has_reason(attribution.evidence, ReasonCode::IncomparableClocks));
}

LATOBS_TEST(clocks, holdover_degrades_confidence_without_refusing) {
  CHECK_OK(scenario, build_scenario());
  CHECK_OK_STATUS(scenario.engine->record_clock_sync(
      make_edge_sync(scenario, 1999000, 0, 100000000, model::ClockSyncState::Holdover)));
  ingest_cross_domain(scenario, 4, 2000000);
  const stats::SummaryRequest summary_request =
      make_summary_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(summary, scenario.engine->summarize(summary_request));
  // Holdover is comparable, so the dwell exists, but it carries uncertainty and
  // a degraded confidence rather than an unqualified value.
  CHECK_EQ(summary.hops[1].dwell.count, std::uint64_t{4});
  CHECK_EQ(*summary.hops[1].dwell.mean_ns, 200);
  CHECK(summary.hops[1].evidence.confidence() != Confidence::High);
}

LATOBS_TEST(clocks, comparable_domains_produce_a_complete_decomposition) {
  CHECK_OK(scenario, build_scenario());
  CHECK_OK_STATUS(scenario.engine->record_clock_sync(
      make_edge_sync(scenario, 1999000, 0, 100000000, model::ClockSyncState::Synchronized)));
  const ingest::IngestReport report = ingest_cross_domain(scenario, 8, 2000000);
  CHECK_EQ(report.hop_dwells_computed, std::uint64_t{24});
  CHECK_EQ(report.hop_dwells_unknown, std::uint64_t{0});

  const attribute::AttributionRequest request =
      make_attribution_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(attribution, scenario.engine->attribute(request));
  CHECK(attribution.kind == attribute::AttributionKind::ObservedDecomposition);
  CHECK_EQ(attribution.exchanges_complete, std::uint64_t{8});
  CHECK(attribution.accounted_mean_ns.has_value());
  CHECK_EQ(*attribution.accounted_mean_ns, 600);
  CHECK(attribution.unaccounted_mean_ns.has_value());
  CHECK_EQ(*attribution.unaccounted_mean_ns, 0);
  // Shares are floor rounded per hop, so the sum can be at most one part per
  // million below a whole: it is never above it.
  std::uint64_t total_share = 0;
  for (const attribute::HopContribution& contribution : attribution.contributions) {
    total_share += contribution.share_ppm;
    CHECK(contribution.uncertainty_ns.has_value());
  }
  CHECK(total_share <= 1000000ULL);
  CHECK(total_share >= 999999ULL);
  CHECK(has_reason(attribution.evidence, ReasonCode::AttributionComplete));
}

LATOBS_TEST(clocks, incomparable_domains_never_produce_a_share) {
  // The strongest form of the proof: run the same evidence through both a
  // comparable and an incomparable configuration and check that the
  // incomparable one publishes nothing derived.
  CHECK_OK(incomparable_scenario, build_scenario());
  ingest_cross_domain(incomparable_scenario, 16, 2000000);
  const attribute::AttributionRequest request =
      make_attribution_request(incomparable_scenario, 0, 100000000,
                               stats::AggregationMode::Current);
  CHECK_OK(refused, incomparable_scenario.engine->attribute(request));
  expect_no_attribution(refused);

  CHECK_OK(comparable_scenario, build_scenario());
  CHECK_OK_STATUS(comparable_scenario.engine->record_clock_sync(
      make_edge_sync(comparable_scenario, 1999000, 0, 100000000,
                     model::ClockSyncState::Synchronized)));
  ingest_cross_domain(comparable_scenario, 16, 2000000);
  CHECK_OK(produced, comparable_scenario.engine->attribute(request));
  CHECK(produced.kind == attribute::AttributionKind::ObservedDecomposition);

  // Same evidence, same request: the only difference is clock comparability.
  CHECK_EQ(refused.exchanges_included, produced.exchanges_included);
  CHECK(!refused.accounted_mean_ns.has_value());
  CHECK(produced.accounted_mean_ns.has_value());
}

LATOBS_TEST_MAIN()
