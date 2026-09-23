// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proof: a hop that was never reported is unknown, never zero. The summary,
// the exports and the attribution must all say so explicitly.

#include <string>
#include <vector>

#include "latency_observatory/runtime/engine.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

ingest::IngestReport ingest_specs(Scenario& scenario, const std::vector<RecordSpec>& specs,
                                  std::int64_t received_at) {
  ingest::IngestRequest request;
  request.received_at = Timestamp{received_at, core::reference_clock_domain()};
  for (const RecordSpec& spec : specs) {
    request.records.push_back(make_record(scenario, spec));
  }
  const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
  CHECK(report.has_value());
  return report.value();
}

}  // namespace

LATOBS_TEST(missing_hops, absent_hops_are_unknown_not_zero) {
  CHECK_OK(scenario, build_scenario());
  std::vector<RecordSpec> specs;
  // Ten exchanges where the middle hop is never reported.
  for (std::uint64_t index = 0; index < 10; ++index) {
    RecordSpec spec;
    spec.sequence = index;
    spec.observed_at_ns = 1000000 + static_cast<std::int64_t>(index) * 1000;
    spec.hop_dwells_ns[0] = 100;
    spec.hop_dwells_ns[1] = -1;  // not reported at all
    spec.hop_dwells_ns[2] = 300;
    specs.push_back(spec);
  }
  const ingest::IngestReport report = ingest_specs(scenario, specs, 2000000);
  CHECK_EQ(report.accepted_current, std::uint64_t{10});

  const stats::SummaryRequest request = make_summary_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(summary, scenario.engine->summarize(request));
  CHECK_EQ(summary.exchanges_included, std::uint64_t{10});
  CHECK_EQ(summary.end_to_end.count, std::uint64_t{10});
  CHECK_EQ(*summary.end_to_end.mean_ns, 400);
  CHECK_EQ(summary.hops.size(), std::size_t{3});

  // Hop 0 and hop 2 were observed for every exchange.
  CHECK_EQ(summary.hops[0].dwell.count, std::uint64_t{10});
  CHECK_EQ(summary.hops[0].dwells_missing, std::uint64_t{0});
  CHECK_EQ(summary.hops[0].dwells_observed, std::uint64_t{10});
  CHECK_EQ(summary.hops[0].coverage_ppm, 1000000ULL);
  CHECK_EQ(summary.hops[2].dwell.count, std::uint64_t{10});

  // Hop 1 was never reported: its distribution is empty, its count is zero and
  // the missing observations are counted separately.
  CHECK_EQ(summary.hops[1].dwells_missing, std::uint64_t{10});
  CHECK_EQ(summary.hops[1].dwells_observed, std::uint64_t{0});
  CHECK_EQ(summary.hops[1].dwell.count, std::uint64_t{0});
  CHECK(!summary.hops[1].dwell.mean_ns.has_value());
  CHECK(!summary.hops[1].dwell.min_ns.has_value());
  CHECK(!summary.hops[1].dwell.max_ns.has_value());
  CHECK(!summary.hops[1].dwell.sum_ns.has_value());
  CHECK(!summary.hops[1].dwell.quantiles.empty());
  CHECK(!summary.hops[1].dwell.quantiles[0].has_value());
  CHECK_EQ(summary.hops[1].coverage_ppm, 0ULL);
  CHECK(summary.hops[1].evidence.state() == EvidenceState::Incomplete ||
        summary.hops[1].evidence.state() == EvidenceState::Missing);

  // The end to end distribution is untouched by the missing hop: it is the sum
  // of the observed end to end values, not a reconstruction from the hops.
  CHECK_EQ(*summary.end_to_end.sum_ns, 4000);
}

LATOBS_TEST(missing_hops, exports_use_null_and_empty_fields) {
  CHECK_OK(scenario, build_scenario());
  std::vector<RecordSpec> specs;
  RecordSpec spec;
  spec.sequence = 1;
  spec.observed_at_ns = 1000000;
  spec.hop_dwells_ns[0] = 100;
  spec.hop_dwells_ns[1] = -1;
  spec.hop_dwells_ns[2] = 300;
  specs.push_back(spec);
  CHECK_EQ(ingest_specs(scenario, specs, 2000000).accepted_current, std::uint64_t{1});

  runtime::ExportRequest json_request;
  json_request.kind = runtime::ExportKind::Samples;
  json_request.format = runtime::ExportFormat::Json;
  json_request.limit = 10;
  CHECK_OK(json_export, scenario.engine->export_data(json_request));
  // Only the two observed hops are present, and no hop reports a zero dwell.
  std::size_t dwell_fields = 0;
  std::size_t position = json_export.text.find("\"dwell_ns\":");
  while (position != std::string::npos) {
    ++dwell_fields;
    position = json_export.text.find("\"dwell_ns\":", position + 1);
  }
  CHECK_EQ(dwell_fields, std::size_t{2});
  CHECK(json_export.text.find("\"dwell_ns\":0") == std::string::npos);

  runtime::ExportRequest csv_request;
  csv_request.kind = runtime::ExportKind::Samples;
  csv_request.format = runtime::ExportFormat::Csv;
  csv_request.limit = 10;
  CHECK_OK(csv_export, scenario.engine->export_data(csv_request));
  CHECK(csv_export.text.find("hop1_dwell_ns") != std::string::npos);
  // The row for the exchange has two empty fields where hop 1 would be, and the
  // empty field is never a zero.
  const std::size_t newline = csv_export.text.find('\n');
  CHECK(newline != std::string::npos);
  const std::string row = csv_export.text.substr(newline + 1);
  CHECK(row.find(",,") != std::string::npos);
  CHECK(row.find("100") != std::string::npos);
  CHECK(row.find("300") != std::string::npos);
}

LATOBS_TEST(missing_hops, attribution_refuses_a_non_tiling_decomposition) {
  CHECK_OK(scenario, build_scenario());
  std::vector<RecordSpec> specs;
  for (std::uint64_t index = 0; index < 4; ++index) {
    RecordSpec spec;
    spec.sequence = index;
    spec.observed_at_ns = 1000000 + static_cast<std::int64_t>(index) * 1000;
    spec.hop_dwells_ns[0] = 100;
    // Two exchanges miss the middle hop entirely.
    spec.hop_dwells_ns[1] = index < 2 ? 200 : -1;
    spec.hop_dwells_ns[2] = 300;
    specs.push_back(spec);
  }
  CHECK_EQ(ingest_specs(scenario, specs, 2000000).accepted_current, std::uint64_t{4});

  const attribute::AttributionRequest request = make_attribution_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(attribution, scenario.engine->attribute(request));
  CHECK_EQ(attribution.exchanges_included, std::uint64_t{4});
  CHECK_EQ(attribution.exchanges_complete, std::uint64_t{2});
  CHECK_EQ(attribution.exchanges_incomplete, std::uint64_t{2});
  CHECK(attribution.kind == attribute::AttributionKind::Refused);
  CHECK(!attribution.accounted_mean_ns.has_value());
  CHECK(!attribution.unaccounted_mean_ns.has_value());
  for (const attribute::HopContribution& contribution : attribution.contributions) {
    CHECK_EQ(contribution.share_ppm, 0ULL);
  }
  bool saw_coverage_reason = false;
  for (const Reason& reason : attribution.evidence.reasons()) {
    if (reason.code == ReasonCode::NonTilingCoverage ||
        reason.code == ReasonCode::PartialCoverage ||
        reason.code == ReasonCode::RefusedByPolicy) {
      saw_coverage_reason = true;
    }
  }
  CHECK(saw_coverage_reason);

  // With partial decomposition allowed by policy the runtime still refuses to
  // invent the missing hop: it reports a partial decomposition and keeps the
  // incomplete exchanges out of the numbers.
  core::RuntimePolicy permissive = scenario.engine->policy();
  permissive.attribution.allow_partial_decomposition = true;
  ScenarioOptions options;
  options.policy = permissive;
  CHECK_OK(scenario_two, build_scenario(options));
  CHECK_EQ(ingest_specs(scenario_two, specs, 2000000).accepted_current, std::uint64_t{4});
  const attribute::AttributionRequest permissive_request = make_attribution_request(
      scenario_two, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(partial, scenario_two.engine->attribute(permissive_request));
  CHECK(partial.kind == attribute::AttributionKind::PartialDecomposition);
  CHECK_EQ(partial.exchanges_complete, std::uint64_t{2});
  CHECK(partial.accounted_mean_ns.has_value());
  CHECK(partial.evidence.state() == EvidenceState::Incomplete);
}

LATOBS_TEST(missing_hops, hop_that_is_present_but_unsupported_is_not_zero) {
  CHECK_OK(scenario, build_scenario());
  RecordSpec spec;
  spec.sequence = 1;
  spec.observed_at_ns = 1000000;
  spec.hop_dwells_ns[0] = 100;
  spec.hop_dwells_ns[1] = 200;
  spec.hop_dwells_ns[2] = 300;
  // The record carries a queue dwell request for a hop whose queue semantics do
  // not permit attribution.
  ScenarioOptions options;
  options.declare_queue_semantics = false;
  CHECK_OK(scenario_no_queue, build_scenario(options));
  CHECK_EQ(ingest_specs(scenario_no_queue, std::vector<RecordSpec>{spec}, 2000000).accepted_current,
           std::uint64_t{1});

  const stats::SummaryRequest request = make_summary_request(
      scenario_no_queue, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(summary, scenario_no_queue.engine->summarize(request));
  CHECK_EQ(summary.hops[2].queue_dwells_observed, std::uint64_t{0});
  CHECK_EQ(summary.hops[2].queue_dwells_unsupported, std::uint64_t{1});
  CHECK_EQ(summary.hops[2].queue_dwell.count, std::uint64_t{0});
  CHECK(!summary.hops[2].queue_dwell.mean_ns.has_value());
  // The hop dwell itself is still observed and reported.
  CHECK_EQ(summary.hops[2].dwells_observed, std::uint64_t{1});
  CHECK_EQ(*summary.hops[2].dwell.mean_ns, 300);
}

LATOBS_TEST_MAIN()
