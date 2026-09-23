// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/core/error.hpp"
#include "latency_observatory/model/identities.hpp"
#include "latency_observatory/core/evidence.hpp"
#include "latency_observatory/core/ids.hpp"
#include "latency_observatory/core/policy.hpp"
#include "latency_observatory/core/time.hpp"
#include "latency_observatory/model/entities.hpp"

namespace latobs::model {

/// Synchronization state of a clock domain, as reported with its sync record.
enum class ClockSyncState : std::uint8_t { Unknown, Unsynced, Synchronized, Holdover };
std::string_view to_string(ClockSyncState state) noexcept;
bool parse_clock_sync_state(std::string_view text, ClockSyncState& out) noexcept;

struct ClockDomainDef {
  ClockDomainId id;
  Name name;
  bool is_reference = false;
  std::string description;

  [[nodiscard]] bool operator==(const ClockDomainDef& other) const noexcept {
    return id == other.id && name == other.name && is_reference == other.is_reference &&
           description == other.description;
  }
};

/// A clock synchronization record. Offset, skew and uncertainty are claims made
/// by a reporting source; the runtime stores them as claims with provenance and
/// never invents a value that was not reported.
struct ClockSync {
  ClockDomainId domain;
  ClockDomainId reference;
  ClockSyncState state = ClockSyncState::Unknown;
  Nanos offset_ns = 0;       // domain reading = reference reading + offset
  Nanos skew_ppb = 0;
  Nanos uncertainty_ns = 0;  // half width of the reported uncertainty
  Nanos valid_for_ns = 0;    // how long the report remains usable
  Timestamp observed_at;     // when the report was produced, in the reference domain
  SourceId reported_by;
  GenerationId generation;
  EpochId epoch;
  IncarnationId incarnation;
  Revision revision;

  [[nodiscard]] Nanos age_ns(const Timestamp& now) const noexcept {
    return now.ns - observed_at.ns;
  }

  [[nodiscard]] bool operator==(const ClockSync& other) const noexcept {
    return domain == other.domain && reference == other.reference && state == other.state &&
           offset_ns == other.offset_ns && skew_ppb == other.skew_ppb &&
           uncertainty_ns == other.uncertainty_ns && valid_for_ns == other.valid_for_ns &&
           observed_at == other.observed_at && reported_by == other.reported_by &&
           generation == other.generation && epoch == other.epoch &&
           incarnation == other.incarnation && revision == other.revision;
  }
};

/// The outcome of a comparability decision. When comparable is false, the
/// uncertainty is meaningless and no derived value may be produced.
struct ClockComparability {
  bool comparable = false;
  Nanos uncertainty_ns = 0;
  /// Offsets that bring each argument's readings onto the reference timeline:
  /// reference_reading = domain_reading - offset_ns.
  Nanos offset_first_ns = 0;
  Nanos offset_second_ns = 0;
  core::Confidence confidence = core::Confidence::None;
  core::Evidence evidence;
};

/// The clock registry owns every comparability decision. Two readings are only
/// comparable when both domains are established through the reference domain
/// with a fresh, generation matched synchronization record.
class ClockRegistry {
 public:
  explicit ClockRegistry(const core::RuntimePolicy& policy) : policy_(policy) {}

  Result<ClockDomainId> define_domain(ClockDomainDef definition);
  [[nodiscard]] core::Status record_sync(ClockSync sync);

  [[nodiscard]] const ClockDomainDef* domain(ClockDomainId id) const noexcept;
  [[nodiscard]] const ClockSync* sync(ClockDomainId id) const noexcept;
  [[nodiscard]] std::size_t domain_count() const noexcept { return domains_.size(); }
  [[nodiscard]] std::vector<const ClockDomainDef*> domains() const;
  [[nodiscard]] std::vector<const ClockSync*> syncs() const;

  /// Decides whether readings from \p a and \p b may be compared at \p now.
  /// \p generation, \p epoch and \p incarnation describe the evidence being
  /// compared and must match the synchronization records when the policy
  /// requires it.
  [[nodiscard]] Result<ClockComparability> compare(ClockDomainId a, ClockDomainId b,
                                                   const Timestamp& now, GenerationId generation,
                                                   EpochId epoch,
                                                   IncarnationId incarnation) const;

  /// Age of a reading relative to a local reference reading. An age is only
  /// produced when the reading's domain can be related to the reference domain;
  /// otherwise the age is unknown and the evidence says why.
  struct AgeEstimate {
    std::optional<Nanos> age_ns;
    core::Evidence evidence;
  };
  [[nodiscard]] AgeEstimate estimate_age(const Timestamp& observed,
                                         const Timestamp& now_reference) const;

  [[nodiscard]] const core::RuntimePolicy& policy() const noexcept { return policy_; }

 private:
  [[nodiscard]] Result<ClockComparability> compare_single(ClockDomainId id, const Timestamp& now,
                                                          GenerationId generation, EpochId epoch,
                                                          IncarnationId incarnation) const;

  core::RuntimePolicy policy_;
  std::map<ClockDomainId, ClockDomainDef> domains_;
  std::map<ClockDomainId, ClockSync> syncs_;
};

}  // namespace latobs::model
