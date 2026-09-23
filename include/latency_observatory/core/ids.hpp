// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "latency_observatory/core/error.hpp"

namespace latobs::core {

/// A validated label. Names are the human-facing identity of domain objects;
/// the typed numeric identities are derived deterministically from them so that
/// the same canonical name always yields the same identity, across processes
/// and across restarts.
class Name {
 public:
  Name() = default;

  static constexpr std::size_t kMaxLength = 128;
  static Result<Name> parse(std::string_view text);
  /// Precondition: text is already a valid name. Only for internal literals.
  static Name assume_valid(std::string text) { return Name(std::move(text)); }

  [[nodiscard]] const std::string& str() const noexcept { return text_; }
  [[nodiscard]] std::string_view view() const noexcept { return text_; }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] bool operator==(const Name& other) const noexcept { return text_ == other.text_; }
  [[nodiscard]] bool operator<(const Name& other) const noexcept { return text_ < other.text_; }

 private:
  explicit Name(std::string text) : text_(std::move(text)) {}
  std::string text_;
};

/// The content-addressing function used by every typed identity. It is exposed
/// so that identities can be recomputed independently of this translation unit.
[[nodiscard]] std::uint64_t latobs_identity_hash(std::string_view text) noexcept;

/// A strongly typed, content-addressed identity. The zero value is invalid:
/// "no identity" is represented by an invalid id, never by a magic name.
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;

  [[nodiscard]] static Result<StrongId> from_value(std::uint64_t value) {
    if (value == 0) {
      return Error(ErrorCode::InvalidArgument, "identity value must be non-zero");
    }
    return StrongId(value);
  }

  /// Used by decoders after the value has been validated as non-zero.
  [[nodiscard]] static constexpr StrongId from_validated_value(std::uint64_t value) noexcept {
    return StrongId(value);
  }

  /// Deterministic content addressing: identical canonical text yields an
  /// identical identity in every process and every run.
  [[nodiscard]] static StrongId derive_from(std::string_view canonical) noexcept;

  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] static Result<StrongId> parse_hex(std::string_view text);

  [[nodiscard]] constexpr bool operator==(const StrongId& other) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const StrongId& other) const noexcept = default;

 private:
  explicit constexpr StrongId(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

template <class Tag>
std::string StrongId<Tag>::to_hex() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t index = 0; index < 16; ++index) {
    out[15 - index] = kDigits[(value_ >> (index * 4)) & 0x0FULL];
  }
  return out;
}

template <class Tag>
Result<StrongId<Tag>> StrongId<Tag>::parse_hex(std::string_view text) {
  if (text.size() != 16) {
    return Error(ErrorCode::ParseError, "identity hex text must be exactly 16 characters");
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    std::uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint64_t>(c - 'a') + 10ULL;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint64_t>(c - 'A') + 10ULL;
    } else {
      return Error(ErrorCode::ParseError, "identity hex text contains a non-hex character");
    }
    value = (value << 4) | digit;
  }
  // All zero hexadecimal is the canonical text form of "no identity": decoding
  // it yields an invalid identity instead of an error, so that encoding an
  // invalid identity and decoding it again is lossless.
  if (value == 0) return StrongId{};
  return StrongId(value);
}

template <class Tag>
StrongId<Tag> StrongId<Tag>::derive_from(std::string_view canonical) noexcept {
  std::uint64_t hash = latobs_identity_hash(canonical);
  if (hash == 0) hash = 1;  // zero is reserved for "invalid"
  return StrongId(hash);
}

/// Hash adapter for unordered containers keyed by typed identities.
template <class Tag>
struct StrongIdHash {
  [[nodiscard]] std::size_t operator()(const StrongId<Tag>& id) const noexcept {
    return static_cast<std::size_t>(id.value() ^ (id.value() >> 32));
  }
};

// ---------------------------------------------------------------------------
// Identity vocabulary. Every important domain object has its own type: ids of
// different kinds are not interchangeable and cannot be compared by accident.
// ---------------------------------------------------------------------------

struct SourceTag {};
struct PathTag {};
struct HopTag {};
struct LinkTag {};
struct QueueTag {};
struct EndpointTag {};
struct GenerationTag {};
struct ClockDomainTag {};
struct EpochTag {};
struct IncarnationTag {};
struct BaselineTag {};
struct MeasurementTag {};
struct SnapshotTag {};
struct SessionTag {};
struct SubscriptionTag {};

using SourceId = StrongId<SourceTag>;
using PathId = StrongId<PathTag>;
using HopId = StrongId<HopTag>;
using LinkId = StrongId<LinkTag>;
using QueueId = StrongId<QueueTag>;
using EndpointId = StrongId<EndpointTag>;
using GenerationId = StrongId<GenerationTag>;
using ClockDomainId = StrongId<ClockDomainTag>;
using EpochId = StrongId<EpochTag>;
using IncarnationId = StrongId<IncarnationTag>;
using BaselineId = StrongId<BaselineTag>;
using MeasurementId = StrongId<MeasurementTag>;
using SnapshotId = StrongId<SnapshotTag>;
using SessionId = StrongId<SessionTag>;
using SubscriptionId = StrongId<SubscriptionTag>;

// ---------------------------------------------------------------------------
// Small value identities with their own invariants.
// ---------------------------------------------------------------------------

/// Per-source observation sequence number. Monotonic within a
/// (source, epoch, incarnation) triple.
class Sequence {
 public:
  constexpr Sequence() noexcept = default;
  [[nodiscard]] static constexpr Sequence from_value(std::uint64_t value) noexcept {
    return Sequence(value);
  }
  [[nodiscard]] static Result<Sequence> parse(std::string_view text);
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] std::optional<Sequence> successor() const noexcept;
  [[nodiscard]] constexpr bool operator==(const Sequence& other) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Sequence& other) const noexcept = default;

 private:
  explicit constexpr Sequence(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

/// A monotonic revision of a definition. Zero is not a valid revision.
class Revision {
 public:
  constexpr Revision() noexcept = default;
  [[nodiscard]] static Result<Revision> from_value(std::uint32_t value);
  /// Used by decoders after the value has been validated as non-zero.
  [[nodiscard]] static constexpr Revision from_validated_value(std::uint32_t value) noexcept {
    return Revision(value);
  }
  [[nodiscard]] static constexpr Revision first() noexcept { return Revision(1); }
  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
  [[nodiscard]] std::optional<Revision> successor() const noexcept;
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool operator==(const Revision& other) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Revision& other) const noexcept = default;

 private:
  explicit constexpr Revision(std::uint32_t value) noexcept : value_(value) {}
  std::uint32_t value_ = 0;
};

/// Position of a hop inside an observed path. Zero-based and dense.
class HopIndex {
 public:
  static constexpr std::uint32_t kMaxExclusive = 4096;
  constexpr HopIndex() noexcept = default;
  [[nodiscard]] static Result<HopIndex> from_value(std::uint32_t value);
  /// Used by decoders after the value has been validated against the maximum.
  [[nodiscard]] static constexpr HopIndex from_validated_value(std::uint32_t value) noexcept {
    return HopIndex(value);
  }
  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
  [[nodiscard]] std::optional<HopIndex> next() const noexcept;
  [[nodiscard]] constexpr bool operator==(const HopIndex& other) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const HopIndex& other) const noexcept = default;

 private:
  explicit constexpr HopIndex(std::uint32_t value) noexcept : value_(value) {}
  std::uint32_t value_ = 0;
};

}  // namespace latobs::core
