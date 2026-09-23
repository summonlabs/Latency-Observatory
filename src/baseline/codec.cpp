// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/baseline/codec.hpp"

#include "latency_observatory/model/codec.hpp"
#include "latency_observatory/stats/codec.hpp"

namespace latobs::baseline {

Result<Baseline> decode_baseline(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "baseline must be an object");
  }
  Baseline baseline;
  LATOBS_TRY(id, model::json_id<core::BaselineTag>(value, "baseline"));
  baseline.id = id;
  LATOBS_TRY(name, model::json_name(value, "name"));
  baseline.name = name;
  LATOBS_TRY(path, model::json_id<core::PathTag>(value, "path"));
  baseline.path = path;
  LATOBS_TRY(generation, model::json_id<core::GenerationTag>(value, "generation"));
  baseline.generation = generation;
  LATOBS_TRY(domain, model::json_id<core::ClockDomainTag>(value, "clock_domain"));
  baseline.domain = domain;
  LATOBS_TRY(revision, model::json_revision(value, "revision"));
  baseline.revision = revision;
  if (const std::optional<std::string> source = model::json_optional_id_hex(value, "source");
      source.has_value()) {
    LATOBS_TRY(parsed, SourceId::parse_hex(*source));
    baseline.source = parsed;
  }
  LATOBS_TRY(synthetic, core::json_require_bool(value, "synthetic"));
  baseline.synthetic = synthetic;
  LATOBS_TRY(created_at, core::json_require_int(value, "created_at_ns"));
  baseline.created_at = Timestamp{created_at, baseline.domain};
  LATOBS_TRY(exchange_count, core::json_require_uint(value, "exchange_count"));
  baseline.exchange_count = exchange_count;
  LATOBS_TRY(policy_digest, core::json_require_string(value, "policy_digest"));
  baseline.policy_digest = std::string(policy_digest);
  LATOBS_TRY(histogram_value, core::json_require_object(value, "histogram"));
  LATOBS_TRY(histogram, stats::decode_histogram_spec(*histogram_value));
  baseline.histogram = histogram;
  LATOBS_TRY(probes_value, core::json_require_array(value, "probes"));
  LATOBS_TRY(probes, probes_value->as_array());
  for (const core::JsonValue& entry : *probes) {
    LATOBS_TRY(text, entry.as_string());
    LATOBS_TRY(probe, stats::parse_quantile_probe(text));
    baseline.probes.push_back(probe);
  }
  LATOBS_TRY(window_value, core::json_require_object(value, "window"));
  LATOBS_TRY(window, stats::decode_time_window(*window_value));
  baseline.window = window;
  LATOBS_TRY(end_to_end_value, core::json_require_object(value, "end_to_end"));
  LATOBS_TRY(end_to_end, stats::decode_distribution(*end_to_end_value));
  baseline.end_to_end = std::move(end_to_end);
  LATOBS_TRY(hops, core::json_require_array(value, "hops"));
  LATOBS_TRY(hops_array, hops->as_array());
  baseline.hops.reserve(hops_array->size());
  for (const core::JsonValue& entry : *hops_array) {
    LATOBS_TRY(hop, stats::decode_hop_summary(entry));
    baseline.hops.push_back(std::move(hop));
  }
  LATOBS_TRY(state_text, core::json_require_string(value, "evidence_state"));
  core::EvidenceState state = core::EvidenceState::Unknown;
  if (!core::parse_evidence_state(state_text, state)) {
    return Error(ErrorCode::ParseError, "unknown evidence state", std::string(state_text));
  }
  baseline.evidence.set_state(state);
  return baseline;
}

Result<Baseline> decode_baseline_text(std::string_view text, const core::Limits& limits) {
  if (text.size() > limits.max_json_bytes) {
    return Error(ErrorCode::OutOfRange, "baseline document exceeds the configured size limit");
  }
  LATOBS_TRY(document, core::parse_json(text, limits.max_json_depth));
  return decode_baseline(document);
}

}  // namespace latobs::baseline
