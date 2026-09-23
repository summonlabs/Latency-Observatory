// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace latobs::core {

// ---------------------------------------------------------------------------
// Checked arithmetic. Externally derived sizes never wrap: every operation
// that can overflow returns an empty optional and the caller must decide.
// ---------------------------------------------------------------------------

[[nodiscard]] bool add_overflow_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept;
[[nodiscard]] bool sub_overflow_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept;
[[nodiscard]] bool mul_overflow_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept;

[[nodiscard]] std::optional<std::int64_t> checked_add_i64(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] std::optional<std::int64_t> checked_sub_i64(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] std::optional<std::int64_t> checked_mul_i64(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] std::optional<std::uint64_t> checked_add_u64(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] std::optional<std::uint64_t> checked_mul_u64(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] std::optional<std::size_t> checked_add_size(std::size_t a, std::size_t b) noexcept;
[[nodiscard]] std::optional<std::size_t> checked_mul_size(std::size_t a, std::size_t b) noexcept;

/// Strict textual parsing: the whole input must be consumed and the value must
/// fit the destination type.
[[nodiscard]] std::optional<std::int64_t> parse_i64(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept;

/// Deterministic integer square root (floor).
[[nodiscard]] std::uint64_t isqrt_u64(std::uint64_t value) noexcept;

/// Minimal unsigned 128-bit accumulator used for latency sums and exact
/// rational arithmetic. Not a general big integer: only what the aggregator
/// needs, with every operation checked.
struct UInt128 {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;

  [[nodiscard]] static UInt128 from_u64(std::uint64_t value) noexcept { return UInt128{0, value}; }
  [[nodiscard]] bool is_zero() const noexcept { return hi == 0 && lo == 0; }
  [[nodiscard]] bool fits_u64() const noexcept { return hi == 0; }
  [[nodiscard]] bool exceeds_i64_max() const noexcept {
    return hi > 0 || lo > 0x7FFFFFFFFFFFFFFFULL;
  }
  [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] UInt128 add_u128(UInt128 a, UInt128 b) noexcept;   // saturating on 2^128 wrap
[[nodiscard]] UInt128 mul_u128_u64(UInt128 a, std::uint64_t b) noexcept;  // saturating
[[nodiscard]] UInt128 mul_u64_to_u128(std::uint64_t a, std::uint64_t b) noexcept;
/// Precondition: a >= b.
[[nodiscard]] UInt128 sub_u128(UInt128 a, UInt128 b) noexcept;
[[nodiscard]] bool u128_equal(UInt128 a, UInt128 b) noexcept;
/// Divides by a non-zero 64-bit divisor; returns quotient and sets remainder.
[[nodiscard]] UInt128 divmod_u128_u64(UInt128 value, std::uint64_t divisor, std::uint64_t& remainder) noexcept;
[[nodiscard]] bool u128_less(UInt128 a, UInt128 b) noexcept;

/// Saturating sentinel used when an accumulator would exceed 2^128-1.
[[nodiscard]] bool is_saturated(UInt128 value) noexcept;

}  // namespace latobs::core
