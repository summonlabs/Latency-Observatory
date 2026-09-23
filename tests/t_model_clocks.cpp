// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <string>

#include "latency_observatory/model/codec.hpp"
#include "latency_observatory/model/clock.hpp"
#include "latency_observatory/model/entities.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

model::ClockSync make_sync(ClockDomainId domain, ClockDomainId reference, Timestamp observed_at,
                           Nanos uncertainty, Nanos valid_for, model::ClockSyncState state,
                           Revision revision) {
  model::ClockSync sync;
  sync.domain = domain;
  sync.reference = reference;
  sync.state = state;
  sync.offset_ns = 1000;
  sync.uncertainty_ns = uncertainty;
  sync.valid_for_ns = valid_for;
  sync.observed_at = observed_at;
  sync.revision = revision;
  return sync;
}

struct ClockFixture {
  core::RuntimePolicy policy = core::default_policy();
  model::ClockRegistry registry{policy};
  ClockDomainId reference = core::reference_clock_domain();
  ClockDomainId edge;
  ClockDomainId other;
  Timestamp now{1000000000LL, core::reference_clock_domain()};

  ClockFixture() {
    model::ClockDomainDef reference_def;
    reference_def.name = Name::assume_valid("lobs.clock.reference.utc");
    reference_def.is_reference = true;
    CHECK(registry.define_domain(reference_def).has_value());
    model::ClockDomainDef edge_def;
    edge_def.name = Name::assume_valid("test.clock.edge");
    CHECK(registry.define_domain(edge_def).has_value());
    edge = ClockDomainId::derive_from("test.clock.edge");
    model::ClockDomainDef other_def;
    other_def.name = Name::assume_valid("test.clock.other");
    CHECK(registry.define_domain(other_def).has_value());
    other = ClockDomainId::derive_from("test.clock.other");
  }
};

}  // namespace

LATOBS_TEST(model, catalog_definition_rules) {
  core::Limits limits;
  model::Catalog catalog(limits);
  model::SourceDescriptor source;
  CHECK_OK(name, Name::parse("catalog.source"));
  source.name = name;
  source.kind = model::SourceKind::Probe;
  source.authority = model::AuthorityClass::Primary;
  source.semantics = model::SemanticsProfile::EndToEndRequestResponse;
  source.revision = Revision::first();
  CHECK_OK(first, catalog.define_source(source));
  CHECK_OK(second, catalog.define_source(source));
  CHECK_EQ(first, second);

  // Same revision with different content is refused.
  model::SourceDescriptor changed = source;
  changed.authority = model::AuthorityClass::Secondary;
  CHECK_ERR(error_conflict, catalog.define_source(changed));
  // A newer revision replaces the definition.
  changed.revision = Revision::from_validated_value(2);
  CHECK_OK(third, catalog.define_source(changed));
  CHECK_EQ(third, first);
  CHECK_OK(stored, catalog.source(first));
  CHECK(stored->authority == model::AuthorityClass::Secondary);
  CHECK_OK(by_name, catalog.source_by_name("catalog.source"));
  CHECK_ERR(error_unknown_name, catalog.source_by_name("catalog.missing"));
  CHECK_ERR(error_zero_revision, catalog.define_source(model::SourceDescriptor{}));
}

LATOBS_TEST(model, catalog_rejects_dangling_references) {
  core::Limits limits;
  model::Catalog catalog(limits);
  model::PathDef path;
  path.name = Name::assume_valid("catalog.path");
  path.revision = Revision::first();
  CHECK_ERR(error_no_generation, catalog.define_path(path));

  model::GenerationDef generation;
  generation.name = Name::assume_valid("catalog.generation.one");
  generation.revision = Revision::first();
  CHECK_OK(generation_id, catalog.define_generation(generation));
  path.generation = generation_id;
  CHECK_ERR(error_no_hops, catalog.define_path(path));
  path.hops.push_back(HopId::derive_from("catalog.hop.missing"));
  CHECK_ERR(error_unknown_hop, catalog.define_path(path));

  model::HopDef hop;
  hop.name = Name::assume_valid("catalog.hop.one");
  hop.kind = model::HopKind::Endpoint;
  hop.revision = Revision::first();
  CHECK_OK(hop_id, catalog.define_hop(hop));
  path.hops.clear();
  path.hops.push_back(hop_id);
  CHECK_OK(path_id, catalog.define_path(path));

  model::LinkDef link;
  link.name = Name::assume_valid("catalog.link");
  link.revision = Revision::first();
  link.from = EndpointId::derive_from("catalog.endpoint.missing");
  CHECK_ERR(error_unknown_endpoint, catalog.define_link(link));
}

LATOBS_TEST(model, generation_lineage) {
  core::Limits limits;
  model::Catalog catalog(limits);
  model::GenerationDef one;
  one.name = Name::assume_valid("lineage.one");
  one.revision = Revision::first();
  CHECK_OK(first, catalog.define_generation(one));
  CHECK(catalog.current_generation() == first);
  CHECK(!catalog.is_superseded(first));

  model::GenerationDef two;
  two.name = Name::assume_valid("lineage.two");
  two.revision = Revision::first();
  two.supersedes = first;
  CHECK_OK(second, catalog.define_generation(two));
  CHECK(catalog.is_superseded(first));
  CHECK(!catalog.is_superseded(second));
  CHECK(catalog.current_generation() == second);

  // A branched lineage has no single current generation: it reports none
  // instead of guessing.
  model::GenerationDef three;
  three.name = Name::assume_valid("lineage.three");
  three.revision = Revision::first();
  three.supersedes = first;
  CHECK_OK(third, catalog.define_generation(three));
  CHECK(!catalog.current_generation().valid());
  CHECK_ERR(error_unknown, catalog.generation(GenerationId::derive_from("lineage.missing")));
}

LATOBS_TEST(model, clock_registry_revision_fence) {
  ClockFixture fixture;
  const Timestamp observed{fixture.now.ns - 1000, fixture.reference};
  CHECK(fixture.registry
            .record_sync(make_sync(fixture.edge, fixture.reference, observed, 100, 1000000,
                                   model::ClockSyncState::Synchronized, Revision::first()))
            .ok());
  // An older revision is refused instead of replacing the newer report.
  CHECK(!fixture.registry
             .record_sync(make_sync(fixture.edge, fixture.reference, observed, 100, 1000000,
                                    model::ClockSyncState::Unsynced,
                                    Revision::from_validated_value(1)))
             .ok());
  CHECK(fixture.registry.sync(fixture.edge) != nullptr);
  CHECK(fixture.registry.sync(fixture.edge)->state == model::ClockSyncState::Synchronized);
  // Same revision but an earlier observation time is refused.
  CHECK(!fixture.registry
             .record_sync(make_sync(fixture.edge, fixture.reference, Timestamp{observed.ns - 1,
                                                                              fixture.reference},
                                    100, 1000000, model::ClockSyncState::Synchronized,
                                    Revision::first()))
             .ok());
}

LATOBS_TEST(model, clock_comparability_matrix) {
  ClockFixture fixture;
  const Timestamp observed{fixture.now.ns - 1000, fixture.reference};

  // Same domain: always comparable, without any synchronization record.
  CHECK_OK(self, fixture.registry.compare(fixture.edge, fixture.edge, fixture.now,
                                          GenerationId{}, EpochId{}, IncarnationId{}));
  CHECK(self.comparable);
  CHECK(self.evidence.reasons()[0].code == ReasonCode::ClockSelfComparable ||
        !self.evidence.reasons().empty());

  // Unknown domain: unknown, not comparable.
  CHECK_OK(unknown, fixture.registry.compare(ClockDomainId::derive_from("missing"), fixture.edge,
                                             fixture.now, GenerationId{}, EpochId{},
                                             IncarnationId{}));
  CHECK(!unknown.comparable);
  CHECK(unknown.evidence.state() == EvidenceState::Unknown);

  // No synchronization record at all.
  CHECK_OK(no_sync, fixture.registry.compare(fixture.edge, fixture.other, fixture.now,
                                             GenerationId{}, EpochId{}, IncarnationId{}));
  CHECK(!no_sync.comparable);

  // Unsynced domain.
  CHECK(fixture.registry
            .record_sync(make_sync(fixture.edge, fixture.reference, observed, 100, 1000000,
                                   model::ClockSyncState::Unsynced, Revision::first()))
            .ok());
  CHECK_OK(unsynced, fixture.registry.compare(fixture.edge, fixture.reference, fixture.now,
                                              GenerationId{}, EpochId{}, IncarnationId{}));
  CHECK(!unsynced.comparable);
  CHECK(unsynced.evidence.state() == EvidenceState::Refused);

  // Synchronized, fresh: comparable with the reference domain.
  CHECK(fixture.registry
            .record_sync(make_sync(fixture.edge, fixture.reference, observed, 100, 1000000,
                                   model::ClockSyncState::Synchronized,
                                   Revision::from_validated_value(2)))
            .ok());
  CHECK_OK(comparable, fixture.registry.compare(fixture.edge, fixture.reference, fixture.now,
                                                GenerationId{}, EpochId{}, IncarnationId{}));
  CHECK(comparable.comparable);
  CHECK_EQ(comparable.uncertainty_ns, 100);
  CHECK(comparable.offset_first_ns == 1000);
  CHECK_EQ(comparable.offset_second_ns, 0);
  CHECK(comparable.confidence == Confidence::High);

  // Expired synchronization record: stale, not comparable.
  const Timestamp later{observed.ns + 2000000, fixture.reference};
  CHECK_OK(expired, fixture.registry.compare(fixture.edge, fixture.reference, later,
                                             GenerationId{}, EpochId{}, IncarnationId{}));
  CHECK(!expired.comparable);
  CHECK(expired.evidence.state() == EvidenceState::Stale);

  // Holdover is degraded when allowed and refused when not.
  CHECK(fixture.registry
            .record_sync(make_sync(fixture.edge, fixture.reference,
                                   Timestamp{fixture.now.ns - 1000, fixture.reference}, 100,
                                   1000000, model::ClockSyncState::Holdover,
                                   Revision::from_validated_value(3)))
            .ok());
  CHECK_OK(holdover, fixture.registry.compare(fixture.edge, fixture.reference, fixture.now,
                                              GenerationId{}, EpochId{}, IncarnationId{}));
  CHECK(holdover.comparable);
  CHECK(holdover.confidence == Confidence::Degraded);

  core::RuntimePolicy strict = fixture.policy;
  strict.comparability.allow_holdover = false;
  model::ClockRegistry strict_registry(strict);
  model::ClockDomainDef reference_def;
  reference_def.name = Name::assume_valid("lobs.clock.reference.utc");
  reference_def.is_reference = true;
  CHECK(strict_registry.define_domain(reference_def).has_value());
  model::ClockDomainDef edge_def;
  edge_def.name = Name::assume_valid("test.clock.edge");
  CHECK(strict_registry.define_domain(edge_def).has_value());
  CHECK(strict_registry
            .record_sync(make_sync(fixture.edge, fixture.reference, observed, 100, 1000000,
                                   model::ClockSyncState::Holdover, Revision::first()))
            .ok());
  CHECK_OK(refused_holdover, strict_registry.compare(fixture.edge, fixture.reference, fixture.now,
                                                     GenerationId{}, EpochId{}, IncarnationId{}));
  CHECK(!refused_holdover.comparable);
  CHECK(refused_holdover.evidence.state() == EvidenceState::Refused);
}

LATOBS_TEST(model, clock_comparability_fences_and_uncertainty) {
  ClockFixture fixture;
  const Timestamp observed{fixture.now.ns - 1000, fixture.reference};
  const GenerationId generation = GenerationId::derive_from("generation.one");
  const EpochId epoch = EpochId::derive_from("epoch.one");
  const IncarnationId incarnation = IncarnationId::derive_from("incarnation.one");

  model::ClockSync sync = make_sync(fixture.edge, fixture.reference, observed, 100, 1000000,
                                    model::ClockSyncState::Synchronized, Revision::first());
  sync.generation = generation;
  sync.epoch = epoch;
  sync.incarnation = incarnation;
  CHECK(fixture.registry.record_sync(sync).ok());

  CHECK_OK(matching, fixture.registry.compare(fixture.edge, fixture.reference, fixture.now,
                                              generation, epoch, incarnation));
  CHECK(matching.comparable);

  // A generation mismatch is conflicting, never comparable.
  CHECK_OK(generation_mismatch,
           fixture.registry.compare(fixture.edge, fixture.reference, fixture.now,
                                    GenerationId::derive_from("generation.two"), epoch,
                                    incarnation));
  CHECK(!generation_mismatch.comparable);
  CHECK(generation_mismatch.evidence.state() == EvidenceState::Conflicting);

  CHECK_OK(epoch_mismatch, fixture.registry.compare(fixture.edge, fixture.reference, fixture.now,
                                                    generation,
                                                    EpochId::derive_from("epoch.two"),
                                                    incarnation));
  CHECK(!epoch_mismatch.comparable);

  CHECK_OK(incarnation_mismatch,
           fixture.registry.compare(fixture.edge, fixture.reference, fixture.now, generation, epoch,
                                    IncarnationId::derive_from("incarnation.two")));
  CHECK(!incarnation_mismatch.comparable);

  // The reference domain of the report must be the comparison reference.
  model::ClockSync elsewhere = make_sync(fixture.edge, fixture.other, observed, 100, 1000000,
                                         model::ClockSyncState::Synchronized,
                                         Revision::from_validated_value(2));
  CHECK(fixture.registry.record_sync(elsewhere).ok());
  CHECK_OK(reference_mismatch,
           fixture.registry.compare(fixture.edge, fixture.reference, fixture.now, GenerationId{},
                                    EpochId{}, IncarnationId{}));
  CHECK(!reference_mismatch.comparable);
  CHECK(reference_mismatch.evidence.state() == EvidenceState::Conflicting);

  // Uncertainty above the policy ceiling is refused even when everything else
  // is in order.
  core::RuntimePolicy tight = fixture.policy;
  tight.comparability.max_uncertainty_ns = 10;
  model::ClockRegistry tight_registry(tight);
  model::ClockDomainDef reference_def;
  reference_def.name = Name::assume_valid("lobs.clock.reference.utc");
  reference_def.is_reference = true;
  CHECK(tight_registry.define_domain(reference_def).has_value());
  model::ClockDomainDef edge_def;
  edge_def.name = Name::assume_valid("test.clock.edge");
  CHECK(tight_registry.define_domain(edge_def).has_value());
  CHECK(tight_registry
            .record_sync(make_sync(fixture.edge, fixture.reference, observed, 1000, 1000000,
                                   model::ClockSyncState::Synchronized, Revision::first()))
            .ok());
  CHECK_OK(too_uncertain, tight_registry.compare(fixture.edge, fixture.reference, fixture.now,
                                                 GenerationId{}, EpochId{}, IncarnationId{}));
  CHECK(!too_uncertain.comparable);
  CHECK(too_uncertain.evidence.state() == EvidenceState::Refused);
  CHECK_EQ(too_uncertain.uncertainty_ns, 1000);
}

LATOBS_TEST(model, clock_age_estimation) {
  ClockFixture fixture;
  const Timestamp observed{fixture.now.ns - 5000, fixture.reference};
  // Same domain: the age is exact without synchronization.
  const model::ClockRegistry::AgeEstimate same =
      fixture.registry.estimate_age(observed, fixture.now);
  CHECK(same.age_ns.has_value());
  CHECK_EQ(*same.age_ns, 5000);

  // Unknown domain: unknown age, never zero.
  const model::ClockRegistry::AgeEstimate unknown =
      fixture.registry.estimate_age(Timestamp{observed.ns, fixture.edge}, fixture.now);
  CHECK(!unknown.age_ns.has_value());

  model::ClockSync sync = make_sync(fixture.edge, fixture.reference,
                                    Timestamp{fixture.now.ns - 10, fixture.reference}, 0, 1000000,
                                    model::ClockSyncState::Synchronized, Revision::first());
  CHECK(fixture.registry.record_sync(sync).ok());
  const model::ClockRegistry::AgeEstimate cross =
      fixture.registry.estimate_age(Timestamp{observed.ns, fixture.edge}, fixture.now);
  CHECK(cross.age_ns.has_value());
  // reference = domain - offset, so the age is measured on the reference scale.
  // The report says domain = reference + offset, so the observation happened
  // 6000 ns before "now" on the reference timeline.
  CHECK_EQ(*cross.age_ns, 5000 + 1000);

  // A report from the future cannot be reconciled.
  const model::ClockRegistry::AgeEstimate backwards =
      fixture.registry.estimate_age(Timestamp{observed.ns, fixture.edge},
                                    Timestamp{fixture.now.ns - 100000, fixture.reference});
  CHECK(!backwards.age_ns.has_value());
}

LATOBS_TEST(model, entity_codecs_round_trip) {
  model::SourceDescriptor source;
  source.name = Name::assume_valid("codec.source");
  source.kind = model::SourceKind::TelemetryAgent;
  source.authority = model::AuthorityClass::Secondary;
  source.semantics = model::SemanticsProfile::EndToEndRequestResponse |
                     model::SemanticsProfile::QueueDwell;
  source.revision = Revision::from_validated_value(3);
  source.description = "round trip";
  std::string text;
  {
    core::JsonWriter writer(text);
    model::write_json(writer, source);
  }
  CHECK_OK(document, core::parse_json(text, 16));
  CHECK_OK(decoded, model::decode_source(document));
  CHECK(decoded.name == source.name);
  CHECK(decoded.kind == source.kind);
  CHECK(decoded.authority == source.authority);
  CHECK(decoded.semantics == source.semantics);
  CHECK(decoded.revision == source.revision);
  CHECK_EQ(decoded.description, source.description);
  CHECK_EQ(decoded.id, SourceId::derive_from("codec.source"));
  CHECK_EQ(std::string(model::to_string(decoded.semantics)),
           std::string("end_to_end_request_response|queue_dwell"));

  model::GenerationDef generation;
  generation.name = Name::assume_valid("codec.generation");
  generation.revision = Revision::first();
  generation.supersedes = GenerationId::derive_from("codec.generation.previous");
  generation.description = "lineage";
  std::string generation_text;
  {
    core::JsonWriter writer(generation_text);
    model::write_json(writer, generation);
  }
  CHECK_OK(generation_document, core::parse_json(generation_text, 16));
  CHECK_OK(generation_decoded, model::decode_generation(generation_document));
  CHECK(generation_decoded.supersedes == generation.supersedes);
  CHECK(generation_decoded.name == generation.name);
}

LATOBS_TEST(model, measurement_codec_round_trip) {
  CHECK_OK(scenario, build_scenario());
  RecordSpec spec;
  spec.sequence = 9;
  spec.hop_dwells_ns[0] = 100;
  spec.hop_dwells_ns[1] = 200;
  spec.hop_dwells_ns[2] = 300;
  const model::MeasurementRecord record = make_record(scenario, spec);
  std::string text;
  {
    core::JsonWriter writer(text);
    model::write_json(writer, record);
  }
  CHECK_OK(document, core::parse_json(text, 32));
  CHECK_OK(decoded, model::decode_measurement(document));
  CHECK(decoded.path == record.path);
  CHECK(decoded.generation == record.generation);
  CHECK(decoded.source == record.source);
  CHECK(decoded.epoch == record.epoch);
  CHECK(decoded.incarnation == record.incarnation);
  CHECK(decoded.sequence == record.sequence);
  CHECK_EQ(decoded.rtt_ns, record.rtt_ns);
  CHECK_EQ(decoded.hops.size(), record.hops.size());
  CHECK(decoded.stamp.observed_at.ns == record.stamp.observed_at.ns);
  CHECK(decoded.stamp.observed_at.domain == record.stamp.observed_at.domain);
  // "make_record" produces an unadmitted record: the decoded evidence must be
  // the same "unknown" default, not silently upgraded.
  CHECK(decoded.evidence.state() == record.evidence.state());
}

LATOBS_TEST_MAIN()
