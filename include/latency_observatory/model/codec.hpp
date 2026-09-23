// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/model/clock.hpp"
#include "latency_observatory/model/entities.hpp"
#include "latency_observatory/model/identities.hpp"
#include "latency_observatory/model/measurement.hpp"

namespace latobs::model {

// ---------------------------------------------------------------------------
// Canonical JSON codecs. Every domain object has exactly one canonical text
// form: encoding the same value always produces the same bytes, which is what
// makes persisted evidence and exported evidence byte comparable.
//
// Decoders are strict: unknown fields are ignored, but missing or ill typed
// fields are errors with a typed code. A decoder never guesses a value.
// ---------------------------------------------------------------------------

void write_json(core::JsonWriter& writer, const Timestamp& timestamp);
void write_json(core::JsonWriter& writer, const ObservationStamp& stamp);
void write_json(core::JsonWriter& writer, const core::Evidence& evidence);
void write_json(core::JsonWriter& writer, const HopObservation& hop);
void write_json(core::JsonWriter& writer, const MeasurementRecord& record);
void write_json(core::JsonWriter& writer, const SourceDescriptor& source);
void write_json(core::JsonWriter& writer, const EndpointDef& endpoint);
void write_json(core::JsonWriter& writer, const LinkDef& link);
void write_json(core::JsonWriter& writer, const QueueDef& queue);
void write_json(core::JsonWriter& writer, const HopDef& hop);
void write_json(core::JsonWriter& writer, const GenerationDef& generation);
void write_json(core::JsonWriter& writer, const PathDef& path);
void write_json(core::JsonWriter& writer, const ClockDomainDef& domain);
void write_json(core::JsonWriter& writer, const ClockSync& sync);

[[nodiscard]] Result<Timestamp> decode_timestamp(const core::JsonValue& value);
[[nodiscard]] Result<ObservationStamp> decode_observation_stamp(const core::JsonValue& value);
[[nodiscard]] Result<core::Evidence> decode_evidence(const core::JsonValue& value);
[[nodiscard]] Result<HopObservation> decode_hop_observation(const core::JsonValue& value);
[[nodiscard]] Result<MeasurementRecord> decode_measurement(const core::JsonValue& value);
[[nodiscard]] Result<SourceDescriptor> decode_source(const core::JsonValue& value);
[[nodiscard]] Result<EndpointDef> decode_endpoint(const core::JsonValue& value);
[[nodiscard]] Result<LinkDef> decode_link(const core::JsonValue& value);
[[nodiscard]] Result<QueueDef> decode_queue(const core::JsonValue& value);
[[nodiscard]] Result<HopDef> decode_hop(const core::JsonValue& value);
[[nodiscard]] Result<GenerationDef> decode_generation(const core::JsonValue& value);
[[nodiscard]] Result<PathDef> decode_path(const core::JsonValue& value);
[[nodiscard]] Result<ClockDomainDef> decode_clock_domain(const core::JsonValue& value);
[[nodiscard]] Result<ClockSync> decode_clock_sync(const core::JsonValue& value);

/// Parses a complete canonical document and decodes it.
[[nodiscard]] Result<MeasurementRecord> decode_measurement_text(std::string_view text,
                                                               const core::Limits& limits);
[[nodiscard]] Result<SourceDescriptor> decode_source_text(std::string_view text,
                                                          const core::Limits& limits);

/// Shared helpers used by every codec in the repository.
[[nodiscard]] Result<core::Name> json_name(const core::JsonValue& object, std::string_view field);
[[nodiscard]] Result<Revision> json_revision(const core::JsonValue& object, std::string_view field);
[[nodiscard]] Result<std::string> json_id_hex(const core::JsonValue& object, std::string_view field);
template <class Tag>
[[nodiscard]] Result<core::StrongId<Tag>> json_id(const core::JsonValue& object,
                                                  std::string_view field) {
  LATOBS_TRY(text, json_id_hex(object, field));
  return core::StrongId<Tag>::parse_hex(text);
}
[[nodiscard]] std::optional<std::string> json_optional_id_hex(const core::JsonValue& object,
                                                              std::string_view field);

}  // namespace latobs::model
