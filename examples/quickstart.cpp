// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Smallest useful program against the public API: define a path, ingest
// observations, summarize them and attribute the latency to hops.

#include <cstdio>
#include <string>

#include "latency_observatory/runtime/engine.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::model;
using namespace latobs::runtime;

namespace {

void print_distribution(const char* label, const stats::Distribution& distribution) {
  std::printf("%s: count=%llu", label,
              static_cast<unsigned long long>(distribution.count));
  if (distribution.mean_ns.has_value()) {
    std::printf(" mean=%lldns", static_cast<long long>(*distribution.mean_ns));
  } else {
    std::printf(" mean=unknown");
  }
  if (distribution.quantiles.size() > 2 && distribution.quantiles[2].has_value()) {
    std::printf(" p99=%lldns", static_cast<long long>(*distribution.quantiles[2]));
  } else {
    std::printf(" p99=unknown");
  }
  std::printf("\n");
}

}  // namespace

int main() {
  runtime::RuntimeConfig config;
  Result<std::unique_ptr<runtime::Engine>> created = runtime::Engine::create(config);
  if (!created.has_value()) {
    std::fprintf(stderr, "cannot create the runtime: %s\n", created.error().describe().c_str());
    return 1;
  }
  std::unique_ptr<runtime::Engine> engine = std::move(created.value());

  model::SourceDescriptor source;
  source.name = Name::assume_valid("example.source");
  source.kind = model::SourceKind::Probe;
  source.authority = model::AuthorityClass::Primary;
  source.semantics = model::SemanticsProfile::EndToEndRequestResponse |
                     model::SemanticsProfile::HopDwell;
  source.revision = Revision::first();
  const Result<SourceId> source_id = engine->define_source(source);
  if (!source_id.has_value()) return 1;

  model::GenerationDef generation;
  generation.name = Name::assume_valid("example.generation");
  generation.revision = Revision::first();
  const Result<GenerationId> generation_id = engine->define_generation(generation);
  if (!generation_id.has_value()) return 1;

  model::HopDef ingress;
  ingress.name = Name::assume_valid("example.hop.ingress");
  ingress.kind = model::HopKind::Endpoint;
  ingress.revision = Revision::first();
  const Result<HopId> ingress_id = engine->define_hop(ingress);
  if (!ingress_id.has_value()) return 1;

  model::HopDef egress;
  egress.name = Name::assume_valid("example.hop.egress");
  egress.kind = model::HopKind::Application;
  egress.revision = Revision::first();
  const Result<HopId> egress_id = engine->define_hop(egress);
  if (!egress_id.has_value()) return 1;

  model::PathDef path;
  path.name = Name::assume_valid("example.path");
  path.generation = generation_id.value();
  path.revision = Revision::first();
  path.hops = {ingress_id.value(), egress_id.value()};
  const Result<PathId> path_id = engine->define_path(path);
  if (!path_id.has_value()) return 1;

  // Four observations, submitted as one batch.
  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < 4; ++index) {
    model::MeasurementRecord record;
    record.path = path_id.value();
    record.generation = generation_id.value();
    record.source = source_id.value();
    record.epoch = EpochId::derive_from("example.epoch");
    record.incarnation = IncarnationId::derive_from("example.incarnation");
    record.source_revision = Revision::first();
    record.sequence = Sequence::from_value(index);
    record.domain = core::reference_clock_domain();
    const std::int64_t start = 1000000 + static_cast<std::int64_t>(index) * 1000;
    const std::int64_t ingress_dwell = 100 + static_cast<std::int64_t>(index) * 10;
    const std::int64_t egress_dwell = 200;
    record.request = Timestamp{start, record.domain};
    record.response = Timestamp{start + ingress_dwell + egress_dwell, record.domain};
    record.rtt_ns = record.response.ns - record.request.ns;
    record.stamp.observed_at = record.response;

    model::HopObservation first;
    first.index = HopIndex::from_validated_value(0);
    first.hop = ingress_id.value();
    first.entry = Timestamp{start, record.domain};
    first.exit = Timestamp{start + ingress_dwell, record.domain};
    first.entry_domain = record.domain;
    first.exit_domain = record.domain;
    record.hops.push_back(first);

    model::HopObservation second;
    second.index = HopIndex::from_validated_value(1);
    second.hop = egress_id.value();
    second.entry = Timestamp{start + ingress_dwell, record.domain};
    second.exit = Timestamp{start + ingress_dwell + egress_dwell, record.domain};
    second.entry_domain = record.domain;
    second.exit_domain = record.domain;
    record.hops.push_back(second);

    request.records.push_back(std::move(record));
  }

  const Result<ingest::IngestReport> report = engine->ingest(std::move(request));
  if (!report.has_value()) {
    std::fprintf(stderr, "ingest failed: %s\n", report.error().describe().c_str());
    return 1;
  }
  std::printf("accepted current=%llu historical=%llu rejected=%llu\n",
              static_cast<unsigned long long>(report.value().accepted_current),
              static_cast<unsigned long long>(report.value().accepted_historical),
              static_cast<unsigned long long>(report.value().rejected));

  stats::SummaryRequest summary_request;
  summary_request.path = path_id.value();
  summary_request.generation = generation_id.value();
  summary_request.mode = stats::AggregationMode::Current;
  summary_request.window = stats::TimeWindow::make(
                               Timestamp{0, core::reference_clock_domain()},
                               Timestamp{100000000, core::reference_clock_domain()})
                               .value();
  const Result<stats::PathSummary> summary = engine->summarize(summary_request);
  if (!summary.has_value()) return 1;
  print_distribution("end to end", summary.value().end_to_end);
  for (const stats::HopSummary& hop : summary.value().hops) {
    print_distribution("hop", hop.dwell);
  }

  attribute::AttributionRequest attribution_request;
  attribution_request.path = path_id.value();
  attribution_request.generation = generation_id.value();
  attribution_request.mode = stats::AggregationMode::Current;
  attribution_request.window = summary_request.window;
  const Result<attribute::AttributionResult> attribution = engine->attribute(attribution_request);
  if (!attribution.has_value()) return 1;
  std::printf("attribution kind=%s accounted=%lldns unaccounted=%lldns\n",
              std::string(attribute::to_string(attribution.value().kind)).c_str(),
              attribution.value().accounted_mean_ns.has_value()
                  ? static_cast<long long>(*attribution.value().accounted_mean_ns)
                  : -1,
              attribution.value().unaccounted_mean_ns.has_value()
                  ? static_cast<long long>(*attribution.value().unaccounted_mean_ns)
                  : -1);
  for (const attribute::HopContribution& contribution : attribution.value().contributions) {
    std::printf("  hop %u share=%llu ppm coverage=%llu ppm\n",
                static_cast<unsigned>(contribution.index.value()),
                static_cast<unsigned long long>(contribution.share_ppm),
                static_cast<unsigned long long>(contribution.coverage_ppm));
  }
  const Status shutdown = engine->shutdown();
  return shutdown.ok() ? 0 : 1;
}
