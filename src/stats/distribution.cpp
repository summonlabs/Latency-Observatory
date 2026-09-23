// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/stats/distribution.hpp"

#include <algorithm>
#include <limits>

#include "latency_observatory/core/digest.hpp"

namespace latobs::stats {
namespace {

constexpr std::uint64_t kI64Max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::uint64_t kI64MinAbs = kI64Max + 1ULL;

[[nodiscard]] core::UInt128 magnitude_of(Nanos value) noexcept {
  if (value >= 0) return core::UInt128::from_u64(static_cast<std::uint64_t>(value));
  return core::UInt128::from_u64(0ULL - static_cast<std::uint64_t>(value));
}

/// Exact signed sum held as two unsigned magnitudes so that no intermediate
/// value can overflow.
struct SignedSum {
  core::UInt128 positive;
  core::UInt128 negative;

  void add(Nanos value) noexcept {
    if (value >= 0) {
      positive = core::add_u128(positive, core::UInt128::from_u64(static_cast<std::uint64_t>(value)));
    } else {
      negative = core::add_u128(negative, magnitude_of(value));
    }
  }

  [[nodiscard]] bool is_zero() const noexcept { return positive.is_zero() && negative.is_zero(); }
  [[nodiscard]] bool overflowed() const noexcept {
    return core::is_saturated(positive) || core::is_saturated(negative);
  }

  [[nodiscard]] std::optional<Nanos> to_i64() const noexcept {
    if (overflowed()) return std::nullopt;
    if (is_zero()) return Nanos{0};
    const bool non_negative = !core::u128_less(positive, negative);
    const core::UInt128 diff = non_negative ? core::sub_u128(positive, negative)
                                            : core::sub_u128(negative, positive);
    if (non_negative) {
      if (!diff.fits_u64() || diff.lo > kI64Max) return std::nullopt;
      return static_cast<Nanos>(diff.lo);
    }
    if (!diff.fits_u64() || diff.lo > kI64MinAbs) return std::nullopt;
    if (diff.lo == kI64MinAbs) return std::numeric_limits<Nanos>::min();
    return -static_cast<Nanos>(diff.lo);
  }
};

[[nodiscard]] std::optional<Nanos> floor_mean(SignedSum sum, std::uint64_t count,
                                              std::uint64_t& remainder) noexcept {
  remainder = 0;
  if (count == 0) return std::nullopt;
  const std::optional<Nanos> total = sum.to_i64();
  if (!total.has_value()) return std::nullopt;
  const Nanos value = *total;
  const auto divisor = static_cast<std::int64_t>(count);
  Nanos quotient = value / divisor;
  Nanos rest = value % divisor;
  if (rest < 0) {
    // Floor division: the mean of a set with a negative total is never
    // reported as a value above the true mean.
    --quotient;
    rest += divisor;
  }
  remainder = static_cast<std::uint64_t>(rest);
  return quotient;
}

}  // namespace

Result<QuantileProbe> QuantileProbe::make(std::uint32_t numerator, std::uint32_t denominator) {
  if (denominator == 0) {
    return Error(ErrorCode::InvalidArgument, "quantile denominator must not be zero");
  }
  if (numerator == 0 || numerator > denominator) {
    return Error(ErrorCode::InvalidArgument, "quantile must satisfy 0 < numerator <= denominator");
  }
  return QuantileProbe(numerator, denominator);
}

std::string QuantileProbe::to_string() const {
  return std::to_string(numerator_) + "/" + std::to_string(denominator_);
}

std::vector<QuantileProbe> default_quantile_probes() {
  std::vector<QuantileProbe> probes;
  const std::uint32_t numerators[] = {50, 90, 99, 999};
  const std::uint32_t denominators[] = {100, 100, 100, 1000};
  for (std::size_t index = 0; index < 4; ++index) {
    const Result<QuantileProbe> probe = QuantileProbe::make(numerators[index], denominators[index]);
    LATOBS_ASSERT_MSG(probe.has_value(), "default quantile probes must be valid");
    probes.push_back(*probe);
  }
  return probes;
}

Result<HistogramSpec> HistogramSpec::make(std::vector<Nanos> bounds, const core::Limits& limits) {
  if (bounds.size() > limits.max_histogram_buckets) {
    return Error(ErrorCode::OutOfRange, "histogram bound count exceeds the configured limit",
                 std::to_string(bounds.size()));
  }
  for (std::size_t index = 0; index < bounds.size(); ++index) {
    if (bounds[index] < 0) {
      return Error(ErrorCode::InvalidArgument, "histogram bounds must not be negative",
                   std::to_string(bounds[index]));
    }
    if (index > 0 && bounds[index] <= bounds[index - 1]) {
      return Error(ErrorCode::InvalidArgument, "histogram bounds must be strictly increasing",
                   std::to_string(bounds[index]));
    }
  }
  HistogramSpec spec;
  spec.bounds = std::move(bounds);
  return spec;
}

HistogramSpec HistogramSpec::latency_default() {
  HistogramSpec spec;
  spec.bounds = {1000,        // 1us
                 2000,        // 2us
                 5000,        // 5us
                 10000,       // 10us
                 20000,       // 20us
                 50000,       // 50us
                 100000,      // 100us
                 200000,      // 200us
                 500000,      // 500us
                 1000000,     // 1ms
                 2000000,     // 2ms
                 5000000,     // 5ms
                 10000000,    // 10ms
                 20000000,    // 20ms
                 50000000,    // 50ms
                 100000000,   // 100ms
                 200000000,   // 200ms
                 500000000,   // 500ms
                 1000000000,  // 1s
                 2000000000,  // 2s
                 5000000000,  // 5s
                 10000000000};  // 10s, above which everything overflows
  return spec;
}

std::string HistogramSpec::canonical_text() const {
  std::string out = "histogram.bounds=";
  for (std::size_t index = 0; index < bounds.size(); ++index) {
    if (index != 0) out.push_back(',');
    out.append(std::to_string(bounds[index]));
  }
  out.push_back('\n');
  return out;
}

std::string HistogramSpec::digest() const { return core::sha256_hex(canonical_text()); }

Histogram Histogram::empty(const HistogramSpec& spec) {
  Histogram histogram;
  histogram.spec = spec;
  histogram.buckets.assign(spec.bucket_count(), 0);
  return histogram;
}

void Histogram::add(Nanos value) noexcept {
  if (buckets.size() != spec.bucket_count()) buckets.assign(spec.bucket_count(), 0);
  if (value < 0) {
    ++underflow;
    return;
  }
  std::size_t index = 0;
  while (index < spec.bounds.size() && value >= spec.bounds[index]) ++index;
  ++buckets[index];
}

std::uint64_t Histogram::total() const noexcept {
  std::uint64_t sum = underflow;
  for (const std::uint64_t bucket : buckets) sum += bucket;
  return sum;
}

std::optional<Nanos> nearest_rank(const std::vector<Nanos>& sorted,
                                  const QuantileProbe& probe) noexcept {
  if (sorted.empty()) return std::nullopt;
  const std::uint64_t count = static_cast<std::uint64_t>(sorted.size());
  const core::UInt128 product =
      core::mul_u64_to_u128(static_cast<std::uint64_t>(probe.numerator()), count);
  std::uint64_t remainder = 0;
  const core::UInt128 quotient =
      core::divmod_u128_u64(product, static_cast<std::uint64_t>(probe.denominator()), remainder);
  std::uint64_t rank = quotient.lo;
  if (remainder != 0) ++rank;
  if (rank == 0) rank = 1;
  if (rank > count) rank = count;
  return sorted[static_cast<std::size_t>(rank - 1)];
}

Result<Distribution> aggregate(std::vector<Nanos> values, const HistogramSpec& spec,
                               const std::vector<QuantileProbe>& probes,
                               const core::Limits& limits) {
  if (values.size() > limits.max_samples_per_path) {
    return Error(ErrorCode::OutOfRange, "aggregation input exceeds the configured sample limit",
                 std::to_string(values.size()));
  }
  if (probes.size() > limits.max_quantile_probes) {
    return Error(ErrorCode::OutOfRange, "quantile probe count exceeds the configured limit",
                 std::to_string(probes.size()));
  }

  Distribution distribution;
  distribution.histogram = Histogram::empty(spec);
  distribution.count = static_cast<std::uint64_t>(values.size());
  // The probe list is always reported, even when no value was observed: an
  // empty distribution has unknown quantiles, not an empty description.
  distribution.probes = probes;
  distribution.quantiles.assign(probes.size(), std::nullopt);
  if (values.empty()) return distribution;

  std::sort(values.begin(), values.end());
  SignedSum sum;
  for (const Nanos value : values) {
    sum.add(value);
    distribution.histogram.add(value);
  }
  distribution.min_ns = values.front();
  distribution.max_ns = values.back();
  std::uint64_t remainder = 0;
  distribution.sum_ns = sum.to_i64();
  // The flag means "the exact sum is not representable as a signed 64 bit
  // value", whether because the accumulator saturated or because the true sum
  // lies outside the range.
  if (sum.overflowed() || !distribution.sum_ns.has_value()) {
    distribution.sum_overflowed = true;
  }
  distribution.mean_ns = floor_mean(sum, distribution.count, remainder);
  distribution.mean_remainder_ns = remainder;

  for (std::size_t index = 0; index < probes.size(); ++index) {
    distribution.quantiles[index] = nearest_rank(values, probes[index]);
  }
  return distribution;
}

Result<Distribution> aggregate_single(Nanos value, const HistogramSpec& spec,
                                      const core::Limits& limits) {
  std::vector<Nanos> values;
  values.push_back(value);
  return aggregate(std::move(values), spec, {}, limits);
}


void write_json(core::JsonWriter& writer, const QuantileProbe& probe) {
  writer.begin_object();
  writer.field("probe", probe.to_string());
  writer.field("numerator", static_cast<std::uint64_t>(probe.numerator()));
  writer.field("denominator", static_cast<std::uint64_t>(probe.denominator()));
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const HistogramSpec& spec) {
  writer.begin_object();
  writer.field("digest", spec.digest());
  writer.field_array("bounds");
  for (const Nanos bound : spec.bounds) writer.value_int(bound);
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const Histogram& histogram) {
  writer.begin_object();
  writer.key("spec");
  write_json(writer, histogram.spec);
  writer.field("underflow", histogram.underflow);
  writer.field_array("buckets");
  for (std::size_t index = 0; index < histogram.buckets.size(); ++index) {
    writer.begin_object();
    writer.field("index", static_cast<std::uint64_t>(index));
    // A bucket is [lower, upper); the last bucket is the overflow bucket.
    if (index < histogram.spec.bounds.size()) {
      if (index == 0) {
        writer.field("lower_ns", static_cast<std::int64_t>(0));
      } else {
        writer.field("lower_ns", histogram.spec.bounds[index - 1]);
      }
      writer.field("upper_ns", histogram.spec.bounds[index]);
    } else {
      writer.field("lower_ns",
                   histogram.spec.bounds.empty() ? static_cast<std::int64_t>(0)
                                                 : histogram.spec.bounds.back());
      writer.field_null("upper_ns");
    }
    writer.field("count", histogram.buckets[index]);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const Distribution& distribution) {
  writer.begin_object();
  writer.field("count", distribution.count);
  writer.field_optional_int("min_ns", distribution.min_ns);
  writer.field_optional_int("max_ns", distribution.max_ns);
  writer.field_optional_int("sum_ns", distribution.sum_ns);
  writer.field_optional_int("mean_ns", distribution.mean_ns);
  writer.field("mean_remainder_ns", distribution.mean_remainder_ns);
  writer.field("sum_overflowed", distribution.sum_overflowed);
  writer.key("histogram");
  write_json(writer, distribution.histogram);
  writer.field_array("quantiles");
  for (std::size_t index = 0; index < distribution.quantiles.size(); ++index) {
    writer.begin_object();
    if (index < distribution.probes.size()) {
      writer.field("probe", distribution.probes[index].to_string());
    } else {
      writer.field("probe", "unknown");
    }
    writer.field_optional_int("value_ns", distribution.quantiles[index]);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

std::string render_distribution_text(const Distribution& distribution) {
  std::string out = "count=" + std::to_string(distribution.count);
  out.append(" min=");
  out.append(distribution.min_ns.has_value() ? std::to_string(*distribution.min_ns) : "unknown");
  out.append(" max=");
  out.append(distribution.max_ns.has_value() ? std::to_string(*distribution.max_ns) : "unknown");
  out.append(" mean=");
  out.append(distribution.mean_ns.has_value() ? std::to_string(*distribution.mean_ns) : "unknown");
  if (distribution.sum_overflowed) out.append(" overflow=true");
  return out;
}

}  // namespace latobs::stats
