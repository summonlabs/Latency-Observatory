// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <limits>
#include <string>
#include <vector>

#include "latency_observatory/core/checked.hpp"
#include "latency_observatory/core/digest.hpp"
#include "latency_observatory/core/evidence.hpp"
#include "latency_observatory/core/ids.hpp"
#include "latency_observatory/core/json.hpp"
#include "latency_observatory/core/policy.hpp"
#include "latency_observatory/core/time.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

LATOBS_TEST(core, name_validation) {
  CHECK_OK(name, Name::parse("alpha.beta-1_2"));
  CHECK_EQ(name.view(), std::string_view("alpha.beta-1_2"));
  CHECK_ERR(error, Name::parse(""));
  CHECK_ERR(error_dash, Name::parse("-leading"));
  CHECK_ERR(error_space, Name::parse("has space"));
  CHECK_ERR(error_long, Name::parse(std::string(200, 'a')));
}

LATOBS_TEST(core, identity_is_content_addressed_and_stable) {
  const SourceId first = SourceId::derive_from("source.alpha");
  const SourceId second = SourceId::derive_from("source.alpha");
  const SourceId other = SourceId::derive_from("source.beta");
  CHECK(first == second);
  CHECK(first != other);
  CHECK(first.valid());
  CHECK_OK(parsed, SourceId::parse_hex(first.to_hex()));
  CHECK(parsed == first);
  CHECK_EQ(first.to_hex().size(), std::size_t{16});
  CHECK_ERR(error_short, SourceId::parse_hex("short"));
  CHECK_ERR(error_hex, SourceId::parse_hex("zzzzzzzzzzzzzzzz"));
  CHECK_ERR(error_zero, SourceId::from_value(0));
  CHECK(!SourceId{}.valid());
  // The canonical text form of "no identity" round trips to an invalid identity.
  CHECK_EQ(SourceId{}.to_hex(), std::string("0000000000000000"));
  CHECK_OK(invalid, SourceId::parse_hex("0000000000000000"));
  CHECK(!invalid.valid());
  // Typed identities of different kinds are distinct types; distinct name
  // spaces produce independent values from the same text.
  const PathId path = PathId::derive_from("source.alpha");
  CHECK_EQ(path.value(), first.value());
}

LATOBS_TEST(core, checked_arithmetic_refuses_to_wrap) {
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  const std::int64_t min = std::numeric_limits<std::int64_t>::min();
  CHECK(!core::checked_add_i64(max, 1).has_value());
  CHECK(!core::checked_add_i64(min, -1).has_value());
  CHECK(!core::checked_sub_i64(min, 1).has_value());
  CHECK(!core::checked_mul_i64(max, 2).has_value());
  CHECK_EQ(*core::checked_mul_i64(min, 1), min);
  CHECK(!core::checked_mul_i64(min, -1).has_value());
  CHECK_EQ(*core::checked_add_i64(2, 3), 5);
  CHECK(!core::checked_mul_u64(std::numeric_limits<std::uint64_t>::max(), 2).has_value());
  CHECK_EQ(*core::checked_mul_u64(1ULL << 32, 1ULL << 31), 1ULL << 63);
  CHECK(!core::checked_add_size(std::numeric_limits<std::size_t>::max(), 1).has_value());
  CHECK(!core::checked_mul_size(std::numeric_limits<std::size_t>::max(), 2).has_value());
  CHECK(!core::parse_i64("").has_value());
  CHECK(!core::parse_i64("12x").has_value());
  CHECK(!core::parse_i64("99999999999999999999").has_value());
  CHECK_EQ(*core::parse_i64("-9223372036854775808"), min);
  CHECK(!core::parse_u64("-1").has_value());
  CHECK_EQ(core::isqrt_u64(0), 0ULL);
  CHECK_EQ(core::isqrt_u64(1), 1ULL);
  CHECK_EQ(core::isqrt_u64(15), 3ULL);
  CHECK_EQ(core::isqrt_u64(16), 4ULL);
  CHECK_EQ(core::isqrt_u64(1ULL << 62), 1ULL << 31);
}

LATOBS_TEST(core, uint128_arithmetic) {
  const core::UInt128 product = core::mul_u64_to_u128(0xFFFFFFFFULL, 0xFFFFFFFFULL);
  CHECK_EQ(product.hi, 0ULL);
  CHECK_EQ(product.lo, 0xFFFFFFFE00000001ULL);
  std::uint64_t remainder = 0;
  const core::UInt128 quotient = core::divmod_u128_u64(product, 0xFFFFFFFFULL, remainder);
  CHECK_EQ(quotient.lo, 0xFFFFFFFFULL);
  CHECK_EQ(quotient.hi, 0ULL);
  CHECK_EQ(remainder, 0ULL);
  const core::UInt128 big = core::mul_u64_to_u128(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
  CHECK_EQ(big.to_string(), std::string("340282366920938463426481119284349108225"));
  std::uint64_t big_remainder = 0;
  const core::UInt128 divided = core::divmod_u128_u64(big, 0xFFFFFFFFFFFFFFFFULL, big_remainder);
  CHECK_EQ(divided.lo, 0xFFFFFFFFFFFFFFFFULL);
  CHECK_EQ(divided.hi, 0ULL);
  CHECK_EQ(big_remainder, 0ULL);
  CHECK(core::u128_less(core::UInt128::from_u64(1), core::UInt128{1, 0}));
  const core::UInt128 scaled = core::mul_u128_u64(core::UInt128::from_u64(1000000), 1000000ULL);
  CHECK_EQ(scaled.to_string(), std::string("1000000000000"));
}

LATOBS_TEST(core, digest_known_vectors) {
  CHECK_EQ(core::sha256_hex(""),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(core::sha256_hex("abc"),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(core::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // One million 'a' bytes, streamed in ten byte updates.
  core::Sha256 streaming;
  for (int index = 0; index < 100000; ++index) {
    streaming.update("aaaaaaaaaa", 10);
  }
  CHECK_EQ(streaming.hex(),
           std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  CHECK_EQ(core::crc32c("123456789"), 0xE3069283u);
  CHECK_EQ(core::crc32c(""), 0u);
  CHECK_EQ(core::fnv1a64(""), 0xcbf29ce484222325ULL);
  CHECK_EQ(core::fnv1a64("a"), 0xaf63dc4c8601ec8cULL);
}

LATOBS_TEST(core, time_rendering_is_utc_and_exact) {
  CHECK_EQ(core::format_utc(0), std::string("1970-01-01T00:00:00.000000000Z"));
  CHECK_EQ(core::format_utc(-1), std::string("1969-12-31T23:59:59.999999999Z"));
  CHECK_EQ(core::format_utc(951782400000000000LL), std::string("2000-02-29T00:00:00.000000000Z"));
  CHECK_OK(parsed, core::parse_utc("2000-02-29T00:00:00.000000000Z"));
  CHECK_EQ(parsed, 951782400000000000LL);
  CHECK_OK(parsed_negative, core::parse_utc("1969-12-31T23:59:59.999999999Z"));
  CHECK_EQ(parsed_negative, -1LL);
  CHECK_OK(parsed_short, core::parse_utc("2026-01-02T03:04:05Z"));
  CHECK_EQ(core::format_utc(parsed_short), std::string("2026-01-02T03:04:05.000000000Z"));
  CHECK_ERR(error_missing_z, core::parse_utc("2000-02-29T00:00:00"));
  CHECK_ERR(error_month, core::parse_utc("2000-13-29T00:00:00Z"));
  CHECK_ERR(error_text, core::parse_utc("not a time"));
}

LATOBS_TEST(core, json_writer_is_canonical_and_balanced) {
  std::string text;
  core::JsonWriter writer(text);
  writer.begin_object();
  writer.field("z", static_cast<std::int64_t>(1));
  writer.field("a", "text");
  writer.field_null("missing");
  writer.field_array("values");
  writer.value_int(1);
  writer.value_int(2);
  writer.end_array();
  writer.end_object();
  CHECK(writer.balanced());
  CHECK_EQ(text, std::string("{\"z\":1,\"a\":\"text\",\"missing\":null,\"values\":[1,2]}"));

  std::string escaped;
  core::JsonWriter escape_writer(escaped);
  escape_writer.value_string("a\"b\\c\nd\te");
  CHECK_EQ(escaped, std::string("\"a\\\"b\\\\c\\nd\\te\""));
}

LATOBS_TEST(core, json_parser_is_strict) {
  CHECK_OK(document, core::parse_json(R"({"a":1,"b":[true,false,null,"x"]})", 8));
  CHECK(document.is_object());
  CHECK_EQ(*core::json_require_int(document, "a"), 1);
  CHECK_OK(array_value, core::json_require_array(document, "b"));
  CHECK_OK(entries, array_value->as_array());
  CHECK_EQ(entries->size(), std::size_t{4});
  CHECK(document.find("missing") == nullptr);

  CHECK_ERR(error_missing, core::json_require_int(document, "missing"));
  CHECK_ERR(error_duplicate, core::parse_json(R"({"a":1,"a":2})", 8));
  CHECK_ERR(error_fraction, core::parse_json(R"({"a":1.5})", 8));
  CHECK_ERR(error_exponent, core::parse_json(R"({"a":1e3})", 8));
  CHECK_ERR(error_trailing, core::parse_json(R"({"a":1} trailing)", 8));
  CHECK_ERR(error_empty, core::parse_json("", 8));
  CHECK_ERR(error_bracket, core::parse_json("[]]", 8));

  std::string deep = "[";
  for (int index = 0; index < 20; ++index) deep.push_back('[');
  for (int index = 0; index < 21; ++index) deep.push_back(']');
  CHECK_ERR(error_deep, core::parse_json(deep, 8));
  CHECK_OK(deep_ok, core::parse_json(deep, 32));

  CHECK_OK(escapes, core::parse_json(R"({"s":"\u0041\u00e9\ud83d\ude00"})", 8));
  CHECK_OK(text, core::json_require_string(escapes, "s"));
  CHECK_EQ(text.size(), std::size_t{7});
  CHECK_EQ(text, std::string("A\u00e9\U0001F600"));
  CHECK_ERR(error_high_surrogate, core::parse_json(R"({"s":"\ud83d"})", 8));
  CHECK_ERR(error_low_surrogate, core::parse_json(R"({"s":"\udc00"})", 8));
  CHECK_ERR(error_unterminated, core::parse_json(R"({"s":"unterminated)", 8));
  CHECK_ERR(error_control, core::parse_json("{\"s\":\"a\u0001b\"}", 8));
}

LATOBS_TEST(core, evidence_distinguishes_states) {
  const core::Evidence observed = core::Evidence::observed_fresh();
  CHECK(observed.usable());
  CHECK_EQ(std::string(core::to_string(observed.state())), std::string("observed"));

  const core::Evidence missing = core::Evidence::missing(core::ReasonCode::HopMissing, "h");
  CHECK(!missing.usable());
  CHECK(missing.state() == core::EvidenceState::Missing);
  CHECK(!missing.has_value());

  const core::Evidence unsupported =
      core::Evidence::unsupported(core::ReasonCode::SemanticsUnsupported, "s");
  const core::Evidence refused = core::Evidence::refused(core::ReasonCode::IncomparableClocks, "c");
  CHECK(unsupported.state() == core::EvidenceState::Unsupported);
  CHECK(refused.state() == core::EvidenceState::Refused);
  CHECK_NE(std::string(core::to_string(unsupported.state())),
           std::string(core::to_string(refused.state())));

  const core::Evidence stale = core::Evidence::observed(core::Freshness::Stale, core::Confidence::None);
  CHECK(stale.has_value());
  CHECK(!stale.usable());

  const core::Evidence merged = core::Evidence::merge(observed, missing);
  CHECK(merged.state() == core::EvidenceState::Incomplete);
  const core::Evidence conflicting =
      core::Evidence::merge(observed, core::Evidence::conflicting(core::ReasonCode::SequenceReplay));
  CHECK(conflicting.state() == core::EvidenceState::Conflicting);
  const core::Evidence refused_merge = core::Evidence::merge(observed, refused);
  CHECK(refused_merge.state() == core::EvidenceState::Refused);
  CHECK(core::Evidence::merge(observed, missing) == core::Evidence::merge(missing, observed));
}

LATOBS_TEST(core, evidence_reasons_are_bounded_and_deduplicated) {
  core::Evidence evidence = core::Evidence::observed_fresh();
  evidence.add_reason(core::ReasonCode::HopMissing, "a");
  evidence.add_reason(core::ReasonCode::HopMissing, "a");
  CHECK_EQ(evidence.reasons().size(), std::size_t{2});
  for (std::size_t index = 0; index < core::Evidence::kMaxReasons + 8; ++index) {
    evidence.add_reason(core::ReasonCode::HopGap, std::to_string(index));
  }
  CHECK(evidence.reasons().size() <= core::Evidence::kMaxReasons);
  CHECK(evidence.reasons_truncated());
}

LATOBS_TEST(core, policy_validation_and_digest) {
  core::RuntimePolicy policy = core::default_policy();
  CHECK(policy.validate().ok());
  const std::string digest = policy.digest();
  CHECK_EQ(digest.size(), std::size_t{64});
  core::RuntimePolicy changed = policy;
  changed.attribution.residual_tolerance_ns += 1;
  CHECK_NE(changed.digest(), digest);

  core::RuntimePolicy invalid = policy;
  invalid.freshness.aging_horizon_ns = invalid.freshness.fresh_horizon_ns;
  CHECK(!invalid.validate().ok());
  core::RuntimePolicy invalid_limits = policy;
  invalid_limits.limits.max_samples_per_path = 1;
  invalid_limits.limits.max_batch_samples = 10;
  CHECK(!invalid_limits.validate().ok());
  core::RuntimePolicy no_transitive = policy;
  no_transitive.comparability.allow_transitive = true;
  CHECK(!no_transitive.validate().ok());

  CHECK(policy.freshness.classify(-1) == core::Freshness::Unknown);
  CHECK(policy.freshness.classify(0) == core::Freshness::Fresh);
  CHECK(policy.freshness.classify(policy.freshness.stale_horizon_ns + 1) == core::Freshness::Expired);
  CHECK(policy.freshness.classify(policy.freshness.aging_horizon_ns) == core::Freshness::Aging);
}

LATOBS_TEST_MAIN()
