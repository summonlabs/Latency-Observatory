// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/core/ids.hpp"

#include <limits>

#include "latency_observatory/core/checked.hpp"
#include "latency_observatory/core/digest.hpp"

namespace latobs::core {

std::uint64_t latobs_identity_hash(std::string_view text) noexcept { return fnv1a64(text); }

namespace {
[[nodiscard]] bool is_name_start(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] bool is_name_char(char c) noexcept {
  return is_name_start(c) || c == '.' || c == '-' || c == ':' || c == '/' || c == '@' || c == '+';
}
}  // namespace

Result<Name> Name::parse(std::string_view text) {
  if (text.empty()) {
    return Error(ErrorCode::InvalidArgument, "name must not be empty");
  }
  if (text.size() > kMaxLength) {
    return Error(ErrorCode::OutOfRange, "name exceeds the maximum length",
                 std::string(text.substr(0, 32)));
  }
  if (!is_name_start(text.front())) {
    return Error(ErrorCode::InvalidArgument, "name must start with an alphanumeric or underscore",
                 std::string(text));
  }
  for (const char c : text) {
    if (!is_name_char(c)) {
      return Error(ErrorCode::InvalidArgument, "name contains a character outside the allowed set",
                   std::string(text));
    }
  }
  return Name::assume_valid(std::string(text));
}

Result<Sequence> Sequence::parse(std::string_view text) {
  const std::optional<std::uint64_t> value = parse_u64(text);
  if (!value.has_value()) {
    return Error(ErrorCode::ParseError, "sequence is not a valid unsigned integer",
                 std::string(text));
  }
  return Sequence::from_value(*value);
}

std::optional<Sequence> Sequence::successor() const noexcept {
  if (value_ == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
  return Sequence(value_ + 1ULL);
}

Result<Revision> Revision::from_value(std::uint32_t value) {
  if (value == 0) {
    return Error(ErrorCode::InvalidArgument, "revision must be at least 1");
  }
  return Revision(value);
}

std::optional<Revision> Revision::successor() const noexcept {
  if (value_ == std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
  return Revision(value_ + 1U);
}

Result<HopIndex> HopIndex::from_value(std::uint32_t value) {
  if (value >= kMaxExclusive) {
    return Error(ErrorCode::OutOfRange, "hop index exceeds the supported maximum",
                 std::to_string(value));
  }
  return HopIndex(value);
}

std::optional<HopIndex> HopIndex::next() const noexcept {
  if (value_ + 1U >= kMaxExclusive) return std::nullopt;
  return HopIndex(value_ + 1U);
}

}  // namespace latobs::core
