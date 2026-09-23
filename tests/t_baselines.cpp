// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proof: a baseline mismatch is explicit. A baseline is never silently applied
// to a different generation, path, clock domain or bucketing, and a deviation
// is reported as statistical evidence with no causal claim.

#include <string>
#include <vector>

#include "latency_observatory/baseline/baseline.hpp"
#include "latency_observatory/baseline/codec.hpp"
#include "latency_observatory/stats/codec.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

ingest::IngestReport ingest_uniform(Scenario& scenario, std::uint64_t count, std::int64_t rtt,
                                    std::uint64_t first_sequence, std::int64_t observed_at) {
  ingest::IngestRequest request;
  request.received_at = Timestamp{observed_at + 1000, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < count; ++index) {
    RecordSpec spec;
    spec.sequence = first_sequence + index;
    spec.observed_at_ns = observed_at + static_cast<std::int64_t>(index);
    spec.hop_dwells_ns[0] = rtt / 3;
    spec.hop_dwells_ns[1] = rtt / 3;
    spec.hop_dwells_ns[2] = rtt - 2 * (rtt / 3);
    request.records.push_back(make_record(scenario, spec));
  }
  const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
  CHECK(report.has_value());
  return report.value();
}

runtime::BaselineRequest baseline_request(const Scenario& scenario, std::int64_t from,
                                          std::int64_t to) {
  runtime::BaselineRequest request;
  request.name = Name::assume_valid("test.baseline.one");
  request.path = scenario.path;
  request.generation = scenario.generation;
  request.mode = stats::AggregationMode::Current;
  request.window = make_window(from, to);
  return request;
}

bool has_mismatch(const baseline::BaselineComparison& comparison, baseline::MismatchKind kind) {
  for (const baseline::BaselineMismatch& mismatch : comparison.mismatches) {
    if (mismatch.kind == kind) return true;
  }
  return false;
}

bool evidence_has(const Evidence& evidence, ReasonCode code) {
  for (const Reason& reason : evidence.reasons()) {
    if (reason.code == code) return true;
  }
  return false;
}

}  // namespace

LATOBS_TEST(baselines, creation_requires_usable_evidence) {
  CHECK_OK(scenario, build_scenario());
  const runtime::BaselineRequest request = baseline_request(scenario, 0, 100000000);
  // No evidence at all: the baseline is refused instead of being created empty.
  CHECK_ERR(error, scenario.engine->create_baseline(request));
  CHECK(error.code() == ErrorCode::Refused);

  ingest_uniform(scenario, 32, 900, 0, 1000000);
  CHECK_OK(baseline_id, scenario.engine->create_baseline(request));
  CHECK_OK(baselines, scenario.engine->list_baselines());
  CHECK_EQ(baselines.size(), std::size_t{1});
  CHECK_OK(stored, scenario.engine->list_baselines());
  const baseline::Baseline& baseline = *stored[0];
  CHECK_EQ(baseline.exchange_count, std::uint64_t{32});
  CHECK_EQ(baseline.end_to_end.count, std::uint64_t{32});
  CHECK(baseline.generation == scenario.generation);
  CHECK(baseline.domain == core::reference_clock_domain());
  CHECK(baseline.id.valid());

  // A second baseline with the same identity is refused.
  CHECK_ERR(duplicate, scenario.engine->create_baseline(request));
  CHECK(duplicate.code() == ErrorCode::AlreadyExists || duplicate.code() == ErrorCode::Refused);
}

LATOBS_TEST(baselines, generation_mismatch_is_explicit) {
  CHECK_OK(scenario, build_scenario());
  ingest_uniform(scenario, 32, 900, 0, 1000000);
  const runtime::BaselineRequest request = baseline_request(scenario, 0, 100000000);
  CHECK_OK(created, scenario.engine->create_baseline(request));
  CHECK_OK(stored_list, scenario.engine->list_baselines());
  const baseline::Baseline& baseline = *stored_list[0];

  // The same evidence seen through a different generation must not be compared
  // against the baseline: the mismatch is reported and no delta is produced.
  stats::SummaryRequest summary_request = make_summary_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  summary_request.generation = GenerationId::derive_from("test.generation.other");
  CHECK_OK(summary, scenario.engine->summarize(summary_request));
  const baseline::BaselineComparison comparison =
      baseline::compare(baseline, summary, scenario.engine->policy());
  CHECK(!comparison.applicable);
  CHECK(has_mismatch(comparison, baseline::MismatchKind::Generation));
  CHECK(!comparison.mean_delta_ns.has_value());
  CHECK(!comparison.min_delta_ns.has_value());
  CHECK(!comparison.max_delta_ns.has_value());
  CHECK(comparison.anomalies.empty());
  CHECK(comparison.evidence.state() == EvidenceState::Unsupported);
  CHECK(evidence_has(comparison.evidence, ReasonCode::BaselineGenerationMismatch));

  // The same summary against the matching generation is applicable. The
  // baseline carries the wall clock creation time, so the comparison is made
  // with a creation time inside the synthetic window: that is the only way a
  // synthetic timeline can be compared against a real clock stamp.
  stats::SummaryRequest matching_request = make_summary_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(matching_summary, scenario.engine->summarize(matching_request));
  baseline::Baseline adjustable = baseline;
  adjustable.created_at = Timestamp{99990000, core::reference_clock_domain()};
  const baseline::BaselineComparison matching =
      baseline::compare(adjustable, matching_summary, scenario.engine->policy());
  CHECK(matching.applicable);
  CHECK(matching.mismatches.empty());
  CHECK(matching.mean_delta_ns.has_value());
  CHECK(!matching.anomalies.empty());
  CHECK_EQ(created, matching.baseline);
}

LATOBS_TEST(baselines, every_mismatch_kind_is_reported_separately) {
  CHECK_OK(scenario, build_scenario());
  ingest_uniform(scenario, 32, 900, 0, 1000000);
  const runtime::BaselineRequest request = baseline_request(scenario, 0, 100000000);
  CHECK_OK(created, scenario.engine->create_baseline(request));
  CHECK_OK(stored_list, scenario.engine->list_baselines());
  const baseline::Baseline baseline = *stored_list[0];

  const stats::SummaryRequest summary_request = make_summary_request(
      scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(summary, scenario.engine->summarize(summary_request));

  // Path mismatch.
  baseline::Baseline wrong_path = baseline;
  wrong_path.path = PathId::derive_from("test.path.other");
  const baseline::BaselineComparison path_comparison =
      baseline::compare(wrong_path, summary, scenario.engine->policy());
  CHECK(!path_comparison.applicable);
  CHECK(has_mismatch(path_comparison, baseline::MismatchKind::Path));

  // Clock domain mismatch.
  baseline::Baseline wrong_domain = baseline;
  wrong_domain.domain = ClockDomainId::derive_from("test.clock.other");
  const baseline::BaselineComparison domain_comparison =
      baseline::compare(wrong_domain, summary, scenario.engine->policy());
  CHECK(!domain_comparison.applicable);
  CHECK(has_mismatch(domain_comparison, baseline::MismatchKind::ClockDomain));

  // Histogram bucketing mismatch.
  baseline::Baseline wrong_histogram = baseline;
  std::vector<Nanos> bounds = {10, 20, 30};
  CHECK_OK(other_spec, stats::HistogramSpec::make(std::move(bounds), core::Limits{}));
  wrong_histogram.histogram = other_spec;
  const baseline::BaselineComparison histogram_comparison =
      baseline::compare(wrong_histogram, summary, scenario.engine->policy());
  CHECK(!histogram_comparison.applicable);
  CHECK(has_mismatch(histogram_comparison, baseline::MismatchKind::Histogram));

  // Missing baseline identity.
  baseline::Baseline missing = baseline;
  missing.id = BaselineId{};
  const baseline::BaselineComparison missing_comparison =
      baseline::compare(missing, summary, scenario.engine->policy());
  CHECK(!missing_comparison.applicable);
  CHECK(has_mismatch(missing_comparison, baseline::MismatchKind::Missing));

  // Stale baseline: created long before the observed window.
  baseline::Baseline stale = baseline;
  stale.created_at = Timestamp{summary.as_of.ns -
                                   scenario.engine->policy().freshness.stale_horizon_ns - 1000,
                               core::reference_clock_domain()};
  const baseline::BaselineComparison stale_comparison =
      baseline::compare(stale, summary, scenario.engine->policy());
  CHECK(!stale_comparison.applicable);
  CHECK(has_mismatch(stale_comparison, baseline::MismatchKind::Stale) ||
        has_mismatch(stale_comparison, baseline::MismatchKind::Expired));
}

LATOBS_TEST(baselines, lookup_refuses_a_generation_it_does_not_have) {
  CHECK_OK(scenario, build_scenario());
  ingest_uniform(scenario, 32, 900, 0, 1000000);
  const runtime::BaselineRequest request = baseline_request(scenario, 0, 100000000);
  CHECK_OK(created, scenario.engine->create_baseline(request));
  (void)created;

  baseline::BaselineStore store(core::default_policy().limits, core::default_policy());
  CHECK_OK(list, scenario.engine->list_baselines());
  CHECK_OK(added, store.add(*list[0]));
  CHECK_OK(found, store.latest_for(scenario.path, scenario.generation,
                                   core::reference_clock_domain(),
                                   stats::HistogramSpec::latency_default()));
  CHECK(found->id == list[0]->id);
  // A different generation of the same path is a conflict, not "no baseline".
  const Result<const baseline::Baseline*> other = store.latest_for(
      scenario.path, GenerationId::derive_from("test.generation.other"),
      core::reference_clock_domain(), stats::HistogramSpec::latency_default());
  CHECK(!other.has_value());
  CHECK(other.error().code() == ErrorCode::Conflict);
  // An unknown path is simply missing.
  const Result<const baseline::Baseline*> unknown = store.latest_for(
      PathId::derive_from("test.path.missing"), scenario.generation,
      core::reference_clock_domain(), stats::HistogramSpec::latency_default());
  CHECK(!unknown.has_value());
  CHECK(unknown.error().code() == ErrorCode::NotFound);
}

LATOBS_TEST(baselines, anomaly_statements_carry_no_causal_claim) {
  CHECK_OK(scenario, build_scenario());
  const core::RuntimePolicy& policy = scenario.engine->policy();
  std::vector<Nanos> reference_values;
  std::vector<Nanos> elevated_values;
  for (int index = 0; index < 64; ++index) {
    reference_values.push_back(1000);
    elevated_values.push_back(1000 + 20 * 1000 * 1000);  // +20ms mean
  }
  CHECK_OK(reference, stats::aggregate(reference_values, stats::HistogramSpec::latency_default(),
                                       stats::default_quantile_probes(), policy.limits));
  CHECK_OK(elevated, stats::aggregate(elevated_values, stats::HistogramSpec::latency_default(),
                                      stats::default_quantile_probes(), policy.limits));
  const baseline::AnomalyEvidence anomaly =
      baseline::classify_anomaly(elevated, reference, HopIndex::from_validated_value(0),
                                 scenario.link_hop, policy);
  CHECK(anomaly.classification == baseline::AnomalyClass::Elevated);
  CHECK(anomaly.mean_delta_ns.has_value());
  CHECK_EQ(*anomaly.mean_delta_ns, 20 * 1000 * 1000);
  CHECK(evidence_has(anomaly.evidence, ReasonCode::NoCausalInference));
  CHECK(evidence_has(anomaly.evidence, ReasonCode::DeviationElevated));
  CHECK(anomaly.ratio_ppm > 1000000ULL);

  // Too few samples: unknown, never "no deviation".
  std::vector<Nanos> few = {1000, 1000};
  CHECK_OK(small, stats::aggregate(few, stats::HistogramSpec::latency_default(),
                                   stats::default_quantile_probes(), policy.limits));
  const baseline::AnomalyEvidence insufficient =
      baseline::classify_anomaly(small, reference, HopIndex::from_validated_value(0), HopId{},
                                 policy);
  CHECK(insufficient.classification == baseline::AnomalyClass::Unknown);
  CHECK(evidence_has(insufficient.evidence, ReasonCode::InsufficientSamples));
  CHECK(evidence_has(insufficient.evidence, ReasonCode::NoCausalInference));

  // Suppressed latency is reported as suppressed, not as an improvement claim.
  std::vector<Nanos> slow_reference_values;
  std::vector<Nanos> suppressed_values;
  for (int index = 0; index < 64; ++index) {
    slow_reference_values.push_back(20 * 1000 * 1000);
    suppressed_values.push_back(1000);
  }
  CHECK_OK(slow_reference,
           stats::aggregate(slow_reference_values, stats::HistogramSpec::latency_default(),
                            stats::default_quantile_probes(), policy.limits));
  CHECK_OK(suppressed, stats::aggregate(suppressed_values, stats::HistogramSpec::latency_default(),
                                        stats::default_quantile_probes(), policy.limits));
  const baseline::AnomalyEvidence suppressed_anomaly =
      baseline::classify_anomaly(suppressed, slow_reference, HopIndex::from_validated_value(0),
                                 scenario.link_hop, policy);
  CHECK(suppressed_anomaly.classification == baseline::AnomalyClass::SuppressedElevated ||
        suppressed_anomaly.classification == baseline::AnomalyClass::Suppressed);
  CHECK(evidence_has(suppressed_anomaly.evidence, ReasonCode::DeviationSuppressed));

  // A deviation below the watch threshold is reported as no deviation, and it
  // is still reported: silence would be indistinguishable from missing data.
  std::vector<Nanos> near_values;
  for (int index = 0; index < 64; ++index) near_values.push_back(1000 + 100);
  CHECK_OK(near, stats::aggregate(near_values, stats::HistogramSpec::latency_default(),
                                  stats::default_quantile_probes(), policy.limits));
  const baseline::AnomalyEvidence near_anomaly =
      baseline::classify_anomaly(near, reference, HopIndex::from_validated_value(0),
                                 scenario.link_hop, policy);
  CHECK(near_anomaly.classification == baseline::AnomalyClass::None);
  CHECK(evidence_has(near_anomaly.evidence, ReasonCode::DeviationNone));
}

LATOBS_TEST(baselines, baseline_round_trips_through_its_codec) {
  CHECK_OK(scenario, build_scenario());
  ingest_uniform(scenario, 16, 900, 0, 1000000);
  const runtime::BaselineRequest request = baseline_request(scenario, 0, 100000000);
  CHECK_OK(created, scenario.engine->create_baseline(request));
  CHECK_OK(list, scenario.engine->list_baselines());
  const baseline::Baseline& baseline = *list[0];
  std::string text;
  {
    core::JsonWriter writer(text);
    baseline::write_json(writer, baseline);
  }
  CHECK_OK(decoded, baseline::decode_baseline_text(text, core::default_policy().limits));
  CHECK(decoded.id == baseline.id);
  CHECK(decoded.generation == baseline.generation);
  CHECK(decoded.path == baseline.path);
  CHECK(decoded.domain == baseline.domain);
  CHECK_EQ(decoded.exchange_count, baseline.exchange_count);
  CHECK_EQ(decoded.end_to_end.count, baseline.end_to_end.count);
  CHECK_EQ(decoded.hops.size(), baseline.hops.size());
  CHECK(decoded.window.from.domain == baseline.window.from.domain);
  CHECK_EQ(decoded.window.from.ns, baseline.window.from.ns);
  CHECK(decoded.histogram.digest() == baseline.histogram.digest());
  CHECK_EQ(decoded.probes.size(), baseline.probes.size());
}

LATOBS_TEST_MAIN()
