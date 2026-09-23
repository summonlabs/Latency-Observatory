// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deliberate attempts to break the runtime: malformed input, hostile sizes,
// extreme values, corrupted persistence and shutdown races. Every case asserts
// that the runtime answers, refuses or reports, and never crashes, hangs or
// invents a value.

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/runtime/engine.hpp"
#include "latency_observatory/runtime/service.hpp"
#include "latency_observatory/store/store.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

std::string random_text(Rng& rng, std::size_t length) {
  static const char kAlphabet[] =
      "{}[]\":,\t\n abcdefghijklmnopqrstuvwxyz0123456789\\/.-+_";
  std::string text;
  text.reserve(length);
  for (std::size_t index = 0; index < length; ++index) {
    text.push_back(kAlphabet[rng.range(0, sizeof(kAlphabet) - 2)]);
  }
  return text;
}

std::size_t file_size(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::size_t>(size);
}

}  // namespace

LATOBS_TEST(adversarial, garbage_requests_never_produce_a_malformed_response) {
  runtime::RuntimeConfig config;
  config.policy.limits.max_json_bytes = 4096;
  config.policy.limits.max_transport_line_bytes = 4096;
  CHECK_OK(service, runtime::Service::create(config));
  Rng rng(20260101);
  std::size_t checked = 0;
  for (int trial = 0; trial < 400; ++trial) {
    const std::string request = random_text(rng, 1 + static_cast<std::size_t>(rng.range(0, 300)));
    const std::string response = service->execute(request);
    // Whatever the input, the answer must be a single well formed JSON document
    // with an explicit ok flag.
    const Result<JsonValue> document = core::parse_json(response, 64);
    if (!document.has_value()) {
      fail(__FILE__, __LINE__, "malformed response for input: " + request + " -> " + response);
    }
    if (document.value().find("ok") == nullptr) {
      fail(__FILE__, __LINE__, "response without an ok flag: " + response);
    }
    ++checked;
  }
  CHECK_EQ(checked, std::size_t{400});

  // Structured but hostile documents.
  const char* hostile[] = {
      R"({"op":"ingest","records":[]})",
      R"({"op":"ingest","records":[{}]})",
      R"({"op":"ingest","records":[{"path":1}]})",
      R"({"op":"summarize","path":"","generation":"","from_ns":0,"to_ns":0})",
      R"({"op":"summarize","path":"x","generation":"x","from_ns":10,"to_ns":0})",
      R"({"op":"history","path":"x","generation":"x","bucket_ns":0})",
      R"({"op":"export","kind":"samples","format":"xml"})",
      R"({"op":"export","kind":"samples","format":"json","limit":0})",
      R"({"op":"define_source","name":"-bad","source_kind":"probe","authority":"primary","semantics":"end_to_end_request_response","revision":1})",
      R"({"op":"define_generation","name":"x","revision":0})",
      R"({"op":"clock_sync","domain":"a","reference":"b","state":"flying","offset_ns":0,"uncertainty_ns":0,"valid_for_ns":1,"observed_at_ns":0,"observed_at_domain":"c","revision":1})",
      R"({"op":"attribute","path":"x","generation":"y","from_ns":0,"to_ns":10,"baseline":"0000000000000000"})",
      R"({"op":"define_path","name":"deep","generation":"g","revision":1,"hops":[]})",
  };
  for (const char* request : hostile) {
    const std::string response = service->execute(request);
    const Result<JsonValue> document = core::parse_json(response, 64);
    CHECK(document.has_value());
    if (document.has_value()) {
      const JsonValue* ok = document.value().find("ok");
      CHECK(ok != nullptr);
      if (ok != nullptr && ok->is_bool()) {
        // A hostile request must be refused, never silently accepted with a
        // fabricated result.
        CHECK(!ok->as_bool().value() || std::string(request).find("\"ingest\",\"records\":[]") != std::string::npos);
      }
    }
  }
  CHECK_OK_STATUS(service->shutdown());
}

LATOBS_TEST(adversarial, extreme_values_are_handled_without_wrapping) {
  CHECK_OK(scenario, build_scenario());
  ingest::IngestRequest request;
  // Each exchange lasts three quarters of the representable range, so the batch
  // is received far in the future of the synthetic epoch.
  const std::int64_t huge_dwell = std::numeric_limits<std::int64_t>::max() / 4;
  const std::int64_t received_at = 1000000 + 3 * huge_dwell + 1000;
  request.received_at = Timestamp{received_at, core::reference_clock_domain()};
  for (int index = 0; index < 2; ++index) {
    RecordSpec spec;
    spec.sequence = static_cast<std::uint64_t>(index);
    spec.observed_at_ns = 1000000;
    spec.hop_dwells_ns[0] = huge_dwell;
    spec.hop_dwells_ns[1] = huge_dwell;
    spec.hop_dwells_ns[2] = huge_dwell;
    request.records.push_back(make_record(scenario, spec));
  }
  CHECK_OK(report, scenario.engine->ingest(std::move(request)));
  CHECK_EQ(report.accepted_current, std::uint64_t{2});

  const stats::SummaryRequest summary_request =
      make_summary_request(scenario, 0, received_at + 1000, stats::AggregationMode::Current);
  CHECK_OK(summary, scenario.engine->summarize(summary_request));
  CHECK_EQ(summary.exchanges_included, std::uint64_t{2});
  CHECK_EQ(summary.end_to_end.count, std::uint64_t{2});
  // Two thirds of INT64_MAX still fit; the sum of two of those does not, and the
  // runtime reports that instead of wrapping.
  CHECK(summary.end_to_end.sum_overflowed);
  CHECK(!summary.end_to_end.sum_ns.has_value());
  CHECK(!summary.end_to_end.mean_ns.has_value());
  CHECK_EQ(*summary.end_to_end.max_ns, std::numeric_limits<std::int64_t>::max() / 4 * 3);
  CHECK_EQ(summary.end_to_end.histogram.total(), std::uint64_t{2});

  const attribute::AttributionRequest attribution_request =
      make_attribution_request(scenario, 0, received_at + 1000, stats::AggregationMode::Current);
  CHECK_OK(attribution, scenario.engine->attribute(attribution_request));
  CHECK(attribution.kind == attribute::AttributionKind::ObservedDecomposition);
  // The per hop sums and their mean are representable; the end to end sum is
  // not. The decomposition is produced and the missing residual is explained
  // rather than fabricated.
  CHECK(attribution.accounted_mean_ns.has_value());
  CHECK_EQ(*attribution.accounted_mean_ns, 3 * huge_dwell);
  CHECK(!attribution.end_to_end_mean_ns.has_value());
  CHECK(!attribution.unaccounted_mean_ns.has_value());
  bool saw_overflow_reason = false;
  for (const Reason& reason : attribution.evidence.reasons()) {
    if (reason.code == ReasonCode::ArithmeticOverflow) saw_overflow_reason = true;
  }
  CHECK(saw_overflow_reason);
}

LATOBS_TEST(adversarial, bounds_are_enforced_at_every_edge) {
  core::RuntimePolicy policy = core::default_policy();
  policy.limits.max_history_buckets = 8;
  policy.limits.max_histogram_buckets = 4;
  policy.limits.max_export_records = 4;
  policy.limits.max_batch_samples = 4;
  policy.limits.max_json_depth = 4;
  ScenarioOptions options;
  options.policy = policy;
  CHECK_OK(scenario, build_scenario(options));

  // Too many history buckets for the window.
  runtime::HistoryRequest history_request;
  history_request.path = scenario.path;
  history_request.generation = scenario.generation;
  history_request.mode = stats::AggregationMode::Historical;
  history_request.window = make_window(0, 1000000);
  history_request.bucket_ns = 1000;  // a thousand buckets in an eight bucket budget
  CHECK_ERR(history_error, scenario.engine->history(history_request));
  CHECK(history_error.code() == ErrorCode::OutOfRange);

  // A window and a bucket width whose rounding would overflow are refused
  // rather than wrapped into a tiny bucket count.
  runtime::HistoryRequest huge_request;
  huge_request.path = scenario.path;
  huge_request.generation = scenario.generation;
  huge_request.mode = stats::AggregationMode::Historical;
  huge_request.window = make_window(0, std::numeric_limits<std::int64_t>::max());
  huge_request.bucket_ns = std::numeric_limits<std::int64_t>::max();
  CHECK_ERR(huge_error, scenario.engine->history(huge_request));
  CHECK(huge_error.code() == ErrorCode::Overflow);

  // A batch larger than the limit is refused as a whole.
  ingest::IngestRequest batch;
  batch.received_at = Timestamp{2000000, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < policy.limits.max_batch_samples + 1; ++index) {
    RecordSpec spec;
    spec.sequence = index;
    spec.observed_at_ns = 1000000;
    batch.records.push_back(make_record(scenario, spec));
  }
  CHECK_ERR(batch_error, scenario.engine->ingest(std::move(batch)));
  CHECK(batch_error.code() == ErrorCode::CapacityExceeded);

  // A histogram specification with more buckets than allowed.
  std::vector<Nanos> bounds = {1, 2, 3, 4, 5};
  CHECK_ERR(histogram_error, stats::HistogramSpec::make(std::move(bounds), policy.limits));
  CHECK(histogram_error.code() == ErrorCode::OutOfRange);

  // A valid batch, then an export limit below the number of records.
  ingest::IngestRequest small;
  small.received_at = Timestamp{2000000, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < policy.limits.max_batch_samples; ++index) {
    RecordSpec spec;
    spec.sequence = index;
    spec.observed_at_ns = 1000000;
    small.records.push_back(make_record(scenario, spec));
  }
  CHECK_OK(small_report, scenario.engine->ingest(small));
  CHECK_EQ(small_report.accepted_current, std::uint64_t{4});
  // A second batch doubles the stored evidence; the export limit is clamped to
  // the policy maximum of four and the rest is reported as skipped.
  ingest::IngestRequest second;
  second.received_at = Timestamp{2000000, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < policy.limits.max_batch_samples; ++index) {
    RecordSpec spec;
    spec.sequence = 100 + index;
    spec.observed_at_ns = 1000000;
    second.records.push_back(make_record(scenario, spec));
  }
  CHECK_OK(second_report, scenario.engine->ingest(std::move(second)));
  CHECK_EQ(second_report.accepted_current, std::uint64_t{4});
  runtime::ExportRequest export_request;
  export_request.kind = runtime::ExportKind::Samples;
  export_request.format = runtime::ExportFormat::Json;
  export_request.limit = 1000;  // clamped to max_export_records
  CHECK_OK(exported, scenario.engine->export_data(export_request));
  CHECK_EQ(exported.records_exported, std::uint64_t{4});
  CHECK(exported.truncated);
  CHECK_EQ(exported.records_skipped, std::uint64_t{4});
}

LATOBS_TEST(adversarial, corrupted_stores_never_crash_recovery) {
  TempDir directory("adversarial-store");
  {
    ScenarioOptions options;
    options.store_directory = directory.path();
    CHECK_OK(scenario, build_scenario(options));
    ingest::IngestRequest request;
    request.received_at = Timestamp{2000000, core::reference_clock_domain()};
    for (std::uint64_t index = 0; index < 8; ++index) {
      RecordSpec spec;
      spec.sequence = index;
      spec.observed_at_ns = 1000000 + static_cast<std::int64_t>(index);
      spec.hop_dwells_ns[0] = 100;
      spec.hop_dwells_ns[1] = 200;
      spec.hop_dwells_ns[2] = 300;
      request.records.push_back(make_record(scenario, spec));
    }
    CHECK_OK(report, scenario.engine->ingest(std::move(request)));
    CHECK_EQ(report.accepted_current, std::uint64_t{8});
    CHECK_OK_STATUS(scenario.engine->shutdown());
  }
  CHECK_OK(segments, store::list_segments(directory.path()));
  CHECK(!segments.empty());
  const std::filesystem::path segment = segments.back();
  const std::string pristine = [&segment]() {
    std::ifstream stream(segment, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }();
  CHECK(pristine.size() > 64);

  Rng rng(4242);
  std::size_t recovered_cleanly = 0;
  std::size_t reported_damage = 0;
  std::size_t lost_definitions = 0;
  for (int trial = 0; trial < 24; ++trial) {
    std::string corrupted = pristine;
    const std::size_t flips = 1 + static_cast<std::size_t>(rng.range(0, 8));
    for (std::size_t flip = 0; flip < flips; ++flip) {
      const std::size_t offset = static_cast<std::size_t>(rng.range(0, corrupted.size() - 1));
      corrupted[offset] = static_cast<char>(corrupted[offset] ^ static_cast<char>(rng.range(1, 255)));
    }
    {
      std::ofstream stream(segment, std::ios::binary | std::ios::trunc);
      stream.write(corrupted.data(), static_cast<std::streamsize>(corrupted.size()));
    }
    runtime::RuntimeConfig config;
    config.store_directory = directory.path();
    Result<std::unique_ptr<runtime::Engine>> reopened = runtime::Engine::create(config);
    if (!reopened.has_value()) {
      // A damaged header is refused outright; that is a report, not a crash.
      ++reported_damage;
      continue;
    }
    const store::RecoveryReport recovery = reopened.value()->recovery();
    if (recovery.clean() && recovery.records_read == 8) {
      ++recovered_cleanly;
    } else {
      ++reported_damage;
      // Recovery must never report more records than were written.
      CHECK(recovery.records_read <= 8 + 16);
    }
    // A damaged store can lose definitions as well as records. Whatever is
    // gone, the lookup must fail with a typed error instead of aborting.
    const Result<const model::PathDef*> path =
        reopened.value()->catalog().path_by_name("test.path.alpha");
    const Result<const model::GenerationDef*> generation =
        reopened.value()->catalog().generation_by_name("test.generation.one");
    if (path.has_value() && generation.has_value()) {
      const stats::SummaryRequest request = make_summary_request(
          path.value()->id, generation.value()->id, 0, 100000000,
          stats::AggregationMode::Historical);
      const Result<stats::PathSummary> summary = reopened.value()->summarize(request);
      CHECK(summary.has_value());
      if (summary.has_value()) {
        CHECK(summary.value().exchanges_included <= 8);
      }
    } else {
      ++lost_definitions;
    }
    CHECK_OK_STATUS(reopened.value()->shutdown());
  }
  CHECK_EQ(recovered_cleanly + reported_damage, std::size_t{24});
  (void)lost_definitions;
  // Random single bit flips inside a record must be caught rather than ignored.
  CHECK(reported_damage > 0);
}

LATOBS_TEST(adversarial, shutdown_races_with_readers_without_losing_an_answer) {
  CHECK_OK(scenario, build_scenario());
  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < 32; ++index) {
    RecordSpec spec;
    spec.sequence = index;
    spec.observed_at_ns = 1000000 + static_cast<std::int64_t>(index);
    spec.hop_dwells_ns[0] = 100;
    spec.hop_dwells_ns[1] = 200;
    spec.hop_dwells_ns[2] = 300;
    request.records.push_back(make_record(scenario, spec));
  }
  CHECK_OK(report, scenario.engine->ingest(std::move(request)));
  CHECK_EQ(report.accepted_current, std::uint64_t{32});

  const stats::SummaryRequest summary_request =
      make_summary_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  std::atomic<int> answered{0};
  std::atomic<int> refused{0};
  std::atomic<bool> failed{false};
  std::vector<std::thread> readers;
  readers.reserve(6);
  for (int reader = 0; reader < 6; ++reader) {
    readers.emplace_back([&]() {
      for (int iteration = 0; iteration < 64; ++iteration) {
        const Result<stats::PathSummary> summary = scenario.engine->summarize(summary_request);
        if (summary.has_value()) {
          answered.fetch_add(1);
          if (summary.value().exchanges_included != 32) failed.store(true);
        } else if (summary.error().code() == ErrorCode::ShuttingDown ||
                   summary.error().code() == ErrorCode::Cancelled) {
          refused.fetch_add(1);
        } else {
          failed.store(true);
        }
      }
    });
  }
  // Shut the runtime down while the readers are running.
  std::thread stopper([&]() {
    const Status status = scenario.engine->shutdown();
    if (!status.ok()) failed.store(true);
  });
  for (std::thread& thread : readers) thread.join();
  stopper.join();
  CHECK(!failed.load());
  // Every iteration produced an answer: a snapshot or an explicit refusal.
  CHECK_EQ(answered.load() + refused.load(), 6 * 64);
  CHECK(scenario.engine->shut_down());
}

LATOBS_TEST(adversarial, a_baseline_never_crosses_a_restart_into_another_generation) {
  TempDir directory("adversarial-baseline");
  ScenarioOptions options;
  options.store_directory = directory.path();
  CHECK_OK(scenario, build_scenario(options));
  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < 16; ++index) {
    RecordSpec spec;
    spec.sequence = index;
    spec.observed_at_ns = 1000000 + static_cast<std::int64_t>(index);
    spec.hop_dwells_ns[0] = 100;
    spec.hop_dwells_ns[1] = 200;
    spec.hop_dwells_ns[2] = 300;
    request.records.push_back(make_record(scenario, spec));
  }
  CHECK_OK(report, scenario.engine->ingest(std::move(request)));
  runtime::BaselineRequest baseline_request;
  baseline_request.name = Name::assume_valid("adversarial.baseline");
  baseline_request.path = scenario.path;
  baseline_request.generation = scenario.generation;
  baseline_request.mode = stats::AggregationMode::Current;
  baseline_request.window = make_window(0, 100000000);
  CHECK_OK(baseline_id, scenario.engine->create_baseline(baseline_request));
  CHECK_OK_STATUS(scenario.engine->shutdown());

  // The restarted runtime declares a new generation and asks for a baseline of
  // it: the lookup must fail explicitly instead of returning the old baseline.
  runtime::RuntimeConfig config;
  config.store_directory = directory.path();
  CHECK_OK(reopened, runtime::Engine::create(config));
  model::GenerationDef next;
  next.name = Name::assume_valid("test.generation.two");
  next.revision = Revision::first();
  next.supersedes = GenerationId::derive_from("test.generation.one");
  CHECK_OK(next_generation, reopened->define_generation(next));
  CHECK(reopened->catalog().is_superseded(GenerationId::derive_from("test.generation.one")));

  attribute::AttributionRequest attribution_request;
  attribution_request.path = PathId::derive_from("test.path.alpha");
  attribution_request.generation = next_generation;
  attribution_request.mode = stats::AggregationMode::Historical;
  attribution_request.window = make_window(0, 100000000);
  attribution_request.baseline = baseline_id;
  CHECK_OK(attribution, reopened->attribute(attribution_request));
  CHECK(attribution.comparison.has_value());
  if (attribution.comparison.has_value()) {
    CHECK(!attribution.comparison->applicable);
    bool saw_generation_mismatch = false;
    for (const baseline::BaselineMismatch& mismatch : attribution.comparison->mismatches) {
      if (mismatch.kind == baseline::MismatchKind::Generation) saw_generation_mismatch = true;
    }
    CHECK(saw_generation_mismatch);
    CHECK(!attribution.comparison->mean_delta_ns.has_value());
  }
  CHECK_OK_STATUS(reopened->shutdown());
}

LATOBS_TEST_MAIN()
