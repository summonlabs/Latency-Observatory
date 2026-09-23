// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A downstream consumer of the installed package: it uses only public headers
// and the exported target, and it fails when the runtime does not behave.

#include <cstdio>
#include <memory>
#include <string>

#include "latency_observatory/runtime/engine.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::model;
using namespace latobs::runtime;

int main() {
  RuntimeConfig config;
  Result<std::unique_ptr<Engine>> created = Engine::create(config);
  if (!created.has_value()) {
    std::fprintf(stderr, "engine creation failed: %s\n", created.error().describe().c_str());
    return 1;
  }
  std::unique_ptr<Engine> engine = std::move(created.value());

  SourceDescriptor source;
  source.name = Name::assume_valid("consumer.source");
  source.kind = SourceKind::Probe;
  source.authority = AuthorityClass::Primary;
  source.semantics = SemanticsProfile::EndToEndRequestResponse | SemanticsProfile::HopDwell;
  source.revision = Revision::first();
  Result<SourceId> source_id = engine->define_source(source);
  if (!source_id.has_value()) return 1;

  GenerationDef generation;
  generation.name = Name::assume_valid("consumer.generation");
  generation.revision = Revision::first();
  Result<GenerationId> generation_id = engine->define_generation(generation);
  if (!generation_id.has_value()) return 1;

  HopDef hop;
  hop.name = Name::assume_valid("consumer.hop");
  hop.kind = HopKind::Endpoint;
  hop.revision = Revision::first();
  Result<HopId> hop_id = engine->define_hop(hop);
  if (!hop_id.has_value()) return 1;

  PathDef path;
  path.name = Name::assume_valid("consumer.path");
  path.generation = generation_id.value();
  path.revision = Revision::first();
  path.hops = {hop_id.value()};
  Result<PathId> path_id = engine->define_path(path);
  if (!path_id.has_value()) return 1;

  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, reference_clock_domain()};
  for (std::uint64_t index = 0; index < 4; ++index) {
    MeasurementRecord record;
    record.path = path_id.value();
    record.generation = generation_id.value();
    record.source = source_id.value();
    record.epoch = EpochId::derive_from("consumer.epoch");
    record.incarnation = IncarnationId::derive_from("consumer.incarnation");
    record.source_revision = Revision::first();
    record.sequence = Sequence::from_value(index);
    record.domain = reference_clock_domain();
    const std::int64_t start = 1000000 + static_cast<std::int64_t>(index);
    record.request = Timestamp{start, record.domain};
    record.response = Timestamp{start + 400, record.domain};
    record.rtt_ns = 400;
    record.stamp.observed_at = record.response;
    HopObservation observation;
    observation.index = HopIndex::from_validated_value(0);
    observation.hop = hop_id.value();
    observation.entry = record.request;
    observation.exit = record.response;
    observation.entry_domain = record.domain;
    observation.exit_domain = record.domain;
    record.hops.push_back(observation);
    request.records.push_back(std::move(record));
  }
  Result<ingest::IngestReport> report = engine->ingest(std::move(request));
  if (!report.has_value() || report.value().accepted_current != 4) {
    std::fprintf(stderr, "ingest did not accept the batch\n");
    return 1;
  }

  stats::SummaryRequest summary_request;
  summary_request.path = path_id.value();
  summary_request.generation = generation_id.value();
  summary_request.window = stats::TimeWindow::make(Timestamp{0, reference_clock_domain()},
                                                   Timestamp{100000000, reference_clock_domain()})
                               .value();
  Result<stats::PathSummary> summary = engine->summarize(summary_request);
  if (!summary.has_value() || summary.value().end_to_end.count != 4) {
    std::fprintf(stderr, "summary is not complete\n");
    return 1;
  }
  std::printf("consumer: version=%s exchanges=%llu mean_ns=%lld\n",
              std::string(LATOBS_CONSUMER_VERSION).c_str(),
              static_cast<unsigned long long>(summary.value().exchanges_included),
              static_cast<long long>(*summary.value().end_to_end.mean_ns));
  const Status shutdown = engine->shutdown();
  return shutdown.ok() ? 0 : 1;
}
