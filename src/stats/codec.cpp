// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/stats/codec.hpp"

#include <string>

#include "latency_observatory/model/codec.hpp"

namespace latobs::stats {
namespace {

}  // namespace

Result<QuantileProbe> parse_quantile_probe(std::string_view text) {
  const std::size_t separator = text.find('/');
  if (separator == std::string_view::npos) {
    return Error(ErrorCode::ParseError, "quantile probe is not of the form n/d", std::string(text));
  }
  const std::optional<std::uint64_t> numerator = core::parse_u64(text.substr(0, separator));
  const std::optional<std::uint64_t> denominator = core::parse_u64(text.substr(separator + 1));
  if (!numerator.has_value() || !denominator.has_value() || *numerator > 0xFFFFFFFFULL ||
      *denominator > 0xFFFFFFFFULL) {
    return Error(ErrorCode::ParseError, "quantile probe is not a valid rational",
                 std::string(text));
  }
  return QuantileProbe::make(static_cast<std::uint32_t>(*numerator),
                             static_cast<std::uint32_t>(*denominator));
}

void write_json(core::JsonWriter& writer, const TimeWindow& window) {
  writer.begin_object();
  writer.field("from_ns", window.from.ns);
  writer.field("to_ns", window.to.ns);
  writer.field("clock_domain", window.from.domain.valid() ? window.from.domain.to_hex()
                                                          : std::string());
  writer.end_object();
}

Result<TimeWindow> decode_time_window(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "time window must be an object");
  }
  LATOBS_TRY(from_ns, core::json_require_int(value, "from_ns"));
  LATOBS_TRY(to_ns, core::json_require_int(value, "to_ns"));
  LATOBS_TRY(domain_text, core::json_require_string(value, "clock_domain"));
  if (domain_text.empty()) {
    return Error(ErrorCode::ParseError, "time window has no clock domain");
  }
  LATOBS_TRY(domain, ClockDomainId::parse_hex(domain_text));
  return TimeWindow::make(Timestamp{from_ns, domain}, Timestamp{to_ns, domain});
}

Result<HistogramSpec> decode_histogram_spec(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "histogram spec must be an object");
  }
  LATOBS_TRY(bounds, core::json_require_array(value, "bounds"));
  std::vector<Nanos> parsed;
  LATOBS_TRY(items, bounds->as_array());
  parsed.reserve(items->size());
  for (const core::JsonValue& entry : *items) {
    LATOBS_TRY(bound, entry.as_int64());
    parsed.push_back(bound);
  }
  core::Limits limits;
  return HistogramSpec::make(std::move(parsed), limits);
}

Result<Histogram> decode_histogram(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "histogram must be an object");
  }
  LATOBS_TRY(spec_value, core::json_require_object(value, "spec"));
  LATOBS_TRY(spec, decode_histogram_spec(*spec_value));
  LATOBS_TRY(underflow, core::json_require_uint(value, "underflow"));
  LATOBS_TRY(bucket_value, core::json_require_array(value, "buckets"));
  LATOBS_TRY(bucket_values, bucket_value->as_array());
  Histogram histogram = Histogram::empty(spec);
  histogram.underflow = underflow;
  std::size_t index = 0;
  for (const core::JsonValue& entry : *bucket_values) {
    LATOBS_TRY(entry_index, core::json_require_uint(entry, "index"));
    LATOBS_TRY(count, core::json_require_uint(entry, "count"));
    if (entry_index != index || index >= histogram.buckets.size()) {
      return Error(ErrorCode::ParseError, "histogram buckets are not dense and ordered");
    }
    histogram.buckets[index] = count;
    ++index;
  }
  if (index != histogram.buckets.size()) {
    return Error(ErrorCode::ParseError, "histogram bucket count does not match its specification");
  }
  return histogram;
}

Result<Distribution> decode_distribution(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "distribution must be an object");
  }
  Distribution distribution;
  LATOBS_TRY(count, core::json_require_uint(value, "count"));
  distribution.count = count;
  LATOBS_TRY(minimum, core::json_optional_int(value, "min_ns"));
  distribution.min_ns = minimum;
  LATOBS_TRY(maximum, core::json_optional_int(value, "max_ns"));
  distribution.max_ns = maximum;
  LATOBS_TRY(sum, core::json_optional_int(value, "sum_ns"));
  distribution.sum_ns = sum;
  LATOBS_TRY(mean, core::json_optional_int(value, "mean_ns"));
  distribution.mean_ns = mean;
  LATOBS_TRY(remainder, core::json_require_uint(value, "mean_remainder_ns"));
  distribution.mean_remainder_ns = remainder;
  LATOBS_TRY(overflowed, core::json_require_bool(value, "sum_overflowed"));
  distribution.sum_overflowed = overflowed;
  LATOBS_TRY(histogram_value, core::json_require_object(value, "histogram"));
  LATOBS_TRY(histogram, decode_histogram(*histogram_value));
  distribution.histogram = std::move(histogram);
  LATOBS_TRY(quantile_value_array, core::json_require_array(value, "quantiles"));
  LATOBS_TRY(quantiles, quantile_value_array->as_array());
  for (const core::JsonValue& entry : *quantiles) {
    LATOBS_TRY(probe_text, core::json_require_string(entry, "probe"));
    LATOBS_TRY(probe, parse_quantile_probe(probe_text));
    LATOBS_TRY(quantile_value, core::json_optional_int(entry, "value_ns"));
    distribution.probes.push_back(probe);
    distribution.quantiles.push_back(quantile_value);
  }
  if (distribution.count == 0 && (distribution.min_ns.has_value() ||
                                  distribution.max_ns.has_value() ||
                                  distribution.mean_ns.has_value())) {
    return Error(ErrorCode::ParseError, "an empty distribution must not carry values");
  }
  return distribution;
}

Result<HopSummary> decode_hop_summary(const core::JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ParseError, "hop summary must be an object");
  }
  HopSummary summary;
  LATOBS_TRY(index_value, core::json_require_uint(value, "index"));
  if (index_value > 0xFFFFFFFFULL) {
    return Error(ErrorCode::OutOfRange, "hop index is outside the supported range");
  }
  LATOBS_TRY(index, HopIndex::from_value(static_cast<std::uint32_t>(index_value)));
  summary.index = index;
  LATOBS_TRY(hop, model::json_id<core::HopTag>(value, "hop"));
  summary.hop = hop;
  LATOBS_TRY(included, core::json_require_uint(value, "exchanges_included"));
  summary.exchanges_included = included;
  LATOBS_TRY(observed, core::json_require_uint(value, "dwells_observed"));
  summary.dwells_observed = observed;
  LATOBS_TRY(missing, core::json_require_uint(value, "dwells_missing"));
  summary.dwells_missing = missing;
  LATOBS_TRY(unsupported, core::json_require_uint(value, "dwells_unsupported"));
  summary.dwells_unsupported = unsupported;
  LATOBS_TRY(queue_observed, core::json_require_uint(value, "queue_dwells_observed"));
  summary.queue_dwells_observed = queue_observed;
  LATOBS_TRY(queue_unsupported, core::json_require_uint(value, "queue_dwells_unsupported"));
  summary.queue_dwells_unsupported = queue_unsupported;
  LATOBS_TRY(coverage, core::json_require_uint(value, "coverage_ppm"));
  summary.coverage_ppm = coverage;
  LATOBS_TRY(dwell_value, core::json_require_object(value, "dwell"));
  LATOBS_TRY(dwell, decode_distribution(*dwell_value));
  summary.dwell = std::move(dwell);
  LATOBS_TRY(queue_value, core::json_require_object(value, "queue_dwell"));
  LATOBS_TRY(queue_dwell, decode_distribution(*queue_value));
  summary.queue_dwell = std::move(queue_dwell);
  LATOBS_TRY(state_text, core::json_require_string(value, "evidence_state"));
  core::EvidenceState state = core::EvidenceState::Unknown;
  if (!core::parse_evidence_state(state_text, state)) {
    return Error(ErrorCode::ParseError, "unknown evidence state", std::string(state_text));
  }
  summary.evidence.set_state(state);
  if (const core::JsonValue* freshness = value.find("freshness");
      freshness != nullptr && freshness->is_string()) {
    LATOBS_TRY(text, freshness->as_string());
    core::Freshness parsed = core::Freshness::Unknown;
    if (core::parse_freshness(text, parsed)) summary.evidence.set_freshness(parsed);
  }
  if (const core::JsonValue* confidence = value.find("confidence");
      confidence != nullptr && confidence->is_string()) {
    LATOBS_TRY(text, confidence->as_string());
    core::Confidence parsed = core::Confidence::None;
    if (core::parse_confidence(text, parsed)) summary.evidence.set_confidence(parsed);
  }
  if (const core::JsonValue* reasons = value.find("reasons");
      reasons != nullptr && reasons->is_array()) {
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
      summary.evidence.add_reason(code, std::move(detail));
    }
  }
  return summary;
}

}  // namespace latobs::stats
