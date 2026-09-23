// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/ingest/codec.hpp"

#include "latency_observatory/model/codec.hpp"

namespace latobs::ingest {

void write_json(core::JsonWriter& writer, SourceId source, const SourceFenceState& state) {
  writer.begin_object();
  writer.field("kind", "fence_state");
  writer.field("source", source.to_hex());
  writer.field("next_ordinal", state.next_ordinal);
  writer.field("current_epoch",
               state.current_epoch.valid() ? state.current_epoch.to_hex() : std::string());
  writer.field("current_incarnation", state.current_incarnation.valid()
                                         ? state.current_incarnation.to_hex()
                                         : std::string());
  writer.field("current_ordinal", state.current_ordinal);
  writer.field("evicted_sessions", state.evicted_sessions);
  writer.field("seen_revision",
               state.current_ordinal == 0 ? 0ULL
                                          : static_cast<std::uint64_t>(1));
  writer.field_array("sessions");
  for (const auto& entry : state.sessions) {
    const SessionState& session = entry.second;
    writer.begin_object();
    writer.field("epoch", entry.first.first.to_hex());
    writer.field("incarnation", entry.first.second.to_hex());
    writer.field("ordinal", session.ordinal);
    writer.field("source_revision", static_cast<std::uint64_t>(session.source_revision.value()));
    writer.field("has_last_sequence", session.has_last_sequence);
    writer.field("last_sequence", session.last_sequence.value());
    writer.field("accepted_current", session.accepted_current);
    writer.field("accepted_historical", session.accepted_historical);
    writer.field("rejected", session.rejected);
    writer.field_array("recent_sequences");
    for (const std::uint64_t sequence : session.recent_order) writer.value_uint(sequence);
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

Result<std::pair<SourceId, SourceFenceState>> decode_fence_state(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "fence state must be an object");
  }
  LATOBS_TRY(source, model::json_id<core::SourceTag>(value, "source"));
  SourceFenceState state;
  LATOBS_TRY(next_ordinal, core::json_require_uint(value, "next_ordinal"));
  state.next_ordinal = next_ordinal;
  LATOBS_TRY(current_ordinal, core::json_require_uint(value, "current_ordinal"));
  state.current_ordinal = current_ordinal;
  LATOBS_TRY(evicted, core::json_require_uint(value, "evicted_sessions"));
  state.evicted_sessions = evicted;
  if (const std::optional<std::string> epoch = model::json_optional_id_hex(value, "current_epoch");
      epoch.has_value()) {
    LATOBS_TRY(parsed, EpochId::parse_hex(*epoch));
    state.current_epoch = parsed;
  }
  if (const std::optional<std::string> incarnation =
          model::json_optional_id_hex(value, "current_incarnation");
      incarnation.has_value()) {
    LATOBS_TRY(parsed, IncarnationId::parse_hex(*incarnation));
    state.current_incarnation = parsed;
  }
  LATOBS_TRY(sessions, core::json_require_array(value, "sessions"));
  LATOBS_TRY(session_array, sessions->as_array());
  for (const core::JsonValue& entry : *session_array) {
    LATOBS_TRY(epoch, model::json_id<core::EpochTag>(entry, "epoch"));
    LATOBS_TRY(incarnation, model::json_id<core::IncarnationTag>(entry, "incarnation"));
    SessionState session;
    LATOBS_TRY(ordinal, core::json_require_uint(entry, "ordinal"));
    session.ordinal = ordinal;
    LATOBS_TRY(revision, model::json_revision(entry, "source_revision"));
    session.source_revision = revision;
    LATOBS_TRY(has_last, core::json_require_bool(entry, "has_last_sequence"));
    session.has_last_sequence = has_last;
    LATOBS_TRY(last_sequence, core::json_require_uint(entry, "last_sequence"));
    session.last_sequence = Sequence::from_value(last_sequence);
    LATOBS_TRY(accepted_current, core::json_require_uint(entry, "accepted_current"));
    session.accepted_current = accepted_current;
    LATOBS_TRY(accepted_historical, core::json_require_uint(entry, "accepted_historical"));
    session.accepted_historical = accepted_historical;
    LATOBS_TRY(rejected, core::json_require_uint(entry, "rejected"));
    session.rejected = rejected;
    LATOBS_TRY(sequences, core::json_require_array(entry, "recent_sequences"));
    LATOBS_TRY(sequence_array, sequences->as_array());
    for (const core::JsonValue& item : *sequence_array) {
      LATOBS_TRY(sequence, item.as_uint64());
      session.recent_sequences.insert(sequence);
      session.recent_order.push_back(sequence);
    }
    state.sessions.emplace(std::make_pair(epoch, incarnation), std::move(session));
  }
  return std::make_pair(source, std::move(state));
}

}  // namespace latobs::ingest
