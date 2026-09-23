// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proof: persistence is versioned and integrity checked, recovery is
// conservative, and a restart never makes historical timing current.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "latency_observatory/core/digest.hpp"
#include "latency_observatory/runtime/engine.hpp"
#include "latency_observatory/store/store.hpp"
#include "support.hpp"
#include "test_framework.hpp"

#if defined(_WIN32)
#include <io.h>
#endif

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

ScenarioOptions options_with_store(const TempDir& directory) {
  ScenarioOptions options;
  options.store_directory = directory.path();
  return options;
}

ingest::IngestReport ingest_batch(Scenario& scenario, std::uint64_t count,
                                  std::uint64_t first_sequence, std::int64_t observed_at,
                                  std::int64_t received_at) {
  ingest::IngestRequest request;
  request.received_at = Timestamp{received_at, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < count; ++index) {
    RecordSpec spec;
    spec.sequence = first_sequence + index;
    spec.observed_at_ns = observed_at + static_cast<std::int64_t>(index);
    spec.hop_dwells_ns[0] = 100;
    spec.hop_dwells_ns[1] = 200;
    spec.hop_dwells_ns[2] = 300;
    request.records.push_back(make_record(scenario, spec));
  }
  const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
  CHECK(report.has_value());
  return report.value();
}

std::size_t path_size(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::size_t>(size);
}

bool evidence_has_reason(const std::vector<Reason>& reasons, ReasonCode code) {
  for (const Reason& reason : reasons) {
    if (reason.code == code) return true;
  }
  return false;
}

}  // namespace

LATOBS_TEST(store, writes_and_recovers_a_complete_store) {
  TempDir directory("store-roundtrip");
  {
    CHECK_OK(scenario, build_scenario(options_with_store(directory)));
    CHECK_EQ(ingest_batch(scenario, 16, 0, 1000000, 2000000).accepted_current, std::uint64_t{16});
    const stats::SummaryRequest request =
        make_summary_request(scenario, 0, 100000000, stats::AggregationMode::Current);
    CHECK_OK(summary, scenario.engine->summarize(request));
    CHECK_EQ(summary.exchanges_included, std::uint64_t{16});
    CHECK_OK_STATUS(scenario.engine->shutdown());
  }
  CHECK(std::filesystem::exists(directory.path() / "manifest.lobs"));
  CHECK_OK(segments, store::list_segments(directory.path()));
  CHECK(!segments.empty());
  CHECK(path_size(directory.path() / "manifest.lobs") > 0);

  runtime::RuntimeConfig config;
  config.store_directory = directory.path();
  CHECK_OK(reopened, runtime::Engine::create(config));
  CHECK_EQ(reopened->catalog().path_count(), std::size_t{1});
  CHECK_EQ(reopened->catalog().source_count(), std::size_t{1});
  CHECK_EQ(reopened->catalog().generation_count(), std::size_t{1});
  const store::RecoveryReport report = reopened->recovery();
  CHECK(report.clean());
  CHECK_EQ(report.segments_accepted, std::uint64_t{1});
  CHECK_EQ(report.segments_rejected, std::uint64_t{0});
  CHECK(report.records_read > 16);
  CHECK_OK_STATUS(reopened->shutdown());
}

LATOBS_TEST(store, restart_does_not_make_history_current) {
  TempDir directory("store-restart");
  const std::int64_t observed_at = 1000000;
  {
    CHECK_OK(scenario, build_scenario(options_with_store(directory)));
    CHECK_EQ(ingest_batch(scenario, 8, 0, observed_at, observed_at + 1000).accepted_current,
             std::uint64_t{8});
    const stats::SummaryRequest request =
        make_summary_request(scenario, 0, 100000000, stats::AggregationMode::Current);
    CHECK_OK(summary, scenario.engine->summarize(request));
    CHECK_EQ(summary.exchanges_included, std::uint64_t{8});
    CHECK_OK_STATUS(scenario.engine->shutdown());
  }

  runtime::RuntimeConfig config;
  config.store_directory = directory.path();
  CHECK_OK(reopened, runtime::Engine::create(config));
  CHECK_OK(reopened_path, reopened->catalog().path_by_name("test.path.alpha"));
  CHECK_OK(reopened_generation, reopened->catalog().generation_by_name("test.generation.one"));
  const stats::SummaryRequest historical_request =
      make_summary_request(reopened_path->id, reopened_generation->id, 0, 100000000,
                           stats::AggregationMode::Historical);
  CHECK_OK(historical, reopened->summarize(historical_request));
  CHECK_EQ(historical.exchanges_included, std::uint64_t{8});
  CHECK_EQ(historical.end_to_end.count, std::uint64_t{8});
  CHECK_EQ(*historical.end_to_end.mean_ns, 600);

  // The current view is empty: the evidence belongs to a previous session and is
  // far outside the freshness horizons of this one.
  const stats::SummaryRequest current_request =
      make_summary_request(reopened_path->id, reopened_generation->id, 0, 100000000,
                           stats::AggregationMode::Current);
  CHECK_OK(current, reopened->summarize(current_request));
  CHECK_EQ(current.exchanges_included, std::uint64_t{0});
  CHECK_EQ(current.exchanges_considered, std::uint64_t{8});
  CHECK_EQ(current.excluded_stale, std::uint64_t{8});
  CHECK_EQ(current.end_to_end.count, std::uint64_t{0});
  CHECK(!current.end_to_end.mean_ns.has_value());

  // The provenance of the recovered evidence is intact and says it was loaded
  // from a previous session.
  CHECK_OK(explanation, reopened->explain(historical_request));
  CHECK(!explanation.provenance.empty());
  CHECK(evidence_has_reason(explanation.reasons, ReasonCode::RestartLoadedEvidence));
  CHECK(explanation.states_no_causality());
  CHECK_OK_STATUS(reopened->shutdown());
}

LATOBS_TEST(store, replays_are_still_refused_after_a_restart) {
  TempDir directory("store-fence");
  {
    CHECK_OK(scenario, build_scenario(options_with_store(directory)));
    CHECK_EQ(ingest_batch(scenario, 4, 0, 1000000, 2000000).accepted_current, std::uint64_t{4});
    CHECK_OK_STATUS(scenario.engine->shutdown());
  }
  runtime::RuntimeConfig config;
  config.store_directory = directory.path();
  CHECK_OK(reopened, runtime::Engine::create(config));
  ingest::IngestRequest replay;
  replay.received_at = Timestamp{2000001, core::reference_clock_domain()};
  RecordSpec spec;
  spec.sequence = 2;
  spec.observed_at_ns = 1000002;
  spec.hop_dwells_ns[0] = 100;
  spec.hop_dwells_ns[1] = 200;
  spec.hop_dwells_ns[2] = 300;
  CHECK_OK(reopened_path, reopened->catalog().path_by_name("test.path.alpha"));
  CHECK_OK(reopened_generation, reopened->catalog().generation_by_name("test.generation.one"));
  CHECK_OK(reopened_source, reopened->catalog().source_by_name("test.source.probe"));
  Scenario reopened_scenario;
  reopened_scenario.engine = nullptr;
  reopened_scenario.path = reopened_path->id;
  reopened_scenario.generation = reopened_generation->id;
  reopened_scenario.source = reopened_source->id;
  reopened_scenario.link = LinkId::derive_from("test.link.alpha");
  reopened_scenario.queue = QueueId::derive_from("test.queue.egress");
  reopened_scenario.client_hop = HopId::derive_from("test.hop.client");
  reopened_scenario.link_hop = HopId::derive_from("test.hop.link");
  reopened_scenario.queue_hop = HopId::derive_from("test.hop.queue");
  reopened_scenario.edge_domain = ClockDomainId::derive_from("test.clock.edge");
  reopened_scenario.reference_domain = core::reference_clock_domain();
  replay.records.push_back(make_record(reopened_scenario, spec));
  CHECK_OK(report, reopened->ingest(std::move(replay)));
  CHECK_EQ(report.rejected, std::uint64_t{1});
  CHECK_EQ(report.replays, std::uint64_t{1});
  CHECK(report.reason_counts.find(ReasonCode::SequenceReplay) != report.reason_counts.end());
  CHECK_OK_STATUS(reopened->shutdown());
}

LATOBS_TEST(store, truncated_segment_is_recovered_conservatively) {
  TempDir directory("store-truncated");
  {
    CHECK_OK(scenario, build_scenario(options_with_store(directory)));
    CHECK_EQ(ingest_batch(scenario, 12, 0, 1000000, 2000000).accepted_current, std::uint64_t{12});
    CHECK_OK_STATUS(scenario.engine->shutdown());
  }
  CHECK_OK(segments, store::list_segments(directory.path()));
  CHECK(!segments.empty());
  const std::size_t size = path_size(segments.back());
  CHECK(size > 16);
  {
    std::FILE* file = std::fopen(segments.back().string().c_str(), "r+b");
    CHECK(file != nullptr);
#if defined(_WIN32)
    const int truncate_result = _chsize_s(_fileno(file), static_cast<long long>(size - 16));
#else
    const int truncate_result = ftruncate(fileno(file), static_cast<off_t>(size - 16));
#endif
    std::fclose(file);
    CHECK_EQ(truncate_result, 0);
  }

  runtime::RuntimeConfig config;
  config.store_directory = directory.path();
  CHECK_OK(reopened, runtime::Engine::create(config));
  const store::RecoveryReport report = reopened->recovery();
  CHECK(report.truncated);
  CHECK(report.segments_rejected >= 1);
  CHECK(evidence_has_reason(report.reasons, ReasonCode::RecoveryConservative) ||
        evidence_has_reason(report.reasons, ReasonCode::RecordIntegrityFailure));
  CHECK_OK(recovered_path, reopened->catalog().path_by_name("test.path.alpha"));
  CHECK_OK(recovered_generation, reopened->catalog().generation_by_name("test.generation.one"));
  const stats::SummaryRequest request =
      make_summary_request(recovered_path->id, recovered_generation->id, 0, 100000000,
                           stats::AggregationMode::Historical);
  CHECK_OK(summary, reopened->summarize(request));
  CHECK(summary.exchanges_included <= 12);
  CHECK_OK_STATUS(reopened->shutdown());
}

LATOBS_TEST(store, corrupted_record_is_never_interpreted) {
  TempDir directory("store-corrupt");
  {
    CHECK_OK(scenario, build_scenario(options_with_store(directory)));
    CHECK_EQ(ingest_batch(scenario, 8, 0, 1000000, 2000000).accepted_current, std::uint64_t{8});
    CHECK_OK_STATUS(scenario.engine->shutdown());
  }
  CHECK_OK(segments, store::list_segments(directory.path()));
  CHECK(!segments.empty());
  {
    std::fstream file(segments.back(), std::ios::in | std::ios::out | std::ios::binary);
    CHECK(file.good());
    const std::size_t offset = path_size(segments.back()) / 2;
    file.seekg(static_cast<std::streamoff>(offset));
    char byte = 0;
    file.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x5A);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&byte, 1);
    file.close();
  }
  runtime::RuntimeConfig config;
  config.store_directory = directory.path();
  CHECK_OK(reopened, runtime::Engine::create(config));
  const store::RecoveryReport report = reopened->recovery();
  CHECK(report.truncated);
  CHECK(report.records_discarded >= 1);
  CHECK(report.records_read >= 1);
  CHECK_OK_STATUS(reopened->shutdown());
}

LATOBS_TEST(store, manifest_integrity_and_version_are_enforced) {
  TempDir directory("store-manifest");
  {
    CHECK_OK(scenario, build_scenario(options_with_store(directory)));
    CHECK_EQ(ingest_batch(scenario, 2, 0, 1000000, 2000000).accepted_current, std::uint64_t{2});
    CHECK_OK_STATUS(scenario.engine->shutdown());
  }
  const std::filesystem::path manifest = directory.path() / "manifest.lobs";
  std::string text;
  {
    std::ifstream stream(manifest, std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  CHECK(text.find("magic=LATOBS-STORE") != std::string::npos);
  CHECK(text.find("crc32c=") != std::string::npos);

  // A manifest whose content changed but whose checksum did not is refused.
  {
    std::string tampered = text;
    const std::size_t position = tampered.find("format=1");
    CHECK(position != std::string::npos);
    tampered.replace(position, 8, "format=9");
    std::ofstream stream(manifest, std::ios::binary | std::ios::trunc);
    stream << tampered;
  }
  runtime::RuntimeConfig config;
  config.store_directory = directory.path();
  CHECK_ERR(error, runtime::Engine::create(config));
  CHECK(error.code() == ErrorCode::IntegrityFailure);

  // A manifest claiming a newer format version is refused with a version error,
  // even though its integrity check is correct.
  {
    std::string newer = text;
    const std::size_t position = newer.find("format=1");
    newer.replace(position, 8, "format=2");
    const std::size_t crc_position = newer.find("crc32c=");
    const std::string body = newer.substr(0, crc_position);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%08x", core::crc32c(body.data(), body.size()));
    newer = body + "crc32c=" + buffer + "\n";
    std::ofstream stream(manifest, std::ios::binary | std::ios::trunc);
    stream << newer;
  }
  CHECK_ERR(version_error, runtime::Engine::create(config));
  CHECK(version_error.code() == ErrorCode::VersionMismatch);
}

LATOBS_TEST(store, segment_rotation_and_capacity_limits) {
  TempDir directory("store-rotation");
  runtime::RuntimeConfig config;
  config.store_directory = directory.path();
  config.policy.limits.max_segment_records = 8;
  config.policy.limits.max_segment_bytes = 4096;
  config.policy.limits.max_store_bytes = 1024 * 1024;
  CHECK_OK(engine, runtime::Engine::create(config));

  model::SourceDescriptor source;
  source.name = Name::assume_valid("rotation.source");
  source.kind = model::SourceKind::Probe;
  source.authority = model::AuthorityClass::Primary;
  source.semantics = model::SemanticsProfile::EndToEndRequestResponse;
  source.revision = Revision::first();
  CHECK_OK(source_id, engine->define_source(source));

  model::GenerationDef generation;
  generation.name = Name::assume_valid("rotation.generation");
  generation.revision = Revision::first();
  CHECK_OK(generation_id, engine->define_generation(generation));

  model::HopDef hop;
  hop.name = Name::assume_valid("rotation.hop");
  hop.kind = model::HopKind::Endpoint;
  hop.revision = Revision::first();
  CHECK_OK(hop_id, engine->define_hop(hop));

  model::PathDef path;
  path.name = Name::assume_valid("rotation.path");
  path.generation = generation_id;
  path.revision = Revision::first();
  path.hops = {hop_id};
  CHECK_OK(path_id, engine->define_path(path));

  for (std::uint64_t batch = 0; batch < 12; ++batch) {
    ingest::IngestRequest request;
    request.received_at = Timestamp{2000000, core::reference_clock_domain()};
    for (std::uint64_t index = 0; index < 4; ++index) {
      model::MeasurementRecord record;
      record.path = path_id;
      record.generation = generation_id;
      record.source = source_id;
      record.epoch = EpochId::derive_from("rotation.epoch");
      record.incarnation = IncarnationId::derive_from("rotation.incarnation");
      record.source_revision = Revision::first();
      record.sequence = Sequence::from_value(batch * 4 + index);
      record.domain = core::reference_clock_domain();
      record.request = Timestamp{1000000 + static_cast<std::int64_t>(batch), record.domain};
      record.response = Timestamp{1000100 + static_cast<std::int64_t>(batch), record.domain};
      record.rtt_ns = 100;
      record.stamp.observed_at = record.response;
      model::HopObservation observation;
      observation.index = HopIndex::from_validated_value(0);
      observation.hop = hop_id;
      observation.entry = record.request;
      observation.exit = record.response;
      observation.entry_domain = record.domain;
      observation.exit_domain = record.domain;
      record.hops.push_back(observation);
      request.records.push_back(std::move(record));
    }
    CHECK_OK(report, engine->ingest(std::move(request)));
    CHECK_EQ(report.rejected, std::uint64_t{0});
  }
  CHECK_OK_STATUS(engine->shutdown());
  CHECK_OK(segments, store::list_segments(directory.path()));
  CHECK(segments.size() > 1);
  for (const std::filesystem::path& segment : segments) {
    CHECK(path_size(segment) <= config.policy.limits.max_segment_bytes);
  }

  // A store that would exceed its configured size refuses new records instead of
  // dropping them silently.
  TempDir small_directory("store-capacity");
  runtime::RuntimeConfig small_config;
  small_config.store_directory = small_directory.path();
  small_config.policy.limits.max_store_bytes = 4096;
  small_config.policy.limits.max_segment_bytes = 4096;
  CHECK_OK(small_engine, runtime::Engine::create(small_config));
  bool refused = false;
  for (int attempt = 0; attempt < 60 && !refused; ++attempt) {
    model::SourceDescriptor filler;
    filler.name = Name::assume_valid("capacity.source." + std::to_string(attempt));
    filler.kind = model::SourceKind::Probe;
    filler.authority = model::AuthorityClass::Primary;
    filler.semantics = model::SemanticsProfile::EndToEndRequestResponse;
    filler.revision = Revision::first();
    const Result<SourceId> defined = small_engine->define_source(filler);
    if (!defined.has_value()) {
      refused = defined.error().code() == ErrorCode::CapacityExceeded;
    }
  }
  CHECK(refused);
  CHECK_OK_STATUS(small_engine->shutdown());
}

LATOBS_TEST_MAIN()