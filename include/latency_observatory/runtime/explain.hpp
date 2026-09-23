// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "latency_observatory/core/evidence.hpp"
#include "latency_observatory/core/json.hpp"
#include "latency_observatory/runtime/vocabulary.hpp"

namespace latobs::runtime {

/// One contributing measurement, with the identity of the source that produced
/// it and the times at which it was observed and received. Explanations never
/// refer to evidence without this provenance.
struct ProvenanceEntry {
  std::string measurement;  // identity in hex
  std::string source;
  std::string epoch;
  std::string incarnation;
  std::uint64_t sequence = 0;
  std::string source_kind;
  std::string authority;
  Nanos observed_at_ns = 0;
  std::string observed_at_domain;
  Nanos received_at_ns = 0;
  std::string state;
  std::string freshness;
  bool synthetic = false;
};

/// A deterministic explanation. The same evidence, policy and request always
/// produce the same explanation, byte for byte.
struct Explanation {
  std::string subject;
  std::string policy_digest;
  std::string policy_text;
  std::string histogram_digest;
  std::uint64_t window_from_ns = 0;
  std::uint64_t window_to_ns = 0;
  Nanos as_of_ns = 0;
  std::string evidence_state;
  std::string freshness;
  std::string confidence;
  std::vector<core::Reason> reasons;
  std::vector<ProvenanceEntry> provenance;
  std::vector<std::string> notes;
  bool provenance_truncated = false;
  bool recovery_applied = false;
  std::vector<core::Reason> recovery_reasons;

  /// True when the note list states that no causal claim is made.
  [[nodiscard]] bool states_no_causality() const;
};

void write_json(core::JsonWriter& writer, const ProvenanceEntry& entry);
void write_json(core::JsonWriter& writer, const Explanation& explanation);

/// Deterministic plain text rendering used by the command line tool.
[[nodiscard]] std::string render_text(const Explanation& explanation);

/// Renders a distributed position: observed/total with the unknown part called
/// out explicitly, so a partial window is never presented as complete.
[[nodiscard]] std::string coverage_text(std::uint64_t observed, std::uint64_t total);

}  // namespace latobs::runtime
