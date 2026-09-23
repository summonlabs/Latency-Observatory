// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/stats/summary.hpp"

namespace latobs::stats {

/// Decoders for the canonical distributions. They are strict: a malformed
/// histogram or an unparsable quantile probe is an error, never a default.
[[nodiscard]] Result<HistogramSpec> decode_histogram_spec(const core::JsonValue& value);
/// Parses a probe written as "numerator/denominator".
[[nodiscard]] Result<QuantileProbe> parse_quantile_probe(std::string_view text);
[[nodiscard]] Result<Histogram> decode_histogram(const core::JsonValue& value);
[[nodiscard]] Result<Distribution> decode_distribution(const core::JsonValue& value);
[[nodiscard]] Result<HopSummary> decode_hop_summary(const core::JsonValue& value);

/// Encodes a time window together with its clock domain.
void write_json(core::JsonWriter& writer, const TimeWindow& window);
[[nodiscard]] Result<TimeWindow> decode_time_window(const core::JsonValue& value);

}  // namespace latobs::stats
