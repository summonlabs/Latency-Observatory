// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/runtime/service.hpp"

#include <algorithm>
#include <map>
#include <utility>

#include "latency_observatory/core/checked.hpp"
#include "latency_observatory/core/digest.hpp"
#include "latency_observatory/model/codec.hpp"

namespace latobs::runtime {
namespace {

[[nodiscard]] bool looks_like_identity(std::string_view text) noexcept {
  if (text.size() != 16) return false;
  for (const char c : text) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

template <class Tag>
[[nodiscard]] Result<core::StrongId<Tag>> parse_identity(std::string_view text) {
  return core::StrongId<Tag>::parse_hex(text);
}

[[nodiscard]] Result<PathId> resolve_path(const Engine& engine, const core::JsonValue& request,
                                          std::string_view field) {
  LATOBS_TRY(text, core::json_require_string(request, field));
  if (looks_like_identity(text)) return parse_identity<core::PathTag>(text);
  LATOBS_TRY(path, engine.catalog().path_by_name(text));
  return path->id;
}

[[nodiscard]] Result<GenerationId> resolve_generation(const Engine& engine,
                                                      const core::JsonValue& request,
                                                      std::string_view field) {
  LATOBS_TRY(text, core::json_require_string(request, field));
  if (looks_like_identity(text)) return parse_identity<core::GenerationTag>(text);
  LATOBS_TRY(generation, engine.catalog().generation_by_name(text));
  return generation->id;
}

[[nodiscard]] Result<SourceId> resolve_source(const Engine& engine, const core::JsonValue& request,
                                              std::string_view field) {
  LATOBS_TRY(text, core::json_require_string(request, field));
  if (looks_like_identity(text)) return parse_identity<core::SourceTag>(text);
  LATOBS_TRY(source, engine.catalog().source_by_name(text));
  return source->id;
}

[[nodiscard]] Result<HopId> resolve_hop(const Engine& engine, std::string_view text) {
  if (looks_like_identity(text)) return parse_identity<core::HopTag>(text);
  const std::string name(text);
  const HopId derived = HopId::derive_from(name);
  LATOBS_TRY(definition, engine.catalog().hop(derived));
  return definition->id;
}

[[nodiscard]] Result<QueueId> resolve_queue(const Engine& engine, std::string_view text) {
  if (looks_like_identity(text)) return parse_identity<core::QueueTag>(text);
  const std::string name(text);
  const QueueId derived = QueueId::derive_from(name);
  LATOBS_TRY(definition, engine.catalog().queue(derived));
  return definition->id;
}

[[nodiscard]] Result<LinkId> resolve_link(const Engine& engine, std::string_view text) {
  if (looks_like_identity(text)) return parse_identity<core::LinkTag>(text);
  const std::string name(text);
  const LinkId derived = LinkId::derive_from(name);
  LATOBS_TRY(definition, engine.catalog().link(derived));
  return definition->id;
}

[[nodiscard]] Result<EndpointId> resolve_endpoint(const Engine& engine, std::string_view text) {
  if (looks_like_identity(text)) return parse_identity<core::EndpointTag>(text);
  const std::string name(text);
  const EndpointId derived = EndpointId::derive_from(name);
  LATOBS_TRY(definition, engine.catalog().endpoint(derived));
  return definition->id;
}

/// Epochs and incarnations are declared by sources as names; their identities
/// are content addressed so the same declaration always yields the same id.
[[nodiscard]] EpochId resolve_epoch(std::string_view text) {
  if (looks_like_identity(text)) {
    const Result<EpochId> parsed = EpochId::parse_hex(text);
    if (parsed.has_value()) return parsed.value();
  }
  return EpochId::derive_from(text);
}

[[nodiscard]] IncarnationId resolve_incarnation(std::string_view text) {
  if (looks_like_identity(text)) {
    const Result<IncarnationId> parsed = IncarnationId::parse_hex(text);
    if (parsed.has_value()) return parsed.value();
  }
  return IncarnationId::derive_from(text);
}

[[nodiscard]] Result<ClockDomainId> resolve_clock_domain(const Engine& engine,
                                                         std::string_view text) {
  if (looks_like_identity(text)) return parse_identity<core::ClockDomainTag>(text);
  for (const model::ClockDomainDef* domain : engine.clocks().domains()) {
    if (domain->name.view() == text) return domain->id;
  }
  return Error(ErrorCode::NotFound, "unknown clock domain", std::string(text));
}

[[nodiscard]] Result<Timestamp> resolve_timestamp(const Engine& /*engine*/,
                                                  const core::JsonValue& request,
                                                  std::string_view field,
                                                  const Timestamp& fallback) {
  LATOBS_TRY(value, core::json_optional_int(request, field));
  if (!value.has_value()) return fallback;
  return Timestamp{*value, fallback.domain};
}

[[nodiscard]] Result<stats::TimeWindow> resolve_window(const Engine& engine,
                                                       const core::JsonValue& request,
                                                       const Timestamp& now) {
  const Timestamp from = Timestamp{0, now.domain};
  const Timestamp to = Timestamp{now.ns + 1, now.domain};
  LATOBS_TRY(from_value, resolve_timestamp(engine, request, "from_ns", from));
  LATOBS_TRY(to_value, resolve_timestamp(engine, request, "to_ns", to));
  return stats::TimeWindow::make(from_value, to_value);
}

[[nodiscard]] Result<stats::AggregationMode> resolve_mode(const core::JsonValue& request,
                                                          stats::AggregationMode fallback) {
  const core::JsonValue* mode = request.find("mode");
  if (mode == nullptr || mode->is_null()) return fallback;
  LATOBS_TRY(text, mode->as_string());
  if (text == "current") return stats::AggregationMode::Current;
  if (text == "historical") return stats::AggregationMode::Historical;
  return Error(ErrorCode::InvalidArgument, "unknown aggregation mode", std::string(text));
}

struct ResponseBuilder {
  std::string text;
  core::JsonWriter writer{text};
  std::string op;
  bool pretty = false;
};

}  // namespace

const std::vector<std::string>& supported_operations() {
  static const std::vector<std::string> operations = {
      "capabilities", "status",        "operations",  "define_source", "define_generation",
      "define_endpoint", "define_link", "define_queue", "define_hop",   "define_path",
      "define_clock_domain", "clock_sync", "ingest",   "summarize",    "history",
      "baseline_create", "baseline_list", "attribute", "explain",      "export",
      "flush", "shutdown",
  };
  return operations;
}

std::string describe_operations() {
  std::string text;
  core::JsonWriter writer(text, true);
  writer.begin_object();
  writer.field_array("operations");
  for (const std::string& operation : supported_operations()) writer.value_string(operation);
  writer.end_array();
  writer.end_object();
  return text;
}

Result<std::unique_ptr<Service>> Service::create(RuntimeConfig config) {
  LATOBS_TRY_STATUS(config.policy.validate());
  core::Limits limits = config.policy.limits;
  LATOBS_TRY(engine, Engine::create(std::move(config)));
  return std::unique_ptr<Service>(new Service(std::move(engine), limits));
}

std::string Service::execute(std::string_view request_text) {
  if (request_text.size() > max_request_bytes_) {
    std::string text;
    core::JsonWriter writer(text);
    writer.begin_object();
    writer.field("ok", false);
    writer.field("op", "unknown");
    writer.field_object("error");
    writer.field("code", core::to_string(ErrorCode::OutOfRange));
    writer.field("message", "the request exceeds the configured maximum size");
    writer.field("context", std::to_string(request_text.size()));
    writer.end_object();
    writer.end_object();
    return text;
  }
  const Result<core::JsonValue> parsed = core::parse_json(request_text, limits_.max_json_depth);
  if (!parsed.has_value()) {
    std::string text;
    core::JsonWriter writer(text);
    writer.begin_object();
    writer.field("ok", false);
    writer.field("op", "unknown");
    writer.field_object("error");
    writer.field("code", core::to_string(parsed.error().code()));
    writer.field("message", parsed.error().message());
    writer.field("context", parsed.error().context());
    writer.end_object();
    writer.end_object();
    return text;
  }
  return execute(parsed.value());
}

std::string Service::execute(const core::JsonValue& request) {
  std::string op_name = "unknown";
  const Result<std::string> outcome = dispatch(request, op_name);
  std::string text;
  core::JsonWriter writer(text);
  writer.begin_object();
  if (outcome.has_value()) {
    writer.field("ok", true);
    writer.field("op", op_name);
    writer.field("policy_digest", engine_->policy().digest());
    // The dispatched text is already a canonical JSON document; emitting it
    // verbatim under the "result" key keeps the response byte stable.
    writer.key("result");
    writer.raw_value(outcome.value());
  } else {
    writer.field("ok", false);
    writer.field("op", op_name);
    writer.field_object("error");
    writer.field("code", core::to_string(outcome.error().code()));
    writer.field("message", outcome.error().message());
    writer.field("context", outcome.error().context());
    writer.end_object();
  }
  writer.end_object();
  LATOBS_ASSERT_MSG(writer.balanced(), "the service response must be balanced JSON");
  return text;
}

Result<std::string> Service::dispatch(const core::JsonValue& request, std::string& op_name) {
  if (!request.is_object()) {
    return Error(ErrorCode::ParseError, "a request must be a JSON object");
  }
  LATOBS_TRY(op_text, core::json_require_string(request, "op"));
  op_name = std::string(op_text);
  const Timestamp now = core::Clock::now_reference();
  std::string text;
  core::JsonWriter writer(text);

  if (op_name == "capabilities") {
    write_json(writer, engine_->capabilities());
    return text;
  }
  if (op_name == "operations") {
    writer.begin_object();
    writer.field_array("operations");
    for (const std::string& operation : supported_operations()) writer.value_string(operation);
    writer.end_array();
    writer.end_object();
    return text;
  }
  if (op_name == "status") {
    writer.begin_object();
    writer.key("stats");
    write_json(writer, engine_->stats());
    writer.field_object("recovery");
    const store::RecoveryReport report = engine_->recovery();
    writer.field("segments_scanned", report.segments_scanned);
    writer.field("segments_accepted", report.segments_accepted);
    writer.field("segments_rejected", report.segments_rejected);
    writer.field("records_read", report.records_read);
    writer.field("records_discarded", report.records_discarded);
    writer.field("truncated", report.truncated);
    writer.field("detail", report.detail);
    writer.end_object();
    writer.field("policy_digest", engine_->policy().digest());
    writer.field("shut_down", engine_->shut_down());
    writer.end_object();
    return text;
  }
  if (op_name == "define_source") {
    model::SourceDescriptor descriptor;
    LATOBS_TRY(name, model::json_name(request, "name"));
    descriptor.name = name;
    LATOBS_TRY(kind_text, core::json_require_string(request, "source_kind"));
    if (!model::parse_source_kind(kind_text, descriptor.kind)) {
      return Error(ErrorCode::InvalidArgument, "unknown source kind", std::string(kind_text));
    }
    LATOBS_TRY(authority_text, core::json_require_string(request, "authority"));
    if (!model::parse_authority_class(authority_text, descriptor.authority)) {
      return Error(ErrorCode::InvalidArgument, "unknown authority class",
                   std::string(authority_text));
    }
    LATOBS_TRY(semantics_text, core::json_require_string(request, "semantics"));
    if (!model::parse_semantics_list(semantics_text, descriptor.semantics)) {
      return Error(ErrorCode::InvalidArgument, "unknown semantics profile",
                   std::string(semantics_text));
    }
    LATOBS_TRY(revision, model::json_revision(request, "revision"));
    descriptor.revision = revision;
    if (const core::JsonValue* description = request.find("description");
        description != nullptr && description->is_string()) {
      LATOBS_TRY(value, description->as_string());
      descriptor.description = std::string(value);
    }
    LATOBS_TRY(id, engine_->define_source(std::move(descriptor)));
    writer.begin_object();
    writer.field("source", id.to_hex());
    writer.field("name", name.str());
    writer.end_object();
    return text;
  }
  if (op_name == "define_generation") {
    model::GenerationDef definition;
    LATOBS_TRY(name, model::json_name(request, "name"));
    definition.name = name;
    LATOBS_TRY(revision, model::json_revision(request, "revision"));
    definition.revision = revision;
    if (const std::optional<std::string> supersedes = model::json_optional_id_hex(request, "supersedes");
        supersedes.has_value()) {
      LATOBS_TRY(parsed, GenerationId::parse_hex(*supersedes));
      definition.supersedes = parsed;
    } else if (const core::JsonValue* supersedes_name = request.find("supersedes_name");
               supersedes_name != nullptr && supersedes_name->is_string()) {
      LATOBS_TRY(value, supersedes_name->as_string());
      LATOBS_TRY(previous, engine_->catalog().generation_by_name(value));
      definition.supersedes = previous->id;
    }
    if (const core::JsonValue* description = request.find("description");
        description != nullptr && description->is_string()) {
      LATOBS_TRY(value, description->as_string());
      definition.description = std::string(value);
    }
    LATOBS_TRY(id, engine_->define_generation(std::move(definition)));
    writer.begin_object();
    writer.field("generation", id.to_hex());
    writer.field("name", name.str());
    writer.end_object();
    return text;
  }
  if (op_name == "define_endpoint" || op_name == "define_link" || op_name == "define_queue" ||
      op_name == "define_hop") {
    LATOBS_TRY(name, model::json_name(request, "name"));
    if (op_name == "define_endpoint") {
      model::EndpointDef definition;
      definition.name = name;
      LATOBS_TRY(role_text, core::json_require_string(request, "role"));
      if (!model::parse_endpoint_role(role_text, definition.role)) {
        return Error(ErrorCode::InvalidArgument, "unknown endpoint role", std::string(role_text));
      }
      LATOBS_TRY(revision, model::json_revision(request, "revision"));
      definition.revision = revision;
      LATOBS_TRY(id, engine_->define_endpoint(std::move(definition)));
      writer.begin_object();
      writer.field("endpoint", id.to_hex());
      writer.end_object();
      return text;
    }
    if (op_name == "define_link") {
      model::LinkDef definition;
      definition.name = name;
      LATOBS_TRY(from_text, core::json_require_string(request, "from"));
      LATOBS_TRY(from, resolve_endpoint(*engine_, from_text));
      LATOBS_TRY(to_text, core::json_require_string(request, "to"));
      LATOBS_TRY(to, resolve_endpoint(*engine_, to_text));
      definition.from = from;
      definition.to = to;
      LATOBS_TRY(revision, model::json_revision(request, "revision"));
      definition.revision = revision;
      LATOBS_TRY(id, engine_->define_link(std::move(definition)));
      writer.begin_object();
      writer.field("link", id.to_hex());
      writer.end_object();
      return text;
    }
    if (op_name == "define_queue") {
      model::QueueDef definition;
      definition.name = name;
      LATOBS_TRY(link_text, core::json_require_string(request, "link"));
      LATOBS_TRY(link, resolve_link(*engine_, link_text));
      definition.link = link;
      LATOBS_TRY(semantics_text, core::json_require_string(request, "semantics"));
      if (!model::parse_semantics_list(semantics_text, definition.semantics)) {
        return Error(ErrorCode::InvalidArgument, "unknown semantics profile",
                     std::string(semantics_text));
      }
      LATOBS_TRY(revision, model::json_revision(request, "revision"));
      definition.revision = revision;
      LATOBS_TRY(id, engine_->define_queue(std::move(definition)));
      writer.begin_object();
      writer.field("queue", id.to_hex());
      writer.end_object();
      return text;
    }
    model::HopDef definition;
    definition.name = name;
    LATOBS_TRY(kind_text, core::json_require_string(request, "hop_kind"));
    if (!model::parse_hop_kind(kind_text, definition.kind)) {
      return Error(ErrorCode::InvalidArgument, "unknown hop kind", std::string(kind_text));
    }
    if (const core::JsonValue* link = request.find("link");
        link != nullptr && link->is_string()) {
      LATOBS_TRY(value, link->as_string());
      LATOBS_TRY(resolved, resolve_link(*engine_, value));
      definition.link = resolved;
    }
    if (const core::JsonValue* queue = request.find("queue");
        queue != nullptr && queue->is_string()) {
      LATOBS_TRY(value, queue->as_string());
      LATOBS_TRY(resolved, resolve_queue(*engine_, value));
      definition.queue = resolved;
    }
    LATOBS_TRY(revision, model::json_revision(request, "revision"));
    definition.revision = revision;
    LATOBS_TRY(id, engine_->define_hop(std::move(definition)));
    writer.begin_object();
    writer.field("hop", id.to_hex());
    writer.end_object();
    return text;
  }
  if (op_name == "define_path") {
    model::PathDef definition;
    LATOBS_TRY(name, model::json_name(request, "name"));
    definition.name = name;
    LATOBS_TRY(generation_text, core::json_require_string(request, "generation"));
    LATOBS_TRY(generation, resolve_generation(*engine_, request, "generation"));
    (void)generation_text;
    definition.generation = generation;
    LATOBS_TRY(revision, model::json_revision(request, "revision"));
    definition.revision = revision;
    LATOBS_TRY(hops, core::json_require_array(request, "hops"));
    LATOBS_TRY(hop_array, hops->as_array());
    for (const core::JsonValue& entry : *hop_array) {
      LATOBS_TRY(hop_text, entry.as_string());
      LATOBS_TRY(hop, resolve_hop(*engine_, hop_text));
      definition.hops.push_back(hop);
    }
    LATOBS_TRY(id, engine_->define_path(std::move(definition)));
    writer.begin_object();
    writer.field("path", id.to_hex());
    writer.end_object();
    return text;
  }
  if (op_name == "define_clock_domain") {
    model::ClockDomainDef definition;
    LATOBS_TRY(name, model::json_name(request, "name"));
    definition.name = name;
    LATOBS_TRY(is_reference, core::json_require_bool(request, "is_reference"));
    definition.is_reference = is_reference;
    if (const core::JsonValue* description = request.find("description");
        description != nullptr && description->is_string()) {
      LATOBS_TRY(value, description->as_string());
      definition.description = std::string(value);
    }
    LATOBS_TRY(id, engine_->define_clock_domain(std::move(definition)));
    writer.begin_object();
    writer.field("clock_domain", id.to_hex());
    writer.end_object();
    return text;
  }
  if (op_name == "clock_sync") {
    model::ClockSync sync;
    LATOBS_TRY(domain_text, core::json_require_string(request, "domain"));
    LATOBS_TRY(domain, resolve_clock_domain(*engine_, domain_text));
    sync.domain = domain;
    LATOBS_TRY(reference_text, core::json_require_string(request, "reference"));
    LATOBS_TRY(reference, resolve_clock_domain(*engine_, reference_text));
    sync.reference = reference;
    LATOBS_TRY(state_text, core::json_require_string(request, "state"));
    if (!model::parse_clock_sync_state(state_text, sync.state)) {
      return Error(ErrorCode::InvalidArgument, "unknown clock sync state", std::string(state_text));
    }
    LATOBS_TRY(offset, core::json_require_int(request, "offset_ns"));
    sync.offset_ns = offset;
    if (const core::JsonValue* skew = request.find("skew_ppb");
        skew != nullptr && !skew->is_null()) {
      LATOBS_TRY(value, skew->as_int64());
      sync.skew_ppb = value;
    }
    LATOBS_TRY(uncertainty, core::json_require_int(request, "uncertainty_ns"));
    sync.uncertainty_ns = uncertainty;
    LATOBS_TRY(valid_for, core::json_require_int(request, "valid_for_ns"));
    sync.valid_for_ns = valid_for;
    LATOBS_TRY(observed_at, core::json_require_int(request, "observed_at_ns"));
    LATOBS_TRY(observed_domain_text, core::json_require_string(request, "observed_at_domain"));
    LATOBS_TRY(observed_domain, resolve_clock_domain(*engine_, observed_domain_text));
    sync.observed_at = Timestamp{observed_at, observed_domain};
    LATOBS_TRY(revision, model::json_revision(request, "revision"));
    sync.revision = revision;
    if (const core::JsonValue* reported_by = request.find("reported_by");
        reported_by != nullptr && reported_by->is_string()) {
      LATOBS_TRY(value, reported_by->as_string());
      LATOBS_TRY(source, resolve_source(*engine_, request, "reported_by"));
      (void)value;
      sync.reported_by = source;
    }
    if (const core::JsonValue* generation = request.find("generation");
        generation != nullptr && generation->is_string()) {
      LATOBS_TRY(value, generation->as_string());
      LATOBS_TRY(resolved, resolve_generation(*engine_, request, "generation"));
      (void)value;
      sync.generation = resolved;
    }
    if (const core::JsonValue* epoch = request.find("epoch");
        epoch != nullptr && epoch->is_string()) {
      LATOBS_TRY(value, epoch->as_string());
      sync.epoch = resolve_epoch(value);
    }
    if (const core::JsonValue* incarnation = request.find("incarnation");
        incarnation != nullptr && incarnation->is_string()) {
      LATOBS_TRY(value, incarnation->as_string());
      sync.incarnation = resolve_incarnation(value);
    }
    LATOBS_TRY_STATUS(engine_->record_clock_sync(sync));
    writer.begin_object();
    writer.field("clock_domain", sync.domain.to_hex());
    writer.field("state", model::to_string(sync.state));
    writer.end_object();
    return text;
  }
  if (op_name == "ingest") {
    ingest::IngestRequest ingest_request;
    ingest_request.received_at = now;
    LATOBS_TRY(received, core::json_optional_int(request, "received_at_ns"));
    if (received.has_value()) {
      ingest_request.received_at = Timestamp{*received, now.domain};
    }
    LATOBS_TRY(records, core::json_require_array(request, "records"));
    LATOBS_TRY(record_array, records->as_array());
    if (record_array->size() > limits_.max_batch_samples) {
      return Error(ErrorCode::CapacityExceeded, "the ingest batch exceeds the configured limit",
                   std::to_string(record_array->size()));
    }
    for (const core::JsonValue& entry : *record_array) {
      model::MeasurementRecord record;
      LATOBS_TRY(path_text, core::json_require_string(entry, "path"));
      LATOBS_TRY(path, resolve_path(*engine_, entry, "path"));
      (void)path_text;
      record.path = path;
      LATOBS_TRY(generation, resolve_generation(*engine_, entry, "generation"));
      record.generation = generation;
      LATOBS_TRY(source, resolve_source(*engine_, entry, "source"));
      record.source = source;
      LATOBS_TRY(epoch_text, core::json_require_string(entry, "epoch"));
      record.epoch = resolve_epoch(epoch_text);
      LATOBS_TRY(incarnation_text, core::json_require_string(entry, "incarnation"));
      record.incarnation = resolve_incarnation(incarnation_text);
      LATOBS_TRY(revision, model::json_revision(entry, "source_revision"));
      record.source_revision = revision;
      LATOBS_TRY(sequence, core::json_require_uint(entry, "sequence"));
      record.sequence = Sequence::from_value(sequence);
      LATOBS_TRY(domain_text, core::json_require_string(entry, "domain"));
      LATOBS_TRY(domain, resolve_clock_domain(*engine_, domain_text));
      LATOBS_TRY(request_ns, core::json_require_int(entry, "request_ns"));
      LATOBS_TRY(response_ns, core::json_require_int(entry, "response_ns"));
      record.request = Timestamp{request_ns, domain};
      record.response = Timestamp{response_ns, domain};
      record.domain = domain;
      record.rtt_ns = response_ns - request_ns;
      if (const core::JsonValue* rtt = entry.find("rtt_ns"); rtt != nullptr && !rtt->is_null()) {
        LATOBS_TRY(declared, rtt->as_int64());
        record.rtt_ns = declared;
      }
      LATOBS_TRY(observed_ns, core::json_require_int(entry, "observed_at_ns"));
      LATOBS_TRY(observed_domain_text,
                 core::json_require_string(entry, "observed_at_domain"));
      LATOBS_TRY(observed_domain, resolve_clock_domain(*engine_, observed_domain_text));
      record.stamp.observed_at = Timestamp{observed_ns, observed_domain};
      record.stamp.received_at = ingest_request.received_at;
      LATOBS_TRY(synthetic, core::json_optional_int(entry, "synthetic"));
      record.synthetic = synthetic.has_value() && *synthetic != 0;

      if (const core::JsonValue* hops = entry.find("hops"); hops != nullptr) {
        LATOBS_TRY(hop_array, hops->as_array());
        for (const core::JsonValue& hop_entry : *hop_array) {
          model::HopObservation observation;
          LATOBS_TRY(index_value, core::json_require_uint(hop_entry, "index"));
          if (index_value > 0xFFFFFFFFULL) {
            return Error(ErrorCode::OutOfRange, "hop index is outside the supported range");
          }
          LATOBS_TRY(index, HopIndex::from_value(static_cast<std::uint32_t>(index_value)));
          observation.index = index;
          LATOBS_TRY(hop_text, core::json_require_string(hop_entry, "hop"));
          LATOBS_TRY(hop, resolve_hop(*engine_, hop_text));
          observation.hop = hop;
          if (const core::JsonValue* link = hop_entry.find("link");
              link != nullptr && link->is_string()) {
            LATOBS_TRY(value, link->as_string());
            LATOBS_TRY(resolved, resolve_link(*engine_, value));
            observation.link = resolved;
          }
          if (const core::JsonValue* queue = hop_entry.find("queue");
              queue != nullptr && queue->is_string()) {
            LATOBS_TRY(value, queue->as_string());
            LATOBS_TRY(resolved, resolve_queue(*engine_, value));
            observation.queue = resolved;
          }
          LATOBS_TRY(entry_ns, core::json_require_int(hop_entry, "entry_ns"));
          LATOBS_TRY(entry_domain_text, core::json_require_string(hop_entry, "entry_domain"));
          LATOBS_TRY(entry_domain, resolve_clock_domain(*engine_, entry_domain_text));
          LATOBS_TRY(exit_ns, core::json_require_int(hop_entry, "exit_ns"));
          LATOBS_TRY(exit_domain_text, core::json_require_string(hop_entry, "exit_domain"));
          LATOBS_TRY(exit_domain, resolve_clock_domain(*engine_, exit_domain_text));
          observation.entry = Timestamp{entry_ns, entry_domain};
          observation.exit = Timestamp{exit_ns, exit_domain};
          observation.entry_domain = entry_domain;
          observation.exit_domain = exit_domain;
          if (const core::JsonValue* queue_entry = hop_entry.find("queue_entry_ns");
              queue_entry != nullptr && !queue_entry->is_null()) {
            LATOBS_TRY(value, queue_entry->as_int64());
            observation.queue_entry = Timestamp{value, entry_domain};
          }
          if (const core::JsonValue* queue_exit = hop_entry.find("queue_exit_ns");
              queue_exit != nullptr && !queue_exit->is_null()) {
            LATOBS_TRY(value, queue_exit->as_int64());
            observation.queue_exit = Timestamp{value, exit_domain};
          }
          record.hops.push_back(std::move(observation));
        }
      }
      ingest_request.records.push_back(std::move(record));
    }
    LATOBS_TRY(report, engine_->ingest(std::move(ingest_request)));
    ingest::write_json(writer, report);
    return text;
  }
  if (op_name == "summarize" || op_name == "explain") {
    stats::SummaryRequest summary_request;
    LATOBS_TRY(path, resolve_path(*engine_, request, "path"));
    summary_request.path = path;
    LATOBS_TRY(generation, resolve_generation(*engine_, request, "generation"));
    summary_request.generation = generation;
    LATOBS_TRY(mode, resolve_mode(request, stats::AggregationMode::Current));
    summary_request.mode = mode;
    LATOBS_TRY(window, resolve_window(*engine_, request, now));
    summary_request.window = window;
    if (op_name == "summarize") {
      LATOBS_TRY(summary, engine_->summarize(summary_request));
      stats::write_json(writer, summary);
      return text;
    }
    LATOBS_TRY(explanation, engine_->explain(summary_request));
    const core::JsonValue* format = request.find("format");
    if (format != nullptr && format->is_string()) {
      LATOBS_TRY(value, format->as_string());
      if (value == "text") {
        writer.value_string(render_text(explanation));
        return text;
      }
      if (value != "json") {
        return Error(ErrorCode::InvalidArgument, "unknown explanation format", std::string(value));
      }
    }
    write_json(writer, explanation);
    return text;
  }
  if (op_name == "history") {
    HistoryRequest history_request;
    LATOBS_TRY(path, resolve_path(*engine_, request, "path"));
    history_request.path = path;
    LATOBS_TRY(generation, resolve_generation(*engine_, request, "generation"));
    history_request.generation = generation;
    LATOBS_TRY(mode, resolve_mode(request, stats::AggregationMode::Historical));
    history_request.mode = mode;
    LATOBS_TRY(window, resolve_window(*engine_, request, now));
    history_request.window = window;
    LATOBS_TRY(bucket, core::json_require_int(request, "bucket_ns"));
    history_request.bucket_ns = bucket;
    if (const core::JsonValue* source = request.find("source");
        source != nullptr && source->is_string()) {
      LATOBS_TRY(resolved, resolve_source(*engine_, request, "source"));
      history_request.restrict_to_source = resolved;
    }
    LATOBS_TRY(result, engine_->history(history_request));
    write_json(writer, result);
    return text;
  }
  if (op_name == "baseline_create") {
    BaselineRequest baseline_request;
    LATOBS_TRY(name, model::json_name(request, "name"));
    baseline_request.name = name;
    LATOBS_TRY(path, resolve_path(*engine_, request, "path"));
    baseline_request.path = path;
    LATOBS_TRY(generation, resolve_generation(*engine_, request, "generation"));
    baseline_request.generation = generation;
    LATOBS_TRY(mode, resolve_mode(request, stats::AggregationMode::Current));
    baseline_request.mode = mode;
    LATOBS_TRY(window, resolve_window(*engine_, request, now));
    baseline_request.window = window;
    if (const core::JsonValue* source = request.find("source");
        source != nullptr && source->is_string()) {
      LATOBS_TRY(resolved, resolve_source(*engine_, request, "source"));
      baseline_request.source = resolved;
    }
    LATOBS_TRY(id, engine_->create_baseline(baseline_request));
    writer.begin_object();
    writer.field("baseline", id.to_hex());
    writer.field("name", name.str());
    writer.end_object();
    return text;
  }
  if (op_name == "baseline_list") {
    LATOBS_TRY(baselines, engine_->list_baselines());
    writer.begin_object();
    writer.field_array("baselines");
    for (const baseline::Baseline* value : baselines) baseline::write_json(writer, *value);
    writer.end_array();
    writer.end_object();
    return text;
  }
  if (op_name == "attribute") {
    attribute::AttributionRequest attribution_request;
    LATOBS_TRY(path, resolve_path(*engine_, request, "path"));
    attribution_request.path = path;
    LATOBS_TRY(generation, resolve_generation(*engine_, request, "generation"));
    attribution_request.generation = generation;
    LATOBS_TRY(mode, resolve_mode(request, stats::AggregationMode::Current));
    attribution_request.mode = mode;
    LATOBS_TRY(window, resolve_window(*engine_, request, now));
    attribution_request.window = window;
    if (const core::JsonValue* baseline = request.find("baseline");
        baseline != nullptr && baseline->is_string()) {
      LATOBS_TRY(value, baseline->as_string());
      LATOBS_TRY(parsed, BaselineId::parse_hex(value));
      attribution_request.baseline = parsed;
    }
    LATOBS_TRY(result, engine_->attribute(attribution_request));
    attribute::write_json(writer, result);
    return text;
  }
  if (op_name == "export") {
    ExportRequest export_request;
    LATOBS_TRY(kind_text, core::json_require_string(request, "kind"));
    if (!parse_export_kind(kind_text, export_request.kind)) {
      return Error(ErrorCode::InvalidArgument, "unknown export kind", std::string(kind_text));
    }
    LATOBS_TRY(format_text, core::json_require_string(request, "format"));
    if (!parse_export_format(format_text, export_request.format)) {
      return Error(ErrorCode::InvalidArgument, "unknown export format", std::string(format_text));
    }
    if (const core::JsonValue* path = request.find("path");
        path != nullptr && path->is_string()) {
      LATOBS_TRY(resolved, resolve_path(*engine_, request, "path"));
      export_request.path = resolved;
    }
    if (const core::JsonValue* generation = request.find("generation");
        generation != nullptr && generation->is_string()) {
      LATOBS_TRY(resolved, resolve_generation(*engine_, request, "generation"));
      export_request.generation = resolved;
    }
    if (const core::JsonValue* from = request.find("from_ns");
        from != nullptr && !from->is_null()) {
      LATOBS_TRY(value, from->as_int64());
      export_request.from = Timestamp{value, now.domain};
    }
    if (const core::JsonValue* to = request.find("to_ns"); to != nullptr && !to->is_null()) {
      LATOBS_TRY(value, to->as_int64());
      export_request.to = Timestamp{value, now.domain};
    }
    if (const core::JsonValue* limit = request.find("limit");
        limit != nullptr && !limit->is_null()) {
      LATOBS_TRY(value, limit->as_uint64());
      export_request.limit = static_cast<std::size_t>(value);
    }
    if (const core::JsonValue* pretty = request.find("pretty");
        pretty != nullptr && !pretty->is_null()) {
      LATOBS_TRY(value, pretty->as_bool());
      export_request.pretty = value;
    }
    LATOBS_TRY(result, engine_->export_data(export_request));
    writer.begin_object();
    writer.field("records_exported", result.records_exported);
    writer.field("records_skipped", result.records_skipped);
    writer.field("truncated", result.truncated);
    writer.field("content_digest", result.content_digest);
    writer.field_array("notes");
    for (const std::string& note : result.notes) writer.value_string(note);
    writer.end_array();
    writer.field("content", result.text);
    writer.end_object();
    return text;
  }
  if (op_name == "flush") {
    LATOBS_TRY_STATUS(engine_->flush());
    writer.begin_object();
    writer.field("flushed", true);
    writer.end_object();
    return text;
  }
  if (op_name == "shutdown") {
    writer.begin_object();
    writer.field("shutting_down", true);
    writer.end_object();
    // The caller closes the runtime; the response is produced first so that a
    // client always sees the acknowledgement.
    return text;
  }
  return Error(ErrorCode::Unsupported, "unknown operation", op_name);
}

Status Service::shutdown() {
  if (engine_ == nullptr) return core::ok_status();
  return engine_->shutdown();
}

}  // namespace latobs::runtime
