// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "latency_observatory/core/error.hpp"
#include "latency_observatory/core/ids.hpp"

namespace latobs::core {

/// Nanoseconds. All duration and latency quantities in the runtime are
/// integers: floating point never enters the measurement path.
using Nanos = std::int64_t;

/// A point in time read from a specific clock domain. Two timestamps may only
/// be subtracted when their domains are comparable; the type keeps the domain
/// attached so this can never be forgotten.
struct Timestamp {
  Nanos ns = 0;
  ClockDomainId domain;

  [[nodiscard]] bool valid() const noexcept { return domain.valid(); }
  [[nodiscard]] bool same_domain(const Timestamp& other) const noexcept {
    return domain == other.domain;
  }
  [[nodiscard]] bool operator==(const Timestamp& other) const noexcept {
    return ns == other.ns && domain == other.domain;
  }
  [[nodiscard]] bool operator<(const Timestamp& other) const noexcept {
    return ns < other.ns;
  }
};

/// A monotonic local reading. It is never persisted and never compared across
/// processes; it exists to measure local durations that must not be affected by
/// wall clock steps.
struct MonoTime {
  Nanos ns = 0;

  [[nodiscard]] bool operator==(const MonoTime& other) const noexcept { return ns == other.ns; }
  [[nodiscard]] bool operator<(const MonoTime& other) const noexcept { return ns < other.ns; }
};

/// The local reference clock domain. Every runtime instance reports its own
/// observations in this domain.
[[nodiscard]] ClockDomainId reference_clock_domain() noexcept;
[[nodiscard]] std::string_view reference_clock_domain_name() noexcept;

class Clock {
 public:
  /// Wall clock reading expressed in the local reference clock domain.
  [[nodiscard]] static Timestamp now_reference() noexcept;
  [[nodiscard]] static MonoTime mono_now() noexcept;
  /// Wall clock reading in an explicitly supplied domain (used when decoding
  /// evidence that was observed elsewhere).
  [[nodiscard]] static Timestamp now_in(ClockDomainId domain) noexcept;
};

/// Deterministic UTC rendering: "YYYY-MM-DDTHH:MM:SS.fffffffffZ". The value is
/// interpreted as nanoseconds since the Unix epoch, independent of the host
/// time zone (the runtime never consults local time).
[[nodiscard]] std::string format_utc(Nanos unix_ns);
[[nodiscard]] Result<Nanos> parse_utc(std::string_view text);

/// Duration between two readings of the same clock domain. The caller must
/// have established comparability; identical domains are always comparable for
/// differences.
[[nodiscard]] inline Nanos duration_between(const Timestamp& earlier,
                                            const Timestamp& later) noexcept {
  return later.ns - earlier.ns;
}

}  // namespace latobs::core
