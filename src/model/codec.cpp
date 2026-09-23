// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/model/codec.hpp"

#include "latency_observatory/core/checked.hpp"

namespace latobs::model {

void write_json(core::JsonWriter& writer, const Timestamp& timestamp) {
  writer.begin_object();
  writer.field("ns", timestamp.ns);
  writer.field("domain", timestamp.domain.valid() ? timestamp.domain.to_hex() : std::string());
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const core::Evidence& evidence) {
  writer.begin_object();
  writer.field("state", core::to_string(evidence.state()));
  writer.field("freshness", core::to_string(evidence.freshness()));
  writer.field("confidence", core::to_string(evidence.confidence()));
  writer.field("reasons_truncated", evidence.reasons_truncated());
  writer.field_array("reasons");
  for (const core::Reason& reason : evidence.reasons()) {
    writer.begin_object();
    writer.field("code", core::to_string(reason.code));
    writer.field("detail", reason.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const ObservationStamp& stamp) {
  writer.begin_object();
  writer.key("observed_at");
  write_json(writer, stamp.observed_at);
  writer.key("received_at");
  write_json(writer, stamp.received_at);
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const HopObservation& hop) {
  writer.begin_object();
  writer.field("index", static_cast<std::uint64_t>(hop.index.value()));
  writer.field("hop", hop.hop.to_hex());
  writer.field("link", hop.link.valid() ? hop.link.to_hex() : std::string());
  writer.field("queue", hop.queue.valid() ? hop.queue.to_hex() : std::string());
  writer.key("entry");
  write_json(writer, hop.entry);
  writer.key("exit");
  write_json(writer, hop.exit);
  writer.key("queue_entry");
  write_json(writer, hop.queue_entry);
  writer.key("queue_exit");
  write_json(writer, hop.queue_exit);
  writer.field_optional_int("dwell_ns", hop.dwell_ns);
  writer.field_optional_int("queue_dwell_ns", hop.queue_dwell_ns);
  writer.field_optional_int("dwell_uncertainty_ns", hop.dwell_uncertainty_ns);
  writer.field_optional_int("queue_dwell_uncertainty_ns", hop.queue_dwell_uncertainty_ns);
  writer.key("evidence");
  write_json(writer, hop.evidence);
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const MeasurementRecord& record) {
  writer.begin_object();
  writer.field("kind", "measurement");
  // The identity is always emitted, derived from the reporting triple and the
  // sequence when the record did not carry one yet.
  writer.field("id", record.id.valid()
                         ? record.id.to_hex()
                         : derive_measurement_id(record.source, record.epoch, record.incarnation,
                                                 record.sequence)
                               .to_hex());
  writer.field("identity_derived", !record.id.valid());
  writer.field("path", record.path.to_hex());
  writer.field("generation", record.generation.to_hex());
  writer.field("source", record.source.to_hex());
  writer.field("epoch", record.epoch.to_hex());
  writer.field("incarnation", record.incarnation.to_hex());
  writer.field("source_revision", static_cast<std::uint64_t>(record.source_revision.value()));
  writer.field("sequence", record.sequence.value());
  writer.field("synthetic", record.synthetic);
  writer.key("request");
  write_json(writer, record.request);
  writer.key("response");
  write_json(writer, record.response);
  writer.field("rtt_ns", record.rtt_ns);
  writer.key("stamp");
  write_json(writer, record.stamp);
  writer.key("evidence");
  write_json(writer, record.evidence);
  writer.field_array("hops");
  for (const HopObservation& hop : record.hops) write_json(writer, hop);
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const SourceDescriptor& source) {
  writer.begin_object();
  writer.field("kind", "source");
  writer.field("id", source.id.to_hex());
  writer.field("name", source.name.str());
  writer.field("source_kind", to_string(source.kind));
  writer.field("authority", to_string(source.authority));
  writer.field("semantics", to_string(source.semantics));
  writer.field("revision", static_cast<std::uint64_t>(source.revision.value()));
  writer.field("description", source.description);
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const EndpointDef& endpoint) {
  writer.begin_object();
  writer.field("kind", "endpoint");
  writer.field("id", endpoint.id.to_hex());
  writer.field("name", endpoint.name.str());
  writer.field("role", to_string(endpoint.role));
  writer.field("revision", static_cast<std::uint64_t>(endpoint.revision.value()));
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const LinkDef& link) {
  writer.begin_object();
  writer.field("kind", "link");
  writer.field("id", link.id.to_hex());
  writer.field("name", link.name.str());
  writer.field("from", link.from.valid() ? link.from.to_hex() : std::string());
  writer.field("to", link.to.valid() ? link.to.to_hex() : std::string());
  writer.field("revision", static_cast<std::uint64_t>(link.revision.value()));
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const QueueDef& queue) {
  writer.begin_object();
  writer.field("kind", "queue");
  writer.field("id", queue.id.to_hex());
  writer.field("name", queue.name.str());
  writer.field("link", queue.link.valid() ? queue.link.to_hex() : std::string());
  writer.field("semantics", to_string(queue.semantics));
  writer.field("revision", static_cast<std::uint64_t>(queue.revision.value()));
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const HopDef& hop) {
  writer.begin_object();
  writer.field("kind", "hop");
  writer.field("id", hop.id.to_hex());
  writer.field("name", hop.name.str());
  writer.field("hop_kind", to_string(hop.kind));
  writer.field("link", hop.link.valid() ? hop.link.to_hex() : std::string());
  writer.field("queue", hop.queue.valid() ? hop.queue.to_hex() : std::string());
  writer.field("revision", static_cast<std::uint64_t>(hop.revision.value()));
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const GenerationDef& generation) {
  writer.begin_object();
  writer.field("kind", "generation");
  writer.field("id", generation.id.to_hex());
  writer.field("name", generation.name.str());
  writer.field("revision", static_cast<std::uint64_t>(generation.revision.value()));
  writer.field("supersedes",
               generation.supersedes.valid() ? generation.supersedes.to_hex() : std::string());
  writer.field("description", generation.description);
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const PathDef& path) {
  writer.begin_object();
  writer.field("kind", "path");
  writer.field("id", path.id.to_hex());
  writer.field("name", path.name.str());
  writer.field("generation", path.generation.to_hex());
  writer.field("revision", static_cast<std::uint64_t>(path.revision.value()));
  writer.field_array("hops");
  for (const HopId hop : path.hops) writer.value_string(hop.to_hex());
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const ClockDomainDef& domain) {
  writer.begin_object();
  writer.field("kind", "clock_domain");
  writer.field("id", domain.id.to_hex());
  writer.field("name", domain.name.str());
  writer.field("is_reference", domain.is_reference);
  writer.field("description", domain.description);
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const ClockSync& sync) {
  writer.begin_object();
  writer.field("kind", "clock_sync");
  writer.field("domain", sync.domain.to_hex());
  writer.field("reference", sync.reference.valid() ? sync.reference.to_hex() : std::string());
  writer.field("state", to_string(sync.state));
  writer.field("offset_ns", sync.offset_ns);
  writer.field("skew_ppb", sync.skew_ppb);
  writer.field("uncertainty_ns", sync.uncertainty_ns);
  writer.field("valid_for_ns", sync.valid_for_ns);
  writer.field("observed_at_ns", sync.observed_at.ns);
  writer.field("observed_at_domain",
               sync.observed_at.domain.valid() ? sync.observed_at.domain.to_hex() : std::string());
  writer.field("reported_by", sync.reported_by.valid() ? sync.reported_by.to_hex() : std::string());
  writer.field("generation", sync.generation.valid() ? sync.generation.to_hex() : std::string());
  writer.field("epoch", sync.epoch.valid() ? sync.epoch.to_hex() : std::string());
  writer.field("incarnation",
               sync.incarnation.valid() ? sync.incarnation.to_hex() : std::string());
  writer.field("revision", static_cast<std::uint64_t>(sync.revision.value()));
  writer.end_object();
}

// ---------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------

Result<core::Name> json_name(const core::JsonValue& object, std::string_view field) {
  LATOBS_TRY(text, core::json_require_string(object, field));
  return core::Name::parse(text);
}

Result<Revision> json_revision(const core::JsonValue& object, std::string_view field) {
  LATOBS_TRY(value, core::json_require_uint(object, field));
  if (value == 0 || value > 0xFFFFFFFFULL) {
    return Error(ErrorCode::OutOfRange, "revision is outside the supported range",
                 std::string(field));
  }
  return Revision::from_value(static_cast<std::uint32_t>(value));
}

Result<std::string> json_id_hex(const core::JsonValue& object, std::string_view field) {
  LATOBS_TRY(text, core::json_require_string(object, field));
  return std::string(text);
}

std::optional<std::string> json_optional_id_hex(const core::JsonValue& object,
                                                std::string_view field) {
  const core::JsonValue* value = object.find(field);
  if (value == nullptr || value->is_null()) return std::nullopt;
  const Result<std::string_view> text = value->as_string();
  if (!text.has_value() || text.value().empty()) return std::nullopt;
  return std::string(text.value());
}

Result<Timestamp> decode_timestamp(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "timestamp must be an object");
  }
  LATOBS_TRY(ns, core::json_require_int(value, "ns"));
  LATOBS_TRY(domain_text, core::json_require_string(value, "domain"));
  Timestamp timestamp;
  timestamp.ns = ns;
  if (!domain_text.empty()) {
    LATOBS_TRY(domain, ClockDomainId::parse_hex(domain_text));
    timestamp.domain = domain;
  }
  return timestamp;
}

Result<core::Evidence> decode_evidence(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "evidence must be an object");
  }
  LATOBS_TRY(state_text, core::json_require_string(value, "state"));
  core::EvidenceState state = core::EvidenceState::Unknown;
  if (!core::parse_evidence_state(state_text, state)) {
    return Error(ErrorCode::ParseError, "unknown evidence state", std::string(state_text));
  }
  core::Evidence evidence;
  evidence.set_state(state);
  if (const core::JsonValue* freshness = value.find("freshness"); freshness != nullptr) {
    LATOBS_TRY(freshness_text, freshness->as_string());
    core::Freshness parsed = core::Freshness::Unknown;
    if (!core::parse_freshness(freshness_text, parsed)) {
      return Error(ErrorCode::ParseError, "unknown freshness", std::string(freshness_text));
    }
    evidence.set_freshness(parsed);
  }
  if (const core::JsonValue* confidence = value.find("confidence"); confidence != nullptr) {
    LATOBS_TRY(confidence_text, confidence->as_string());
    core::Confidence parsed = core::Confidence::None;
    if (!core::parse_confidence(confidence_text, parsed)) {
      return Error(ErrorCode::ParseError, "unknown confidence", std::string(confidence_text));
    }
    evidence.set_confidence(parsed);
  }
  if (const core::JsonValue* reasons = value.find("reasons"); reasons != nullptr) {
    LATOBS_TRY(list, reasons->as_array());
    for (const core::JsonValue& entry : *list) {
      LATOBS_TRY(code_text, core::json_require_string(entry, "code"));
      core::ReasonCode code = core::ReasonCode::MalformedPayload;
      if (!core::parse_reason_code(code_text, code)) {
        return Error(ErrorCode::ParseError, "unknown reason code", std::string(code_text));
      }
      std::string detail;
      if (const core::JsonValue* detail_value = entry.find("detail");
          detail_value != nullptr && detail_value->is_string()) {
        LATOBS_TRY(detail_text, detail_value->as_string());
        detail = std::string(detail_text);
      }
      evidence.add_reason(code, std::move(detail));
    }
  }
  return evidence;
}

Result<ObservationStamp> decode_observation_stamp(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "observation stamp must be an object");
  }
  LATOBS_TRY(observed, core::json_require_object(value, "observed_at"));
  LATOBS_TRY(received, core::json_require_object(value, "received_at"));
  ObservationStamp stamp;
  LATOBS_TRY(observed_at, decode_timestamp(*observed));
  LATOBS_TRY(received_at, decode_timestamp(*received));
  stamp.observed_at = observed_at;
  stamp.received_at = received_at;
  return stamp;
}

Result<HopObservation> decode_hop_observation(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "hop observation must be an object");
  }
  LATOBS_TRY(index_value, core::json_require_uint(value, "index"));
  if (index_value > 0xFFFFFFFFULL) {
    return Error(ErrorCode::OutOfRange, "hop index is outside the supported range");
  }
  LATOBS_TRY(index, HopIndex::from_value(static_cast<std::uint32_t>(index_value)));
  LATOBS_TRY(hop, json_id<core::HopTag>(value, "hop"));
  HopObservation observation;
  observation.index = index;
  observation.hop = hop;
  if (const std::optional<std::string> link = json_optional_id_hex(value, "link"); link.has_value()) {
    LATOBS_TRY(parsed, LinkId::parse_hex(*link));
    observation.link = parsed;
  }
  if (const std::optional<std::string> queue = json_optional_id_hex(value, "queue");
      queue.has_value()) {
    LATOBS_TRY(parsed, QueueId::parse_hex(*queue));
    observation.queue = parsed;
  }
  LATOBS_TRY(entry, core::json_require_object(value, "entry"));
  LATOBS_TRY(exit, core::json_require_object(value, "exit"));
  LATOBS_TRY(entry_value, decode_timestamp(*entry));
  LATOBS_TRY(exit_value, decode_timestamp(*exit));
  observation.entry = entry_value;
  observation.exit = exit_value;
  observation.entry_domain = entry_value.domain;
  observation.exit_domain = exit_value.domain;
  if (const core::JsonValue* queue_entry = value.find("queue_entry");
      queue_entry != nullptr && queue_entry->is_object()) {
    LATOBS_TRY(parsed, decode_timestamp(*queue_entry));
    observation.queue_entry = parsed;
  }
  if (const core::JsonValue* queue_exit = value.find("queue_exit");
      queue_exit != nullptr && queue_exit->is_object()) {
    LATOBS_TRY(parsed, decode_timestamp(*queue_exit));
    observation.queue_exit = parsed;
  }
  LATOBS_TRY(dwell, core::json_optional_int(value, "dwell_ns"));
  observation.dwell_ns = dwell;
  LATOBS_TRY(queue_dwell, core::json_optional_int(value, "queue_dwell_ns"));
  observation.queue_dwell_ns = queue_dwell;
  LATOBS_TRY(uncertainty, core::json_optional_int(value, "dwell_uncertainty_ns"));
  observation.dwell_uncertainty_ns = uncertainty;
  LATOBS_TRY(queue_uncertainty, core::json_optional_int(value, "queue_dwell_uncertainty_ns"));
  observation.queue_dwell_uncertainty_ns = queue_uncertainty;
  if (const core::JsonValue* evidence = value.find("evidence");
      evidence != nullptr && evidence->is_object()) {
    LATOBS_TRY(parsed, decode_evidence(*evidence));
    observation.evidence = parsed;
  }
  return observation;
}

Result<MeasurementRecord> decode_measurement(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "measurement must be an object");
  }
  MeasurementRecord record;
  if (const std::optional<std::string> id = json_optional_id_hex(value, "id"); id.has_value()) {
    LATOBS_TRY(parsed, MeasurementId::parse_hex(*id));
    record.id = parsed;
  }
  LATOBS_TRY(path, json_id<core::PathTag>(value, "path"));
  record.path = path;
  LATOBS_TRY(generation, json_id<core::GenerationTag>(value, "generation"));
  record.generation = generation;
  LATOBS_TRY(source, json_id<core::SourceTag>(value, "source"));
  record.source = source;
  LATOBS_TRY(epoch, json_id<core::EpochTag>(value, "epoch"));
  record.epoch = epoch;
  LATOBS_TRY(incarnation, json_id<core::IncarnationTag>(value, "incarnation"));
  record.incarnation = incarnation;
  LATOBS_TRY(revision, json_revision(value, "source_revision"));
  record.source_revision = revision;
  LATOBS_TRY(sequence, core::json_require_uint(value, "sequence"));
  record.sequence = Sequence::from_value(sequence);
  if (!record.id.valid()) {
    record.id = derive_measurement_id(record.source, record.epoch, record.incarnation,
                                      record.sequence);
  }
  LATOBS_TRY(synthetic, core::json_require_bool(value, "synthetic"));
  record.synthetic = synthetic;
  LATOBS_TRY(request, core::json_require_object(value, "request"));
  LATOBS_TRY(response, core::json_require_object(value, "response"));
  LATOBS_TRY(request_value, decode_timestamp(*request));
  LATOBS_TRY(response_value, decode_timestamp(*response));
  record.request = request_value;
  record.response = response_value;
  if (!request_value.domain.valid() || request_value.domain != response_value.domain) {
    return Error(ErrorCode::ParseError,
                 "request and response must share one clock domain");
  }
  record.domain = request_value.domain;
  LATOBS_TRY(rtt, core::json_require_int(value, "rtt_ns"));
  record.rtt_ns = rtt;
  LATOBS_TRY(stamp, core::json_require_object(value, "stamp"));
  LATOBS_TRY(stamp_value, decode_observation_stamp(*stamp));
  record.stamp = stamp_value;
  if (const core::JsonValue* evidence = value.find("evidence");
      evidence != nullptr && evidence->is_object()) {
    LATOBS_TRY(parsed, decode_evidence(*evidence));
    record.evidence = parsed;
  }
  if (const core::JsonValue* hops = value.find("hops"); hops != nullptr) {
    LATOBS_TRY(list, hops->as_array());
    record.hops.reserve(list->size());
    for (const core::JsonValue& entry : *list) {
      LATOBS_TRY(hop, decode_hop_observation(entry));
      record.hops.push_back(std::move(hop));
    }
  }
  return record;
}

Result<SourceDescriptor> decode_source(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "source must be an object");
  }
  SourceDescriptor source;
  LATOBS_TRY(name, json_name(value, "name"));
  source.name = name;
  LATOBS_TRY(kind_text, core::json_require_string(value, "source_kind"));
  if (!parse_source_kind(kind_text, source.kind)) {
    return Error(ErrorCode::ParseError, "unknown source kind", std::string(kind_text));
  }
  LATOBS_TRY(authority_text, core::json_require_string(value, "authority"));
  if (!parse_authority_class(authority_text, source.authority)) {
    return Error(ErrorCode::ParseError, "unknown authority class", std::string(authority_text));
  }
  LATOBS_TRY(semantics_text, core::json_require_string(value, "semantics"));
  if (!parse_semantics_list(semantics_text, source.semantics)) {
    return Error(ErrorCode::ParseError, "unknown semantics profile",
                 std::string(semantics_text));
  }
  LATOBS_TRY(revision, json_revision(value, "revision"));
  source.revision = revision;
  if (const core::JsonValue* description = value.find("description");
      description != nullptr && description->is_string()) {
    LATOBS_TRY(text, description->as_string());
    source.description = std::string(text);
  }
  source.id = SourceId::derive_from(source.name.view());
  return source;
}

Result<EndpointDef> decode_endpoint(const core::JsonValue& value) {
  EndpointDef endpoint;
  LATOBS_TRY(name, json_name(value, "name"));
  endpoint.name = name;
  LATOBS_TRY(role_text, core::json_require_string(value, "role"));
  if (!parse_endpoint_role(role_text, endpoint.role)) {
    return Error(ErrorCode::ParseError, "unknown endpoint role", std::string(role_text));
  }
  LATOBS_TRY(revision, json_revision(value, "revision"));
  endpoint.revision = revision;
  endpoint.id = EndpointId::derive_from(endpoint.name.view());
  return endpoint;
}

Result<LinkDef> decode_link(const core::JsonValue& value) {
  LinkDef link;
  LATOBS_TRY(name, json_name(value, "name"));
  link.name = name;
  if (const std::optional<std::string> from = json_optional_id_hex(value, "from");
      from.has_value()) {
    LATOBS_TRY(parsed, EndpointId::parse_hex(*from));
    link.from = parsed;
  }
  if (const std::optional<std::string> to = json_optional_id_hex(value, "to"); to.has_value()) {
    LATOBS_TRY(parsed, EndpointId::parse_hex(*to));
    link.to = parsed;
  }
  LATOBS_TRY(revision, json_revision(value, "revision"));
  link.revision = revision;
  link.id = LinkId::derive_from(link.name.view());
  return link;
}

Result<QueueDef> decode_queue(const core::JsonValue& value) {
  QueueDef queue;
  LATOBS_TRY(name, json_name(value, "name"));
  queue.name = name;
  if (const std::optional<std::string> link = json_optional_id_hex(value, "link");
      link.has_value()) {
    LATOBS_TRY(parsed, LinkId::parse_hex(*link));
    queue.link = parsed;
  }
  LATOBS_TRY(semantics_text, core::json_require_string(value, "semantics"));
  if (!parse_semantics_list(semantics_text, queue.semantics)) {
    return Error(ErrorCode::ParseError, "unknown semantics profile",
                 std::string(semantics_text));
  }
  LATOBS_TRY(revision, json_revision(value, "revision"));
  queue.revision = revision;
  queue.id = QueueId::derive_from(queue.name.view());
  return queue;
}

Result<HopDef> decode_hop(const core::JsonValue& value) {
  HopDef hop;
  LATOBS_TRY(name, json_name(value, "name"));
  hop.name = name;
  LATOBS_TRY(kind_text, core::json_require_string(value, "hop_kind"));
  if (!parse_hop_kind(kind_text, hop.kind)) {
    return Error(ErrorCode::ParseError, "unknown hop kind", std::string(kind_text));
  }
  if (const std::optional<std::string> link = json_optional_id_hex(value, "link");
      link.has_value()) {
    LATOBS_TRY(parsed, LinkId::parse_hex(*link));
    hop.link = parsed;
  }
  if (const std::optional<std::string> queue = json_optional_id_hex(value, "queue");
      queue.has_value()) {
    LATOBS_TRY(parsed, QueueId::parse_hex(*queue));
    hop.queue = parsed;
  }
  LATOBS_TRY(revision, json_revision(value, "revision"));
  hop.revision = revision;
  hop.id = HopId::derive_from(hop.name.view());
  return hop;
}

Result<GenerationDef> decode_generation(const core::JsonValue& value) {
  GenerationDef generation;
  LATOBS_TRY(name, json_name(value, "name"));
  generation.name = name;
  LATOBS_TRY(revision, json_revision(value, "revision"));
  generation.revision = revision;
  if (const std::optional<std::string> supersedes = json_optional_id_hex(value, "supersedes");
      supersedes.has_value()) {
    LATOBS_TRY(parsed, GenerationId::parse_hex(*supersedes));
    generation.supersedes = parsed;
  }
  if (const core::JsonValue* description = value.find("description");
      description != nullptr && description->is_string()) {
    LATOBS_TRY(text, description->as_string());
    generation.description = std::string(text);
  }
  generation.id = GenerationId::derive_from(generation.name.view());
  return generation;
}

Result<PathDef> decode_path(const core::JsonValue& value) {
  PathDef path;
  LATOBS_TRY(name, json_name(value, "name"));
  path.name = name;
  LATOBS_TRY(generation, json_id<core::GenerationTag>(value, "generation"));
  path.generation = generation;
  LATOBS_TRY(revision, json_revision(value, "revision"));
  path.revision = revision;
  LATOBS_TRY(hops_value, core::json_require_array(value, "hops"));
  LATOBS_TRY(hops, hops_value->as_array());
  for (const core::JsonValue& entry : *hops) {
    LATOBS_TRY(text, entry.as_string());
    LATOBS_TRY(hop_id, HopId::parse_hex(text));
    path.hops.push_back(hop_id);
  }
  path.id = PathId::derive_from(path.name.view());
  return path;
}

Result<ClockDomainDef> decode_clock_domain(const core::JsonValue& value) {
  ClockDomainDef domain;
  LATOBS_TRY(name, json_name(value, "name"));
  domain.name = name;
  LATOBS_TRY(is_reference, core::json_require_bool(value, "is_reference"));
  domain.is_reference = is_reference;
  if (const core::JsonValue* description = value.find("description");
      description != nullptr && description->is_string()) {
    LATOBS_TRY(text, description->as_string());
    domain.description = std::string(text);
  }
  domain.id = ClockDomainId::derive_from(domain.name.view());
  return domain;
}

Result<ClockSync> decode_clock_sync(const core::JsonValue& value) {
  ClockSync sync;
  LATOBS_TRY(domain, json_id<core::ClockDomainTag>(value, "domain"));
  sync.domain = domain;
  if (const std::optional<std::string> reference = json_optional_id_hex(value, "reference");
      reference.has_value()) {
    LATOBS_TRY(parsed, ClockDomainId::parse_hex(*reference));
    sync.reference = parsed;
  }
  LATOBS_TRY(state_text, core::json_require_string(value, "state"));
  if (!parse_clock_sync_state(state_text, sync.state)) {
    return Error(ErrorCode::ParseError, "unknown clock sync state", std::string(state_text));
  }
  LATOBS_TRY(offset, core::json_require_int(value, "offset_ns"));
  sync.offset_ns = offset;
  LATOBS_TRY(skew, core::json_require_int(value, "skew_ppb"));
  sync.skew_ppb = skew;
  LATOBS_TRY(uncertainty, core::json_require_int(value, "uncertainty_ns"));
  sync.uncertainty_ns = uncertainty;
  LATOBS_TRY(valid_for, core::json_require_int(value, "valid_for_ns"));
  sync.valid_for_ns = valid_for;
  LATOBS_TRY(observed_at, core::json_require_int(value, "observed_at_ns"));
  LATOBS_TRY(observed_domain, core::json_require_string(value, "observed_at_domain"));
  sync.observed_at.ns = observed_at;
  if (!observed_domain.empty()) {
    LATOBS_TRY(parsed, ClockDomainId::parse_hex(observed_domain));
    sync.observed_at.domain = parsed;
  }
  if (const std::optional<std::string> reported_by = json_optional_id_hex(value, "reported_by");
      reported_by.has_value()) {
    LATOBS_TRY(parsed, SourceId::parse_hex(*reported_by));
    sync.reported_by = parsed;
  }
  if (const std::optional<std::string> generation = json_optional_id_hex(value, "generation");
      generation.has_value()) {
    LATOBS_TRY(parsed, GenerationId::parse_hex(*generation));
    sync.generation = parsed;
  }
  if (const std::optional<std::string> epoch = json_optional_id_hex(value, "epoch");
      epoch.has_value()) {
    LATOBS_TRY(parsed, EpochId::parse_hex(*epoch));
    sync.epoch = parsed;
  }
  if (const std::optional<std::string> incarnation = json_optional_id_hex(value, "incarnation");
      incarnation.has_value()) {
    LATOBS_TRY(parsed, IncarnationId::parse_hex(*incarnation));
    sync.incarnation = parsed;
  }
  if (const core::JsonValue* revision = value.find("revision");
      revision != nullptr && !revision->is_null()) {
    LATOBS_TRY(parsed, json_revision(value, "revision"));
    sync.revision = parsed;
  }
  return sync;
}

Result<MeasurementRecord> decode_measurement_text(std::string_view text,
                                                  const core::Limits& limits) {
  if (text.size() > limits.max_json_bytes) {
    return Error(ErrorCode::OutOfRange, "measurement document exceeds the configured size limit",
                 std::to_string(text.size()));
  }
  LATOBS_TRY(document, core::parse_json(text, limits.max_json_depth));
  return decode_measurement(document);
}

Result<SourceDescriptor> decode_source_text(std::string_view text, const core::Limits& limits) {
  if (text.size() > limits.max_json_bytes) {
    return Error(ErrorCode::OutOfRange, "source document exceeds the configured size limit",
                 std::to_string(text.size()));
  }
  LATOBS_TRY(document, core::parse_json(text, limits.max_json_depth));
  return decode_source(document);
}

}  // namespace latobs::model
