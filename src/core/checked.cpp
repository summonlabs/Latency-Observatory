// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/core/checked.hpp"

#include <limits>

namespace latobs::core {
namespace {
constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kI64Max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::uint64_t kI64MinAbs =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL;
constexpr UInt128 kSaturated{kU64Max, kU64Max};

[[nodiscard]] std::uint64_t bit_at(UInt128 value, int bit) noexcept {
  if (bit >= 64) return (value.hi >> (bit - 64)) & 1ULL;
  return (value.lo >> bit) & 1ULL;
}
}  // namespace

bool add_overflow_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if ((b > 0) && (a > std::numeric_limits<std::int64_t>::max() - b)) return true;
  if ((b < 0) && (a < std::numeric_limits<std::int64_t>::min() - b)) return true;
  out = a + b;
  return false;
}

bool sub_overflow_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if ((b < 0) && (a > std::numeric_limits<std::int64_t>::max() + b)) return true;
  if ((b > 0) && (a < std::numeric_limits<std::int64_t>::min() + b)) return true;
  out = a - b;
  return false;
}

bool mul_overflow_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return false;
  }
  const bool negative = (a < 0) != (b < 0);
  const std::uint64_t ua = (a < 0) ? (0ULL - static_cast<std::uint64_t>(a)) : static_cast<std::uint64_t>(a);
  const std::uint64_t ub = (b < 0) ? (0ULL - static_cast<std::uint64_t>(b)) : static_cast<std::uint64_t>(b);
  if (ua > kU64Max / ub) return true;
  const std::uint64_t product = ua * ub;
  if (negative) {
    if (product > kI64MinAbs) return true;
    out = (product == kI64MinAbs) ? std::numeric_limits<std::int64_t>::min()
                                  : -static_cast<std::int64_t>(product);
    return false;
  }
  if (product > kI64Max) return true;
  out = static_cast<std::int64_t>(product);
  return false;
}

std::optional<std::int64_t> checked_add_i64(std::int64_t a, std::int64_t b) noexcept {
  std::int64_t out = 0;
  if (add_overflow_i64(a, b, out)) return std::nullopt;
  return out;
}

std::optional<std::int64_t> checked_sub_i64(std::int64_t a, std::int64_t b) noexcept {
  std::int64_t out = 0;
  if (sub_overflow_i64(a, b, out)) return std::nullopt;
  return out;
}

std::optional<std::int64_t> checked_mul_i64(std::int64_t a, std::int64_t b) noexcept {
  std::int64_t out = 0;
  if (mul_overflow_i64(a, b, out)) return std::nullopt;
  return out;
}

std::optional<std::uint64_t> checked_add_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (a > kU64Max - b) return std::nullopt;
  return a + b;
}

std::optional<std::uint64_t> checked_mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (a == 0 || b == 0) return 0ULL;
  if (a > kU64Max / b) return std::nullopt;
  return a * b;
}

std::optional<std::size_t> checked_add_size(std::size_t a, std::size_t b) noexcept {
  if (a > std::numeric_limits<std::size_t>::max() - b) return std::nullopt;
  return a + b;
}

std::optional<std::size_t> checked_mul_size(std::size_t a, std::size_t b) noexcept {
  if (a == 0 || b == 0) return std::size_t{0};
  if (a > std::numeric_limits<std::size_t>::max() / b) return std::nullopt;
  return a * b;
}

std::optional<std::int64_t> parse_i64(std::string_view text) noexcept {
  if (text.empty()) return std::nullopt;
  std::size_t index = 0;
  bool negative = false;
  if (text[0] == '+' || text[0] == '-') {
    negative = text[0] == '-';
    index = 1;
    if (text.size() == 1) return std::nullopt;
  }
  std::uint64_t magnitude = 0;
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (c < '0' || c > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (magnitude > (kU64Max - digit) / 10ULL) return std::nullopt;
    magnitude = magnitude * 10ULL + digit;
  }
  if (negative) {
    if (magnitude > kI64MinAbs) return std::nullopt;
    if (magnitude == kI64MinAbs) return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(magnitude);
  }
  if (magnitude > kI64Max) return std::nullopt;
  return static_cast<std::int64_t>(magnitude);
}

std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept {
  if (text.empty()) return std::nullopt;
  std::size_t index = 0;
  if (text[0] == '+') {
    index = 1;
    if (text.size() == 1) return std::nullopt;
  }
  std::uint64_t magnitude = 0;
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (c < '0' || c > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (magnitude > (kU64Max - digit) / 10ULL) return std::nullopt;
    magnitude = magnitude * 10ULL + digit;
  }
  return magnitude;
}

std::uint64_t isqrt_u64(std::uint64_t value) noexcept {
  std::uint64_t result = 0;
  std::uint64_t bit = 1ULL << 62;
  std::uint64_t remainder = value;
  while (bit > remainder) bit >>= 2;
  while (bit != 0) {
    if (remainder >= result + bit) {
      remainder -= result + bit;
      result = (result >> 1) + bit;
    } else {
      result >>= 1;
    }
    bit >>= 2;
  }
  return result;
}

std::string UInt128::to_string() const {
  if (is_zero()) return "0";
  std::string digits;
  UInt128 current = *this;
  while (!current.is_zero()) {
    std::uint64_t remainder = 0;
    current = divmod_u128_u64(current, 10ULL, remainder);
    digits.push_back(static_cast<char>('0' + remainder));
  }
  return std::string(digits.rbegin(), digits.rend());
}

UInt128 add_u128(UInt128 a, UInt128 b) noexcept {
  const std::uint64_t lo = a.lo + b.lo;
  const std::uint64_t carry = (lo < a.lo) ? 1ULL : 0ULL;
  if (a.hi > kU64Max - b.hi) return kSaturated;
  const std::uint64_t hi = a.hi + b.hi;
  if (hi > kU64Max - carry) return kSaturated;
  return UInt128{hi + carry, lo};
}

UInt128 mul_u64_to_u128(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a_lo = a & 0xFFFFFFFFULL;
  const std::uint64_t a_hi = a >> 32;
  const std::uint64_t b_lo = b & 0xFFFFFFFFULL;
  const std::uint64_t b_hi = b >> 32;

  const std::uint64_t p0 = a_lo * b_lo;
  const std::uint64_t p1 = a_lo * b_hi;
  const std::uint64_t p2 = a_hi * b_lo;
  const std::uint64_t p3 = a_hi * b_hi;

  const std::uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
  const std::uint64_t lo = (p0 & 0xFFFFFFFFULL) | (mid << 32);
  const std::uint64_t hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
  return UInt128{hi, lo};
}

UInt128 mul_u128_u64(UInt128 a, std::uint64_t b) noexcept {
  if (a.is_zero() || b == 0) return UInt128{0, 0};
  const UInt128 low = mul_u64_to_u128(a.lo, b);
  const UInt128 high = mul_u64_to_u128(a.hi, b);
  if (high.hi != 0) return kSaturated;
  const std::uint64_t hi = high.lo + low.hi;
  if (hi < high.lo) return kSaturated;
  return UInt128{hi, low.lo};
}

UInt128 divmod_u128_u64(UInt128 value, std::uint64_t divisor, std::uint64_t& remainder) noexcept {
  if (divisor == 0) {
    remainder = 0;
    return kSaturated;
  }
  UInt128 quotient{0, 0};
  UInt128 rem{0, 0};
  for (int bit = 127; bit >= 0; --bit) {
    rem.hi = (rem.hi << 1) | (rem.lo >> 63);
    rem.lo = (rem.lo << 1) | bit_at(value, bit);
    quotient.hi = (quotient.hi << 1) | (quotient.lo >> 63);
    quotient.lo <<= 1;
    if (rem.hi != 0 || rem.lo >= divisor) {
      const std::uint64_t borrow = (rem.lo < divisor) ? 1ULL : 0ULL;
      rem.lo -= divisor;
      rem.hi -= borrow;
      quotient.lo |= 1ULL;
    }
  }
  remainder = rem.lo;
  return quotient;
}

UInt128 sub_u128(UInt128 a, UInt128 b) noexcept {
  const std::uint64_t borrow = (a.lo < b.lo) ? 1ULL : 0ULL;
  return UInt128{a.hi - b.hi - borrow, a.lo - b.lo};
}

bool u128_equal(UInt128 a, UInt128 b) noexcept { return a.hi == b.hi && a.lo == b.lo; }

bool u128_less(UInt128 a, UInt128 b) noexcept {
  if (a.hi != b.hi) return a.hi < b.hi;
  return a.lo < b.lo;
}

bool is_saturated(UInt128 value) noexcept { return value.hi == kU64Max && value.lo == kU64Max; }

}  // namespace latobs::core
