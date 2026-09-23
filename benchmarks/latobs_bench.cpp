// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Benchmarks. Every measurement reports completed work: a benchmark that did not
// complete its operation is reported as a failure, never as a fast number.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "latency_observatory/runtime/engine.hpp"
#include "latency_observatory/runtime/service.hpp"
#include "latency_observatory/store/store.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::model;
using namespace latobs::runtime;

namespace {

using SteadyClock = std::chrono::steady_clock;

struct Measurement {
  std::string name;
  std::uint64_t units = 0;
  double nanoseconds = 0.0;
  bool completed = false;
};

std::vector<Measurement> results;

double elapsed_ns(const SteadyClock::time_point& start, const SteadyClock::time_point& end) {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

void report(const std::string& name, std::uint64_t units, double nanoseconds, bool completed) {
  results.push_back(Measurement{name, units, nanoseconds, completed});
}

std::filesystem::path temporary_directory() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  return base / "latobs-benchmark";
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t records =
      argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 5000;
  std::printf("latency observatory benchmarks: %zu records per case\n\n", records);

  runtime::RuntimeConfig config;
  const std::filesystem::path store = temporary_directory();
  std::error_code error;
  std::filesystem::remove_all(store, error);
  config.store_directory = store;
  Result<std::unique_ptr<runtime::Engine>> created = runtime::Engine::create(config);
  if (!created.has_value()) {
    std::fprintf(stderr, "cannot create the runtime: %s\n", created.error().describe().c_str());
    return 1;
  }
  std::unique_ptr<runtime::Engine> engine = std::move(created.value());

  model::SourceDescriptor source;
  source.name = Name::assume_valid("bench.source");
  source.kind = model::SourceKind::Probe;
  source.authority = model::AuthorityClass::Primary;
  source.semantics = model::SemanticsProfile::EndToEndRequestResponse |
                     model::SemanticsProfile::HopDwell |
                     model::SemanticsProfile::QueueDwell;
  source.revision = Revision::first();
  const Result<SourceId> source_id = engine->define_source(source);
  if (!source_id.has_value()) return 1;

  model::GenerationDef generation;
  generation.name = Name::assume_valid("bench.generation");
  generation.revision = Revision::first();
  const Result<GenerationId> generation_id = engine->define_generation(generation);
  if (!generation_id.has_value()) return 1;

  const char* hop_names[3] = {"bench.hop.client", "bench.hop.link", "bench.hop.server"};
  std::vector<HopId> hops;
  for (const char* name : hop_names) {
    model::HopDef hop;
    hop.name = Name::assume_valid(name);
    hop.kind = model::HopKind::Endpoint;
    hop.revision = Revision::first();
    const Result<HopId> defined = engine->define_hop(hop);
    if (!defined.has_value()) return 1;
    hops.push_back(defined.value());
  }

  model::PathDef path;
  path.name = Name::assume_valid("bench.path");
  path.generation = generation_id.value();
  path.revision = Revision::first();
  path.hops = hops;
  const Result<PathId> path_id = engine->define_path(path);
  if (!path_id.has_value()) return 1;

  // --- ingestion ---
  std::vector<ingest::IngestRequest> batches;
  const std::size_t batch_size = 500;
  std::uint64_t sequence = 0;
  while (sequence < records) {
    ingest::IngestRequest request;
    request.received_at = Timestamp{2000000000, core::reference_clock_domain()};
    const std::size_t count = std::min(batch_size, records - static_cast<std::size_t>(sequence));
    for (std::size_t index = 0; index < count; ++index) {
      model::MeasurementRecord record;
      record.path = path_id.value();
      record.generation = generation_id.value();
      record.source = source_id.value();
      record.epoch = EpochId::derive_from("bench.epoch");
      record.incarnation = IncarnationId::derive_from("bench.incarnation");
      record.source_revision = Revision::first();
      record.sequence = Sequence::from_value(sequence);
      record.domain = core::reference_clock_domain();
      const std::int64_t start = 1000000 + static_cast<std::int64_t>(sequence);
      record.request = Timestamp{start, record.domain};
      std::int64_t cursor = start;
      for (std::uint32_t hop_index = 0; hop_index < 3; ++hop_index) {
        const std::int64_t dwell = 100 + static_cast<std::int64_t>(hop_index) * 50;
        model::HopObservation observation;
        observation.index = HopIndex::from_validated_value(hop_index);
        observation.hop = hops[hop_index];
        observation.entry = Timestamp{cursor, record.domain};
        observation.exit = Timestamp{cursor + dwell, record.domain};
        observation.entry_domain = record.domain;
        observation.exit_domain = record.domain;
        record.hops.push_back(observation);
        cursor += dwell;
      }
      record.response = Timestamp{cursor, record.domain};
      record.rtt_ns = record.response.ns - record.request.ns;
      record.stamp.observed_at = record.response;
      request.records.push_back(std::move(record));
      ++sequence;
    }
    batches.push_back(std::move(request));
  }

  std::uint64_t accepted = 0;
  const SteadyClock::time_point ingest_start = SteadyClock::now();
  for (ingest::IngestRequest& request : batches) {
    const Result<ingest::IngestReport> report = engine->ingest(std::move(request));
    if (!report.has_value()) {
      std::fprintf(stderr, "ingest failed: %s\n", report.error().describe().c_str());
      return 1;
    }
    accepted += report.value().accepted_current;
  }
  const SteadyClock::time_point ingest_end = SteadyClock::now();
  report("ingest (with durable persistence)", accepted, elapsed_ns(ingest_start, ingest_end),
         accepted == records);

  // --- summarization ---
  stats::SummaryRequest summary_request;
  summary_request.path = path_id.value();
  summary_request.generation = generation_id.value();
  summary_request.mode = stats::AggregationMode::Current;
  summary_request.window = stats::TimeWindow::make(
                               Timestamp{0, core::reference_clock_domain()},
                               Timestamp{100000000000LL, core::reference_clock_domain()})
                               .value();
  std::uint64_t summarized = 0;
  const SteadyClock::time_point summary_start = SteadyClock::now();
  const int summary_iterations = 20;
  for (int iteration = 0; iteration < summary_iterations; ++iteration) {
    const Result<stats::PathSummary> summary = engine->summarize(summary_request);
    if (!summary.has_value()) return 1;
    summarized += summary.value().exchanges_included;
  }
  const SteadyClock::time_point summary_end = SteadyClock::now();
  report("summarize (end to end plus per hop)", summarized,
         elapsed_ns(summary_start, summary_end),
         summarized == static_cast<std::uint64_t>(summary_iterations) * accepted);

  // --- attribution ---
  attribute::AttributionRequest attribution_request;
  attribution_request.path = path_id.value();
  attribution_request.generation = generation_id.value();
  attribution_request.mode = stats::AggregationMode::Current;
  attribution_request.window = summary_request.window;
  std::uint64_t attributed = 0;
  const SteadyClock::time_point attribution_start = SteadyClock::now();
  const int attribution_iterations = 20;
  for (int iteration = 0; iteration < attribution_iterations; ++iteration) {
    const Result<attribute::AttributionResult> result = engine->attribute(attribution_request);
    if (!result.has_value()) return 1;
    if (result.value().kind == attribute::AttributionKind::ObservedDecomposition) {
      attributed += result.value().exchanges_complete;
    }
  }
  const SteadyClock::time_point attribution_end = SteadyClock::now();
  report("attribute (decomposition with residual)", attributed,
         elapsed_ns(attribution_start, attribution_end),
         attributed == static_cast<std::uint64_t>(attribution_iterations) * accepted);

  // --- export ---
  runtime::ExportRequest export_request;
  export_request.kind = runtime::ExportKind::Samples;
  export_request.format = runtime::ExportFormat::Json;
  export_request.limit = records;
  std::uint64_t exported = 0;
  const SteadyClock::time_point export_start = SteadyClock::now();
  const Result<runtime::ExportResult> export_result = engine->export_data(export_request);
  const SteadyClock::time_point export_end = SteadyClock::now();
  if (!export_result.has_value()) return 1;
  exported = export_result.value().records_exported;
  report("export samples to canonical json", exported, elapsed_ns(export_start, export_end),
         exported == accepted);

  // --- history ---
  runtime::HistoryRequest history_request;
  history_request.path = path_id.value();
  history_request.generation = generation_id.value();
  history_request.mode = stats::AggregationMode::Historical;
  history_request.window = summary_request.window;
  history_request.bucket_ns = 1000000000;
  std::uint64_t bucketed = 0;
  const SteadyClock::time_point history_start = SteadyClock::now();
  const Result<runtime::HistoryResult> history = engine->history(history_request);
  const SteadyClock::time_point history_end = SteadyClock::now();
  if (!history.has_value()) return 1;
  bucketed = history.value().buckets_with_evidence;
  report("history distributions per bucket", bucketed, elapsed_ns(history_start, history_end),
         history.value().buckets.size() > 0);

  const Status shutdown = engine->shutdown();
  if (!shutdown.ok()) return 1;

  // --- persistence recovery ---
  const SteadyClock::time_point recovery_start = SteadyClock::now();
  Result<std::unique_ptr<runtime::Engine>> reopened = runtime::Engine::create(config);
  const SteadyClock::time_point recovery_end = SteadyClock::now();
  if (!reopened.has_value()) {
    std::fprintf(stderr, "recovery failed: %s\n", reopened.error().describe().c_str());
    return 1;
  }
  const store::RecoveryReport recovery = reopened.value()->recovery();
  report("recover the store from disk", recovery.records_read,
         elapsed_ns(recovery_start, recovery_end), recovery.records_read > 0);
  const Status reopened_shutdown = reopened.value()->shutdown();
  if (!reopened_shutdown.ok()) return 1;

  std::uint64_t store_bytes = 0;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(store, error)) {
    if (entry.is_regular_file(error)) store_bytes += entry.file_size(error);
  }

  std::printf("%-42s %12s %16s %14s\n", "case", "units", "total ms", "ns/unit");
  bool all_completed = true;
  for (const Measurement& measurement : results) {
    const double per_unit =
        measurement.units == 0 ? 0.0 : measurement.nanoseconds / static_cast<double>(measurement.units);
    std::printf("%-42s %12llu %16.3f %14.1f%s\n", measurement.name.c_str(),
                static_cast<unsigned long long>(measurement.units),
                measurement.nanoseconds / 1.0e6, per_unit,
                measurement.completed ? "" : "  INCOMPLETE");
    if (!measurement.completed) all_completed = false;
  }
  std::printf("\nstore bytes on disk: %llu\n",
              static_cast<unsigned long long>(store_bytes));
  std::printf("all benchmark operations completed: %s\n", all_completed ? "yes" : "no");
  std::filesystem::remove_all(store, error);
  return all_completed ? 0 : 1;
}
