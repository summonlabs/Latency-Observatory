// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "latency_observatory/core/evidence.hpp"
#include "latency_observatory/core/ids.hpp"
#include "latency_observatory/core/time.hpp"
#include "latency_observatory/model/identities.hpp"

namespace latobs::model {

/// When and where a record was observed. The observation time is the time
/// reported by the observing source; the receive time is this runtime's local
/// reference clock reading at the moment the record was accepted. The two are
/// never conflated: freshness is computed from them, and a restart never
/// re-stamps the observation time.
struct ObservationStamp {
  Timestamp observed_at;   // source domain
  Timestamp received_at;   // local reference domain
  MonoTime received_mono;  // local monotonic reading, never persisted

  [[nodiscard]] bool valid() const noexcept { return observed_at.valid() && received_at.valid(); }
};

/// One hop of one exchange. The raw entry/exit readings are always retained;
/// a dwell is present only when the runtime was able to compute it under the
/// comparability policy. An absent dwell is unknown, never zero.
struct HopObservation {
  HopIndex index;
  HopId hop;
  LinkId link;    // invalid when the hop is not link bound
  QueueId queue;  // invalid when the hop has no queue binding
  ClockDomainId entry_domain;
  ClockDomainId exit_domain;
  Timestamp entry;
  Timestamp exit;
  Timestamp queue_entry;  // valid only when the source reported queue residency
  Timestamp queue_exit;
  std::optional<Nanos> dwell_ns;
  std::optional<Nanos> queue_dwell_ns;
  /// Half width of the uncertainty carried by a cross domain dwell. It is zero
  /// for a dwell computed inside a single clock domain.
  std::optional<Nanos> dwell_uncertainty_ns;
  std::optional<Nanos> queue_dwell_uncertainty_ns;
  core::Evidence evidence;

  [[nodiscard]] bool has_dwell() const noexcept { return dwell_ns.has_value(); }
};

/// A complete observed exchange: end to end latency plus whatever per-hop
/// evidence the source reported. Missing hops stay missing.
struct MeasurementRecord {
  MeasurementId id;
  PathId path;
  GenerationId generation;
  SourceId source;
  EpochId epoch;
  IncarnationId incarnation;
  Revision source_revision;
  Sequence sequence;
  ClockDomainId domain;  // domain of request/response
  Timestamp request;
  Timestamp response;
  Nanos rtt_ns = 0;
  std::vector<HopObservation> hops;
  ObservationStamp stamp;
  core::Evidence evidence;
  bool synthetic = false;  // provenance label carried by the recording source

  [[nodiscard]] const HopObservation* hop_at(HopIndex index) const noexcept {
    for (const HopObservation& hop : hops) {
      if (hop.index == index) return &hop;
    }
    return nullptr;
  }
};

/// Identity of a measured exchange. It is derived from the reporting triple and
/// the source sequence number, so a replay of the same sequence produces the
/// same identity and can be recognised as a replay.
[[nodiscard]] MeasurementId derive_measurement_id(SourceId source, EpochId epoch,
                                                  IncarnationId incarnation,
                                                  Sequence sequence) noexcept;

[[nodiscard]] std::string measurement_identity_text(SourceId source, EpochId epoch,
                                                    IncarnationId incarnation, Sequence sequence);

}  // namespace latobs::model
