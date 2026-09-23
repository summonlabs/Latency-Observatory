// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/core/checked.hpp"
#include "latency_observatory/core/json.hpp"
#include "latency_observatory/core/policy.hpp"
#include "latency_observatory/stats/vocabulary.hpp"

namespace latobs::stats {

using core::Nanos;

/// An exact rational quantile probe. Rationals are used instead of doubles so
/// that the same probe always selects the same rank on every platform.
class QuantileProbe {
 public:
  constexpr QuantileProbe() noexcept = default;

  [[nodiscard]] static Result<QuantileProbe> make(std::uint32_t numerator,
                                                  std::uint32_t denominator);

  [[nodiscard]] constexpr std::uint32_t numerator() const noexcept { return numerator_; }
  [[nodiscard]] constexpr std::uint32_t denominator() const noexcept { return denominator_; }
  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] constexpr bool operator==(const QuantileProbe& other) const noexcept {
    return numerator_ == other.numerator_ && denominator_ == other.denominator_;
  }
  [[nodiscard]] constexpr bool operator<(const QuantileProbe& other) const noexcept {
    return static_cast<std::uint64_t>(numerator_) * other.denominator_ <
           static_cast<std::uint64_t>(other.numerator_) * denominator_;
  }

 private:
  constexpr QuantileProbe(std::uint32_t numerator, std::uint32_t denominator) noexcept
      : numerator_(numerator), denominator_(denominator) {}
  std::uint32_t numerator_ = 0;
  std::uint32_t denominator_ = 1;
};

/// The probes reported by default, in deterministic ascending order.
[[nodiscard]] std::vector<QuantileProbe> default_quantile_probes();

/// An explicit, ordered histogram bucketing. Values are counted as
/// [bounds[i-1], bounds[i]) with an explicit overflow bucket above the last
/// bound and an explicit underflow counter below zero.
struct HistogramSpec {
  std::vector<Nanos> bounds;

  [[nodiscard]] static Result<HistogramSpec> make(std::vector<Nanos> bounds,
                                                  const core::Limits& limits);
  /// Latency oriented default: microsecond to ten second coverage.
  [[nodiscard]] static HistogramSpec latency_default();

  [[nodiscard]] std::size_t bucket_count() const noexcept { return bounds.size() + 1; }
  [[nodiscard]] std::string canonical_text() const;
  [[nodiscard]] std::string digest() const;
  [[nodiscard]] bool operator==(const HistogramSpec& other) const noexcept {
    return bounds == other.bounds;
  }
};

struct Histogram {
  HistogramSpec spec;
  /// buckets[i] counts values in [bounds[i-1], bounds[i]); the final entry is
  /// the overflow bucket for values >= bounds.back().
  std::vector<std::uint64_t> buckets;
  std::uint64_t underflow = 0;  // values < 0

  [[nodiscard]] static Histogram empty(const HistogramSpec& spec);
  void add(Nanos value) noexcept;
  [[nodiscard]] std::uint64_t total() const noexcept;
};

/// The deterministic aggregate of a bounded set of values. Every field is
/// order independent, so the same set of samples always produces the same
/// distribution regardless of the order in which they were observed.
struct Distribution {
  std::uint64_t count = 0;
  std::optional<Nanos> min_ns;
  std::optional<Nanos> max_ns;
  std::optional<Nanos> sum_ns;
  std::optional<Nanos> mean_ns;         // floor division of sum by count
  std::uint64_t mean_remainder_ns = 0;  // exact remainder, never rounded away
  bool sum_overflowed = false;
  Histogram histogram;
  std::vector<QuantileProbe> probes;            // the probes that produced the values
  std::vector<std::optional<Nanos>> quantiles;  // aligned with probes

  [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

/// Aggregates a bounded, unordered set of values. The probe list is applied to
/// the sorted values using the nearest rank definition:
///   rank = ceil(numerator * count / denominator), clamped to [1, count];
///   the reported value is the value at that 1-based rank.
/// This definition is exact for every probe and independent of rounding mode.
[[nodiscard]] Result<Distribution> aggregate(std::vector<Nanos> values, const HistogramSpec& spec,
                                             const std::vector<QuantileProbe>& probes,
                                             const core::Limits& limits);

/// Aggregates a single value into an existing distribution (used for the
/// streaming accumulator path). The quantile slots are left cleared.
[[nodiscard]] Result<Distribution> aggregate_single(Nanos value, const HistogramSpec& spec,
                                                    const core::Limits& limits);

/// Value at an exact rational rank in an already sorted sequence.
[[nodiscard]] std::optional<Nanos> nearest_rank(const std::vector<Nanos>& sorted,
                                                const QuantileProbe& probe) noexcept;

/// Deterministic rendering shared by the tooling.
[[nodiscard]] std::string render_distribution_text(const Distribution& distribution);

/// Canonical JSON rendering. Unknown quantities are emitted as null with the
/// evidence state alongside them; they are never emitted as zero.
void write_json(core::JsonWriter& writer, const QuantileProbe& probe);
void write_json(core::JsonWriter& writer, const HistogramSpec& spec);
void write_json(core::JsonWriter& writer, const Histogram& histogram);
void write_json(core::JsonWriter& writer, const Distribution& distribution);

}  // namespace latobs::stats
