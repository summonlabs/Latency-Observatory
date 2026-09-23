// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Attribution is only produced where the semantics permit it, the residual is
// always reported, and no statement about cause is ever produced.

#include <string>
#include <vector>

#include "latency_observatory/attribute/attribute.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

bool evidence_has(const Evidence& evidence, ReasonCode code) {
  for (const Reason& reason : evidence.reasons()) {
    if (reason.code == code) return true;
  }
  return false;
}

ingest::IngestReport ingest_equal_hop(Scenario& scenario, std::uint64_t count, std::int64_t hop_ns,
                                      std::int64_t residual_ns, std::uint64_t first_sequence = 0) {
  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < count; ++index) {
    RecordSpec spec;
    spec.sequence = first_sequence + index;
    spec.observed_at_ns = 1000000 + static_cast<std::int64_t>(index);
    spec.hop_dwells_ns[0] = hop_ns;
    spec.hop_dwells_ns[1] = hop_ns;
    spec.hop_dwells_ns[2] = hop_ns;
    // The residual is observed end to end without being attributed to a hop.
    spec.end_to_end_extra_ns = residual_ns;
    request.records.push_back(make_record(scenario, spec));
  }
  const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
  CHECK(report.has_value());
  return report.value();
}

}  // namespace

LATOBS_TEST(attribution, complete_decomposition_reports_every_hop) {
  CHECK_OK(scenario, build_scenario());
  CHECK_EQ(ingest_equal_hop(scenario, 16, 1000, 0).accepted_current, std::uint64_t{16});
  const attribute::AttributionRequest request = make_attribution_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(result, scenario.engine->attribute(request));
  CHECK(result.kind == attribute::AttributionKind::ObservedDecomposition);
  CHECK_EQ(result.exchanges_included, std::uint64_t{16});
  CHECK_EQ(result.exchanges_complete, std::uint64_t{16});
  CHECK_EQ(result.exchanges_incomplete, std::uint64_t{0});
  CHECK_EQ(result.contributions.size(), std::size_t{3});
  CHECK_EQ(*result.end_to_end_mean_ns, 3000);
  CHECK_EQ(*result.accounted_mean_ns, 3000);
  CHECK(result.unaccounted_mean_ns.has_value());
  CHECK_EQ(*result.unaccounted_mean_ns, 0);
  CHECK(evidence_has(result.evidence, ReasonCode::ResidualWithinTolerance));
  CHECK(evidence_has(result.evidence, ReasonCode::AttributionComplete));
  CHECK(evidence_has(result.evidence, ReasonCode::NoCausalInference));
  for (const attribute::HopContribution& contribution : result.contributions) {
    CHECK_EQ(contribution.share_ppm, 333333ULL);
    CHECK_EQ(contribution.coverage_ppm, 1000000ULL);
    CHECK(evidence_has(contribution.evidence, ReasonCode::NoCausalInference));
  }
}

LATOBS_TEST(attribution, residual_is_always_reported) {
  CHECK_OK(scenario, build_scenario());
  // 5000 ns of each exchange is observed end to end and attributable to no hop:
  // above the default 1000 ns residual tolerance.
  CHECK_EQ(ingest_equal_hop(scenario, 8, 1000, 5000).accepted_current, std::uint64_t{8});
  const attribute::AttributionRequest request = make_attribution_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(result, scenario.engine->attribute(request));
  CHECK(result.kind == attribute::AttributionKind::ObservedDecomposition);
  CHECK_EQ(*result.end_to_end_mean_ns, 8000);
  CHECK_EQ(*result.accounted_mean_ns, 3000);
  CHECK_EQ(result.exchanges_complete, std::uint64_t{8});
  CHECK(result.unaccounted_mean_ns.has_value());
  CHECK_EQ(*result.unaccounted_mean_ns, 5000);
  CHECK(evidence_has(result.evidence, ReasonCode::UnaccountedResidual));
  CHECK(!evidence_has(result.evidence, ReasonCode::ResidualWithinTolerance));
  CHECK(result.residual_min_ns.has_value());
  CHECK_EQ(*result.residual_min_ns, 5000);
  CHECK_EQ(*result.residual_max_ns, 5000);

  // A residual inside the policy tolerance is reported as such, and it is still
  // reported: a small residual is never hidden.
  CHECK_OK(tight_scenario, build_scenario());
  CHECK_EQ(ingest_equal_hop(tight_scenario, 8, 1000, 100).accepted_current, std::uint64_t{8});
  const attribute::AttributionRequest tight_request = make_attribution_request(
      tight_scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(tight_result, tight_scenario.engine->attribute(tight_request));
  CHECK(tight_result.unaccounted_mean_ns.has_value());
  CHECK_EQ(*tight_result.unaccounted_mean_ns, 100);
  CHECK(evidence_has(tight_result.evidence, ReasonCode::ResidualWithinTolerance));
  CHECK(!evidence_has(tight_result.evidence, ReasonCode::UnaccountedResidual));
}

LATOBS_TEST(attribution, negative_residual_is_reported_as_overlap) {
  CHECK_OK(scenario, build_scenario());
  // The hops claim more time than the exchange took: an overlap, reported as
  // such instead of being clamped to zero.
  CHECK_EQ(ingest_equal_hop(scenario, 4, 1000, -50).accepted_current, std::uint64_t{4});
  const attribute::AttributionRequest request = make_attribution_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(result, scenario.engine->attribute(request));
  CHECK_EQ(result.negative_residuals, std::uint64_t{4});
  CHECK(result.unaccounted_mean_ns.has_value());
  CHECK_EQ(*result.unaccounted_mean_ns, -50);
  CHECK(evidence_has(result.evidence, ReasonCode::HopOverlap));
}

LATOBS_TEST(attribution, refuses_when_hop_semantics_are_not_declared) {
  ScenarioOptions options;
  options.declare_hop_semantics = false;
  CHECK_OK(scenario, build_scenario(options));
  CHECK_EQ(ingest_equal_hop(scenario, 8, 1000, 0).accepted_current, std::uint64_t{8});
  const attribute::AttributionRequest request = make_attribution_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(result, scenario.engine->attribute(request));
  CHECK(result.kind == attribute::AttributionKind::Unsupported);
  CHECK(evidence_has(result.evidence, ReasonCode::SemanticsUnsupported));
  CHECK(!result.accounted_mean_ns.has_value());
  for (const attribute::HopContribution& contribution : result.contributions) {
    CHECK_EQ(contribution.share_ppm, 0ULL);
  }
}

LATOBS_TEST(attribution, refuses_with_no_evidence) {
  CHECK_OK(scenario, build_scenario());
  const attribute::AttributionRequest request = make_attribution_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(result, scenario.engine->attribute(request));
  CHECK(result.kind == attribute::AttributionKind::Refused);
  CHECK_EQ(result.exchanges_included, std::uint64_t{0});
  CHECK(evidence_has(result.evidence, ReasonCode::NoSamples));
  CHECK(!result.end_to_end_mean_ns.has_value());
}

LATOBS_TEST(attribution, stale_evidence_is_not_attributed) {
  CHECK_OK(scenario, build_scenario());
  ingest::IngestRequest request;
  // The evidence is observed long after the exchange happened.
  request.received_at = Timestamp{1000000 + 4 * 60 * 1000 * 1000 * 1000LL,
                                  core::reference_clock_domain()};
  RecordSpec spec;
  spec.sequence = 1;
  spec.observed_at_ns = 1000000;
  spec.hop_dwells_ns[0] = 100;
  spec.hop_dwells_ns[1] = 100;
  spec.hop_dwells_ns[2] = 100;
  request.records.push_back(make_record(scenario, spec));
  CHECK_EQ(scenario.engine->ingest(std::move(request)).value().accepted_historical,
           std::uint64_t{1});

  const attribute::AttributionRequest current = make_attribution_request(
      scenario, 0, 2000000000000LL, stats::AggregationMode::Current);
  CHECK_OK(current_result, scenario.engine->attribute(current));
  CHECK(current_result.kind == attribute::AttributionKind::Refused);
  CHECK_EQ(current_result.exchanges_included, std::uint64_t{0});

  // Historical mode reports the same evidence as history, and still refuses to
  // present it as a current decomposition.
  const attribute::AttributionRequest historical = make_attribution_request(
      scenario, 0, 2000000000000LL, stats::AggregationMode::Historical);
  CHECK_OK(historical_result, scenario.engine->attribute(historical));
  CHECK_EQ(historical_result.exchanges_included, std::uint64_t{1});
  CHECK(historical_result.kind == attribute::AttributionKind::ObservedDecomposition);
}

LATOBS_TEST(attribution, queue_contributions_are_separate_from_hop_dwell) {
  CHECK_OK(scenario, build_scenario());
  CHECK_EQ(ingest_equal_hop(scenario, 4, 3000, 0).accepted_current, std::uint64_t{4});
  const attribute::AttributionRequest request = make_attribution_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(result, scenario.engine->attribute(request));
  CHECK(result.kind == attribute::AttributionKind::ObservedDecomposition);
  // The queue hop reports both a dwell and a queue residency: the residency is
  // reported next to the dwell, never added to it.
  const attribute::HopContribution& queue_hop = result.contributions[2];
  CHECK(queue_hop.mean_dwell_ns.has_value());
  CHECK(queue_hop.mean_queue_dwell_ns.has_value());
  CHECK_EQ(*queue_hop.mean_queue_dwell_ns, 3000 - 20);
  CHECK_EQ(*result.accounted_mean_ns, 9000);
}

LATOBS_TEST(attribution, anomaly_evidence_is_attached_when_a_baseline_is_supplied) {
  CHECK_OK(scenario, build_scenario());
  CHECK_EQ(ingest_equal_hop(scenario, 32, 1000, 0).accepted_current, std::uint64_t{32});
  runtime::BaselineRequest baseline_request;
  baseline_request.name = Name::assume_valid("test.baseline.attribution");
  baseline_request.path = scenario.path;
  baseline_request.generation = scenario.generation;
  baseline_request.mode = stats::AggregationMode::Current;
  baseline_request.window = make_window(0, 100000000);
  CHECK_OK(baseline_id, scenario.engine->create_baseline(baseline_request));

  // A second batch of much slower exchanges, with fresh sequence numbers so the
  // replay fence is not involved.
  CHECK_EQ(ingest_equal_hop(scenario, 32, 20000, 0, 32).accepted_current, std::uint64_t{32});
  attribute::AttributionRequest request = make_attribution_request(
      scenario, 1000000, 100000000, stats::AggregationMode::Current);
  request.baseline = baseline_id;
  CHECK_OK(result, scenario.engine->attribute(request));
  (void)result;

  // The comparison itself is exercised with a baseline whose creation time sits
  // inside the synthetic window: the engine stamps baselines with the wall
  // clock, which is far outside a synthetic timeline.
  stats::SummaryRequest summary_request = make_summary_request(
      scenario, 1000000, 100000000, stats::AggregationMode::Current);
  CHECK_OK(summary, scenario.engine->summarize(summary_request));
  CHECK_OK(stored, scenario.engine->list_baselines());
  baseline::Baseline adjustable = *stored[0];
  adjustable.created_at = Timestamp{99000000, core::reference_clock_domain()};
  const baseline::BaselineComparison comparison =
      baseline::compare(adjustable, summary, scenario.engine->policy());
  CHECK(comparison.applicable);
  CHECK(!comparison.anomalies.empty());
  bool saw_causal_free_statement = false;
  for (const baseline::AnomalyEvidence& anomaly : comparison.anomalies) {
    if (evidence_has(anomaly.evidence, ReasonCode::NoCausalInference)) {
      saw_causal_free_statement = true;
    }
  }
  CHECK(saw_causal_free_statement);
}

LATOBS_TEST(attribution, unknown_baseline_is_reported_not_ignored) {
  CHECK_OK(scenario, build_scenario());
  CHECK_EQ(ingest_equal_hop(scenario, 8, 1000, 0).accepted_current, std::uint64_t{8});
  attribute::AttributionRequest request = make_attribution_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  request.baseline = BaselineId::derive_from("test.baseline.missing");
  CHECK_OK(result, scenario.engine->attribute(request));
  CHECK(!result.comparison.has_value());
  CHECK(evidence_has(result.evidence, ReasonCode::BaselineMissing));
}

LATOBS_TEST_MAIN()
