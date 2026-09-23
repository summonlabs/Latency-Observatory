// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <limits>
#include <string>
#include <vector>

#include "latency_observatory/stats/codec.hpp"
#include "latency_observatory/stats/distribution.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

stats::HistogramSpec fixed_spec() {
  std::vector<core::Nanos> bounds = {100, 200, 300};
  const Result<stats::HistogramSpec> spec =
      stats::HistogramSpec::make(std::move(bounds), core::Limits{});
  CHECK(spec.has_value());
  return spec.value();
}

std::vector<stats::QuantileProbe> fixed_probes() {
  std::vector<stats::QuantileProbe> probes;
  const Result<stats::QuantileProbe> p50 = stats::QuantileProbe::make(50, 100);
  const Result<stats::QuantileProbe> p90 = stats::QuantileProbe::make(90, 100);
  CHECK(p50.has_value());
  CHECK(p90.has_value());
  probes.push_back(p50.value());
  probes.push_back(p90.value());
  return probes;
}

}  // namespace

LATOBS_TEST(stats, quantile_probes_are_exact_rationals) {
  CHECK_OK(half, stats::QuantileProbe::make(1, 2));
  CHECK_OK(ninetynine, stats::QuantileProbe::make(99, 100));
  CHECK_ERR(error_zero_numerator, stats::QuantileProbe::make(0, 100));
  CHECK_ERR(error_above_one, stats::QuantileProbe::make(101, 100));
  CHECK_ERR(error_zero_denominator, stats::QuantileProbe::make(1, 0));
  CHECK(half < ninetynine);
  CHECK_EQ(half.to_string(), std::string("1/2"));
  const std::vector<stats::QuantileProbe> defaults = stats::default_quantile_probes();
  CHECK_EQ(defaults.size(), std::size_t{4});
  CHECK_EQ(defaults[0].to_string(), std::string("50/100"));
  CHECK_EQ(defaults[3].to_string(), std::string("999/1000"));
}

LATOBS_TEST(stats, nearest_rank_definition) {
  const std::vector<core::Nanos> sorted = {10, 20, 30, 40, 50};
  CHECK_OK(p50, stats::QuantileProbe::make(50, 100));
  CHECK_OK(p90, stats::QuantileProbe::make(90, 100));
  CHECK_OK(p100, stats::QuantileProbe::make(100, 100));
  CHECK_OK(p20, stats::QuantileProbe::make(20, 100));
  CHECK_EQ(*stats::nearest_rank(sorted, p50), 30);
  CHECK_EQ(*stats::nearest_rank(sorted, p90), 50);
  CHECK_EQ(*stats::nearest_rank(sorted, p100), 50);
  CHECK_EQ(*stats::nearest_rank(sorted, p20), 10);
  CHECK(!stats::nearest_rank({}, p50).has_value());
  const std::vector<core::Nanos> single = {7};
  CHECK_EQ(*stats::nearest_rank(single, p100), 7);
}

LATOBS_TEST(stats, aggregation_is_exact) {
  const stats::HistogramSpec spec = fixed_spec();
  std::vector<core::Nanos> values = {150, 50, 250, 350, 150};
  CHECK_OK(distribution, stats::aggregate(values, spec, fixed_probes(), core::Limits{}));
  CHECK_EQ(distribution.count, std::uint64_t{5});
  CHECK_EQ(*distribution.min_ns, 50);
  CHECK_EQ(*distribution.max_ns, 350);
  CHECK_EQ(*distribution.sum_ns, 950);
  CHECK_EQ(*distribution.mean_ns, 190);
  CHECK_EQ(distribution.mean_remainder_ns, std::uint64_t{0});
  CHECK_EQ(distribution.histogram.buckets.size(), std::size_t{4});
  CHECK_EQ(distribution.histogram.buckets[0], std::uint64_t{1});
  CHECK_EQ(distribution.histogram.buckets[1], std::uint64_t{2});
  CHECK_EQ(distribution.histogram.buckets[2], std::uint64_t{1});
  CHECK_EQ(distribution.histogram.buckets[3], std::uint64_t{1});
  CHECK_EQ(distribution.histogram.total(), std::uint64_t{5});
  CHECK_EQ(distribution.quantiles.size(), std::size_t{2});
  CHECK_EQ(*distribution.quantiles[0], 150);
  CHECK_EQ(*distribution.quantiles[1], 350);

  std::vector<core::Nanos> awkward = {1, 2};
  CHECK_OK(awkward_distribution, stats::aggregate(awkward, spec, fixed_probes(), core::Limits{}));
  CHECK_EQ(*awkward_distribution.mean_ns, 1);
  CHECK_EQ(awkward_distribution.mean_remainder_ns, std::uint64_t{1});
}

LATOBS_TEST(stats, aggregation_handles_negatives_and_overflow) {
  const stats::HistogramSpec spec = fixed_spec();
  std::vector<core::Nanos> mixed = {-50, 50};
  CHECK_OK(distribution, stats::aggregate(mixed, spec, fixed_probes(), core::Limits{}));
  CHECK_EQ(*distribution.min_ns, -50);
  CHECK_EQ(*distribution.max_ns, 50);
  CHECK_EQ(*distribution.sum_ns, 0);
  CHECK_EQ(*distribution.mean_ns, 0);
  CHECK_EQ(distribution.histogram.underflow, std::uint64_t{1});
  CHECK_EQ(distribution.histogram.total(), std::uint64_t{2});

  std::vector<core::Nanos> huge = {std::numeric_limits<core::Nanos>::max(),
                                   std::numeric_limits<core::Nanos>::max()};
  CHECK_OK(huge_distribution, stats::aggregate(huge, spec, fixed_probes(), core::Limits{}));
  CHECK(!huge_distribution.sum_ns.has_value());
  CHECK(!huge_distribution.mean_ns.has_value());
  CHECK(huge_distribution.sum_overflowed);
  CHECK_EQ(*huge_distribution.max_ns, std::numeric_limits<core::Nanos>::max());
}

LATOBS_TEST(stats, empty_and_single_aggregation) {
  const stats::HistogramSpec spec = fixed_spec();
  std::vector<core::Nanos> empty;
  CHECK_OK(distribution, stats::aggregate(empty, spec, fixed_probes(), core::Limits{}));
  CHECK(distribution.empty());
  CHECK(!distribution.min_ns.has_value());
  CHECK(!distribution.mean_ns.has_value());
  CHECK_EQ(distribution.histogram.total(), std::uint64_t{0});
  CHECK_EQ(distribution.quantiles.size(), std::size_t{2});
  CHECK(!distribution.quantiles[0].has_value());
}

LATOBS_TEST(stats, aggregation_is_order_independent) {
  const stats::HistogramSpec spec = fixed_spec();
  const std::vector<stats::QuantileProbe> probes = fixed_probes();
  std::vector<core::Nanos> base;
  Rng rng(7);
  for (int index = 0; index < 200; ++index) {
    base.push_back(rng.signed_range(0, 400));
  }
  CHECK_OK(reference, stats::aggregate(base, spec, probes, core::Limits{}));
  std::string reference_text;
  {
    core::JsonWriter writer(reference_text);
    stats::write_json(writer, reference);
  }
  for (int trial = 0; trial < 40; ++trial) {
    std::vector<core::Nanos> shuffled = base;
    for (std::size_t index = shuffled.size(); index > 1; --index) {
      const std::size_t other = static_cast<std::size_t>(rng.range(0, index - 1));
      std::swap(shuffled[index - 1], shuffled[other]);
    }
    CHECK_OK(candidate, stats::aggregate(shuffled, spec, probes, core::Limits{}));
    std::string candidate_text;
    core::JsonWriter writer(candidate_text);
    stats::write_json(writer, candidate);
    CHECK_EQ(candidate_text, reference_text);
  }
}

LATOBS_TEST(stats, histogram_spec_validation) {
  std::vector<core::Nanos> unsorted = {300, 100};
  CHECK_ERR(error_unsorted, stats::HistogramSpec::make(std::move(unsorted), core::Limits{}));
  std::vector<core::Nanos> duplicate = {100, 100};
  CHECK_ERR(error_duplicate, stats::HistogramSpec::make(std::move(duplicate), core::Limits{}));
  std::vector<core::Nanos> negative = {-1};
  CHECK_ERR(error_negative, stats::HistogramSpec::make(std::move(negative), core::Limits{}));
  core::Limits small;
  small.max_histogram_buckets = 2;
  std::vector<core::Nanos> too_many = {1, 2, 3};
  CHECK_ERR(error_too_many, stats::HistogramSpec::make(std::move(too_many), small));
  const stats::HistogramSpec default_spec = stats::HistogramSpec::latency_default();
  CHECK_EQ(default_spec.bucket_count(), std::size_t{23});
  CHECK_EQ(default_spec.digest().size(), std::size_t{64});
  CHECK_EQ(default_spec.digest(), stats::HistogramSpec::latency_default().digest());
}

LATOBS_TEST(stats, aggregation_respects_limits) {
  core::Limits limits;
  limits.max_samples_per_path = 3;
  limits.max_quantile_probes = 1;
  std::vector<core::Nanos> values = {1, 2, 3, 4};
  CHECK_ERR(error_samples, stats::aggregate(values, fixed_spec(), fixed_probes(), limits));
  std::vector<core::Nanos> small = {1, 2};
  CHECK_ERR(error_probes, stats::aggregate(small, fixed_spec(), fixed_probes(), limits));
  const std::vector<stats::QuantileProbe> single = {fixed_probes().front()};
  CHECK_OK(ok, stats::aggregate(small, fixed_spec(), single, limits));
  CHECK_EQ(ok.count, std::uint64_t{2});
}

LATOBS_TEST(stats, distribution_round_trips_through_its_codec) {
  std::vector<core::Nanos> values = {10, 20, 20, 30, 400};
  CHECK_OK(distribution, stats::aggregate(values, fixed_spec(), fixed_probes(), core::Limits{}));
  std::string text;
  core::JsonWriter writer(text);
  stats::write_json(writer, distribution);
  CHECK_OK(document, core::parse_json(text, 64));
  CHECK_OK(decoded, stats::decode_distribution(document));
  CHECK_EQ(decoded.count, distribution.count);
  CHECK_EQ(*decoded.mean_ns, *distribution.mean_ns);
  CHECK_EQ(*decoded.sum_ns, *distribution.sum_ns);
  CHECK_EQ(decoded.histogram.buckets.size(), distribution.histogram.buckets.size());
  CHECK_EQ(decoded.probes.size(), distribution.probes.size());
  CHECK_EQ(*decoded.quantiles[1], *distribution.quantiles[1]);
  CHECK_EQ(decoded.histogram.spec.digest(), distribution.histogram.spec.digest());
}

LATOBS_TEST_MAIN()
