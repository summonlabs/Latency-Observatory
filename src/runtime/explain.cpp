// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/runtime/explain.hpp"

#include <algorithm>

namespace latobs::runtime {
namespace {

constexpr std::string_view kNoCausalityNote =
    "no causal claim: the runtime reports observed time and deviations from a generation bound "
    "baseline, never causes";

}  // namespace

bool Explanation::states_no_causality() const {
  return std::find(notes.begin(), notes.end(), std::string(kNoCausalityNote)) != notes.end();
}

std::string coverage_text(std::uint64_t observed, std::uint64_t total) {
  std::string out = std::to_string(observed) + "/" + std::to_string(total);
  if (observed < total) {
    out.append(" (unknown=");
    out.append(std::to_string(total - observed));
    out.push_back(')');
  }
  return out;
}

void write_json(core::JsonWriter& writer, const ProvenanceEntry& entry) {
  writer.begin_object();
  writer.field("measurement", entry.measurement);
  writer.field("source", entry.source);
  writer.field("source_kind", entry.source_kind);
  writer.field("authority", entry.authority);
  writer.field("epoch", entry.epoch);
  writer.field("incarnation", entry.incarnation);
  writer.field("sequence", entry.sequence);
  writer.field("observed_at_ns", entry.observed_at_ns);
  writer.field("observed_at_domain", entry.observed_at_domain);
  writer.field("received_at_ns", entry.received_at_ns);
  writer.field("state", entry.state);
  writer.field("freshness", entry.freshness);
  writer.field("synthetic", entry.synthetic);
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const Explanation& explanation) {
  writer.begin_object();
  writer.field("subject", explanation.subject);
  writer.field("policy_digest", explanation.policy_digest);
  writer.field("histogram_digest", explanation.histogram_digest);
  writer.field("window_from_ns", explanation.window_from_ns);
  writer.field("window_to_ns", explanation.window_to_ns);
  writer.field("as_of_ns", explanation.as_of_ns);
  writer.field("evidence_state", explanation.evidence_state);
  writer.field("freshness", explanation.freshness);
  writer.field("confidence", explanation.confidence);
  writer.field_array("reasons");
  for (const core::Reason& reason : explanation.reasons) {
    writer.begin_object();
    writer.field("code", core::to_string(reason.code));
    writer.field("detail", reason.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.field_array("provenance");
  for (const ProvenanceEntry& entry : explanation.provenance) {
    write_json(writer, entry);
  }
  writer.end_array();
  writer.field("provenance_truncated", explanation.provenance_truncated);
  writer.field_array("notes");
  for (const std::string& note : explanation.notes) writer.value_string(note);
  writer.end_array();
  writer.field("recovery_applied", explanation.recovery_applied);
  writer.field_array("recovery_reasons");
  for (const core::Reason& reason : explanation.recovery_reasons) {
    writer.begin_object();
    writer.field("code", core::to_string(reason.code));
    writer.field("detail", reason.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.field_object("policy");
  writer.field("version", "1");
  writer.end_object();
  writer.end_object();
}

std::string render_text(const Explanation& explanation) {
  std::string out;
  out.append("subject: ");
  out.append(explanation.subject);
  out.push_back('\n');
  out.append("as_of_ns: ");
  out.append(std::to_string(explanation.as_of_ns));
  out.push_back('\n');
  out.append("evidence: ");
  out.append(explanation.evidence_state);
  out.push_back('/');
  out.append(explanation.freshness);
  out.push_back('/');
  out.append(explanation.confidence);
  out.push_back('\n');
  out.append("policy_digest: ");
  out.append(explanation.policy_digest);
  out.push_back('\n');
  out.append("reasons:\n");
  for (const core::Reason& reason : explanation.reasons) {
    out.append("  - ");
    out.append(core::to_string(reason.code));
    if (!reason.detail.empty()) {
      out.append(": ");
      out.append(reason.detail);
    }
    out.push_back('\n');
  }
  out.append("provenance: ");
  out.append(std::to_string(explanation.provenance.size()));
  if (explanation.provenance_truncated) out.append(" (truncated)");
  out.push_back('\n');
  for (const ProvenanceEntry& entry : explanation.provenance) {
    out.append("  - measurement=");
    out.append(entry.measurement);
    out.append(" source=");
    out.append(entry.source);
    out.append(" sequence=");
    out.append(std::to_string(entry.sequence));
    out.append(" observed_at_ns=");
    out.append(std::to_string(entry.observed_at_ns));
    out.append(" received_at_ns=");
    out.append(std::to_string(entry.received_at_ns));
    out.append(" freshness=");
    out.append(entry.freshness);
    out.push_back('\n');
  }
  out.append("notes:\n");
  for (const std::string& note : explanation.notes) {
    out.append("  - ");
    out.append(note);
    out.push_back('\n');
  }
  return out;
}

}  // namespace latobs::runtime
