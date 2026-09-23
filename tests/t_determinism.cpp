// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Property tests: aggregation and export are deterministic. The same set of
// observations always produces byte identical canonical output regardless of
// the order in which the observations were submitted.

#include <algorithm>
#include <string>
#include <vector>

#include "latency_observatory/core/digest.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

std::vector<RecordSpec> random_specs(Rng& rng, std::size_t count) {
  std::vector<RecordSpec> specs;
  for (std::size_t index = 0; index < count; ++index) {
    RecordSpec spec;
    spec.sequence = index;
    spec.observed_at_ns = 1000000 + static_cast<std::int64_t>(rng.range(0, 500000));
    spec.hop_dwells_ns[0] = rng.signed_range(0, 5000);
    spec.hop_dwells_ns[1] = rng.signed_range(0, 5000);
    spec.hop_dwells_ns[2] = rng.signed_range(0, 5000);
    specs.push_back(spec);
  }
  return specs;
}

/// The aggregation content of a summary: counters and distributions only. The
/// evidence reason list legitimately differs when the same observations arrive
/// out of order (the fences record gaps and reordering), so the determinism
/// property is stated over the aggregated numbers.
std::string aggregate_content(const stats::PathSummary& summary) {
  std::string text;
  core::JsonWriter writer(text);
  writer.begin_object();
  writer.field("considered", summary.exchanges_considered);
  writer.field("included", summary.exchanges_included);
  writer.field("excluded_other_path", summary.excluded_other_path);
  writer.field("excluded_other_generation", summary.excluded_other_generation);
  writer.field("excluded_out_of_window", summary.excluded_out_of_window);
  writer.field("excluded_stale", summary.excluded_stale);
  writer.field("excluded_conflicting", summary.excluded_conflicting);
  writer.field("excluded_unsupported", summary.excluded_unsupported);
  writer.key("end_to_end");
  stats::write_json(writer, summary.end_to_end);
  writer.field_array("hops");
  for (const stats::HopSummary& hop : summary.hops) {
    writer.begin_object();
    writer.field("index", static_cast<std::uint64_t>(hop.index.value()));
    writer.field("included", hop.exchanges_included);
    writer.field("observed", hop.dwells_observed);
    writer.field("missing", hop.dwells_missing);
    writer.field("unsupported", hop.dwells_unsupported);
    writer.field("queue_observed", hop.queue_dwells_observed);
    writer.field("queue_unsupported", hop.queue_dwells_unsupported);
    writer.field("coverage_ppm", hop.coverage_ppm);
    writer.key("dwell");
    stats::write_json(writer, hop.dwell);
    writer.key("queue_dwell");
    stats::write_json(writer, hop.queue_dwell);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return text;
}

std::string canonical_summary(Scenario& scenario, stats::AggregationMode mode) {
  const stats::SummaryRequest request = make_summary_request(scenario, 0, 1000000000, mode);
  const Result<stats::PathSummary> summary = scenario.engine->summarize(request);
  CHECK(summary.has_value());
  return aggregate_content(summary.value());
}

}  // namespace

LATOBS_TEST(determinism, submission_order_does_not_change_the_summary) {
  // One data set, six submission orders: only the order changes.
  Rng data_generator(100);
  const std::vector<RecordSpec> dataset = random_specs(data_generator, 64);
  std::string reference;
  for (int trial = 0; trial < 6; ++trial) {
    Rng order_generator(static_cast<std::uint64_t>(1000 + trial));
    CHECK_OK(scenario, build_scenario());
    std::vector<RecordSpec> specs = dataset;
    if (trial != 0) {
      for (std::size_t index = specs.size(); index > 1; --index) {
        const std::size_t other = static_cast<std::size_t>(order_generator.range(0, index - 1));
        std::swap(specs[index - 1], specs[other]);
      }
    }
    // Every record is stamped with the same receive time so that only the order
    // differs between trials.
    ingest::IngestRequest request;
    request.received_at = Timestamp{2000000, core::reference_clock_domain()};
    for (const RecordSpec& spec : specs) {
      request.records.push_back(make_record(scenario, spec));
    }
    const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
    CHECK(report.has_value());
    if (trial == 0) {
      // Sequence numbers arrive in order on the first trial only; the others
      // are reordered, which the fence marks as historical. Both are compared in
      // historical mode where reordering does not change inclusion.
      reference = canonical_summary(scenario, stats::AggregationMode::Historical);
      continue;
    }
    const std::string candidate = canonical_summary(scenario, stats::AggregationMode::Historical);
    CHECK_EQ(candidate, reference);
  }
}

/// Sorted CSV data lines: the export row order follows insertion order, so the
/// set of exported rows is what must be stable across batchings.
std::string sorted_export_rows(const std::string& csv) {
  std::vector<std::string> rows;
  std::size_t position = csv.find('\n');
  if (position == std::string::npos) return std::string();
  std::string header = csv.substr(0, position);
  ++position;
  while (position < csv.size()) {
    const std::size_t end = csv.find('\n', position);
    rows.push_back(csv.substr(position, end == std::string::npos ? std::string::npos
                                                                 : end - position));
    if (end == std::string::npos) break;
    position = end + 1;
  }
  std::sort(rows.begin(), rows.end());
  std::string combined = header;
  combined.push_back('\n');
  for (const std::string& row : rows) {
    combined.append(row);
    combined.push_back('\n');
  }
  return combined;
}

LATOBS_TEST(determinism, batching_and_ordering_do_not_change_exports) {
  // One fixed data set, ingested with different batch boundaries and different
  // in-batch orders. The aggregated content and the set of exported rows must
  // be identical every time.
  Rng generator(500);
  const std::vector<RecordSpec> specs = random_specs(generator, 32);
  std::string reference_aggregate;
  std::string reference_export;
  for (int trial = 0; trial < 5; ++trial) {
    Rng ordering(static_cast<std::uint64_t>(900 + trial));
    CHECK_OK(scenario, build_scenario());
    std::vector<RecordSpec> pending = specs;
    if (trial != 0) {
      for (std::size_t index = pending.size(); index > 1; --index) {
        const std::size_t other = static_cast<std::size_t>(ordering.range(0, index - 1));
        std::swap(pending[index - 1], pending[other]);
      }
    }
    std::size_t position = 0;
    while (position < pending.size()) {
      const std::size_t batch = std::min<std::size_t>(
          pending.size() - position, 1 + static_cast<std::size_t>(ordering.range(1, 9)));
      ingest::IngestRequest request;
      request.received_at = Timestamp{2000000, core::reference_clock_domain()};
      for (std::size_t offset = 0; offset < batch; ++offset) {
        request.records.push_back(make_record(scenario, pending[position + offset]));
      }
      const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
      CHECK(report.has_value());
      position += batch;
    }
    CHECK_EQ(canonical_summary(scenario, stats::AggregationMode::Historical),
             trial == 0 ? canonical_summary(scenario, stats::AggregationMode::Historical)
                        : reference_aggregate);
    const std::string aggregate = canonical_summary(scenario, stats::AggregationMode::Historical);
    runtime::ExportRequest export_request;
    export_request.kind = runtime::ExportKind::Samples;
    export_request.format = runtime::ExportFormat::Csv;
    export_request.limit = 1000;
    const Result<runtime::ExportResult> exported = scenario.engine->export_data(export_request);
    CHECK(exported.has_value());
    CHECK_EQ(exported.value().records_exported, static_cast<std::uint64_t>(specs.size()));
    const std::string rows = sorted_export_rows(exported.value().text);
    if (trial == 0) {
      reference_aggregate = aggregate;
      reference_export = rows;
    } else {
      CHECK_EQ(aggregate, reference_aggregate);
      CHECK_EQ(rows, reference_export);
    }
  }
}

LATOBS_TEST(determinism, histogram_invariants_hold_for_random_data) {
  Rng generator(2026);
  for (int trial = 0; trial < 25; ++trial) {
    std::vector<Nanos> values;
    const std::size_t count = static_cast<std::size_t>(generator.range(1, 300));
    for (std::size_t index = 0; index < count; ++index) {
      values.push_back(generator.signed_range(0, 100000000));
    }
    const stats::HistogramSpec spec = stats::HistogramSpec::latency_default();
    const std::vector<stats::QuantileProbe> probes = stats::default_quantile_probes();
    CHECK_OK(distribution, stats::aggregate(values, spec, probes, core::Limits{}));
    CHECK_EQ(distribution.count, static_cast<std::uint64_t>(count));
    // Buckets partition the values exactly.
    CHECK_EQ(distribution.histogram.total(), static_cast<std::uint64_t>(count));
    CHECK_EQ(distribution.histogram.underflow, std::uint64_t{0});
    // Ordering of the statistics.
    CHECK(*distribution.min_ns <= *distribution.quantiles[0]);
    CHECK(*distribution.quantiles[0] <= *distribution.quantiles[1]);
    CHECK(*distribution.quantiles[1] <= *distribution.quantiles[2]);
    CHECK(*distribution.quantiles[2] <= *distribution.quantiles[3]);
    CHECK(*distribution.quantiles[3] <= *distribution.max_ns);
    CHECK(*distribution.mean_ns >= *distribution.min_ns);
    CHECK(*distribution.mean_ns <= *distribution.max_ns);
    // The exact sum identity: sum == mean * count + remainder.
    const std::int64_t reconstructed =
        *distribution.mean_ns * static_cast<std::int64_t>(count) +
        static_cast<std::int64_t>(distribution.mean_remainder_ns);
    CHECK_EQ(reconstructed, *distribution.sum_ns);
    // The histogram is monotone as a cumulative distribution.
    std::uint64_t cumulative = distribution.histogram.underflow;
    for (const std::uint64_t bucket : distribution.histogram.buckets) {
      cumulative += bucket;
    }
    CHECK_EQ(cumulative, static_cast<std::uint64_t>(count));
  }
}

LATOBS_TEST(determinism, quantization_is_reproducible_across_engines) {
  // Two independent runtimes, same evidence, same policy: identical digests.
  Rng generator(99);
  std::vector<RecordSpec> specs = random_specs(generator, 48);
  std::string first;
  std::string second;
  for (int round = 0; round < 2; ++round) {
    CHECK_OK(scenario, build_scenario());
    ingest::IngestRequest request;
    request.received_at = Timestamp{2000000, core::reference_clock_domain()};
    for (const RecordSpec& spec : specs) {
      request.records.push_back(make_record(scenario, spec));
    }
    const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
    CHECK(report.has_value());
    const std::string canonical = canonical_summary(scenario, stats::AggregationMode::Current);
    const std::string digest = core::sha256_hex(canonical);
    if (round == 0) {
      first = digest;
    } else {
      second = digest;
    }
    CHECK_EQ(scenario.engine->policy().digest(), core::default_policy().digest());
  }
  CHECK_EQ(first, second);
}

LATOBS_TEST_MAIN()
