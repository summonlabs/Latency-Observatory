// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/core/error.hpp"
#include "latency_observatory/model/identities.hpp"
#include "latency_observatory/core/ids.hpp"
#include "latency_observatory/core/policy.hpp"

namespace latobs::model {

/// How a source describes itself. These labels are declarations made by the
/// operator of the source; the runtime records them and never upgrades them.
enum class SourceKind : std::uint8_t { Synthetic, Probe, TelemetryAgent, DerivedAggregate, Unknown };
std::string_view to_string(SourceKind kind) noexcept;
bool parse_source_kind(std::string_view text, SourceKind& out) noexcept;

/// Where a source sits in the authority order. Authority decides which source
/// wins when two sources describe the same observation differently.
enum class AuthorityClass : std::uint8_t { Primary, Secondary, Derived, Synthetic, Unknown };
std::string_view to_string(AuthorityClass authority) noexcept;
bool parse_authority_class(std::string_view text, AuthorityClass& out) noexcept;

/// What a hop is, as declared by the source that reports it. These are
/// semantic labels attached to observations; the runtime makes no claim about
/// the hardware that implements a hop.
enum class HopKind : std::uint8_t {
  Endpoint,
  Link,
  ForwardingStage,
  QueueingStage,
  Application,
  Unknown,
};
std::string_view to_string(HopKind kind) noexcept;
bool parse_hop_kind(std::string_view text, HopKind& out) noexcept;

enum class EndpointRole : std::uint8_t { Client, Server, Intermediate, Unknown };
std::string_view to_string(EndpointRole role) noexcept;
bool parse_endpoint_role(std::string_view text, EndpointRole& out) noexcept;

/// The measurement semantics a source declares. A value is only computed when
/// the semantics of the contributing sources permit it.
enum class SemanticsProfile : std::uint8_t {
  Unknown = 0,
  EndToEndRequestResponse = 1u << 0u,
  HopDwell = 1u << 1u,
  QueueDwell = 1u << 2u,
};
/// Renders a (possibly combined) semantics mask as a '|' separated list, or
/// "unknown" when no semantics are declared.
[[nodiscard]] std::string to_string(SemanticsProfile profile);
bool parse_semantics_profile(std::string_view text, SemanticsProfile& out) noexcept;
bool parse_semantics_list(std::string_view text, SemanticsProfile& out) noexcept;
[[nodiscard]] bool has_semantics(SemanticsProfile value, SemanticsProfile flag) noexcept;
[[nodiscard]] SemanticsProfile operator|(SemanticsProfile a, SemanticsProfile b) noexcept;
[[nodiscard]] SemanticsProfile operator&(SemanticsProfile a, SemanticsProfile b) noexcept;

struct SourceDescriptor {
  SourceId id;
  Name name;
  SourceKind kind = SourceKind::Unknown;
  AuthorityClass authority = AuthorityClass::Unknown;
  SemanticsProfile semantics = SemanticsProfile::Unknown;
  Revision revision;
  std::string description;

  [[nodiscard]] bool operator==(const SourceDescriptor& other) const noexcept;
};

struct EndpointDef {
  EndpointId id;
  Name name;
  EndpointRole role = EndpointRole::Unknown;
  Revision revision;

  [[nodiscard]] bool operator==(const EndpointDef& other) const noexcept;
};

struct LinkDef {
  LinkId id;
  Name name;
  EndpointId from;
  EndpointId to;
  Revision revision;

  [[nodiscard]] bool operator==(const LinkDef& other) const noexcept;
};

/// A queue is only attributable when the reporting source declares queue dwell
/// semantics for it.
struct QueueDef {
  QueueId id;
  Name name;
  LinkId link;
  SemanticsProfile semantics = SemanticsProfile::Unknown;
  Revision revision;

  [[nodiscard]] bool operator==(const QueueDef& other) const noexcept;
};

struct HopDef {
  HopId id;
  Name name;
  HopKind kind = HopKind::Unknown;
  LinkId link;    // invalid when the hop is not bound to a link
  QueueId queue;  // invalid when the hop has no queue binding
  Revision revision;

  [[nodiscard]] bool has_queue() const noexcept { return queue.valid(); }
  [[nodiscard]] bool operator==(const HopDef& other) const noexcept;
};

/// A generation is a deployment epoch of the observed system. Baselines and
/// current evidence are always generation bound: evidence from one generation
/// is never silently compared with another.
struct GenerationDef {
  GenerationId id;
  Name name;
  Revision revision;
  GenerationId supersedes;  // invalid when this generation has no predecessor
  std::string description;

  [[nodiscard]] bool operator==(const GenerationDef& other) const noexcept;
};

/// A path is the ordered sequence of hops observed for one request/response
/// exchange. Paths belong to exactly one generation.
struct PathDef {
  PathId id;
  Name name;
  GenerationId generation;
  Revision revision;
  std::vector<HopId> hops;

  [[nodiscard]] bool operator==(const PathDef& other) const noexcept;
};

/// The definition registry. Identities are content addressed from canonical
/// names, so the same definition yields the same identity in every process.
class Catalog {
 public:
  explicit Catalog(const core::Limits& limits) : limits_(limits) {}

  Result<SourceId> define_source(SourceDescriptor descriptor);
  Result<EndpointId> define_endpoint(EndpointDef definition);
  Result<LinkId> define_link(LinkDef definition);
  Result<QueueId> define_queue(QueueDef definition);
  Result<HopId> define_hop(HopDef definition);
  Result<GenerationId> define_generation(GenerationDef definition);
  Result<PathId> define_path(PathDef definition);

  [[nodiscard]] Result<const SourceDescriptor*> source(SourceId id) const;
  [[nodiscard]] Result<const EndpointDef*> endpoint(EndpointId id) const;
  [[nodiscard]] Result<const LinkDef*> link(LinkId id) const;
  [[nodiscard]] Result<const QueueDef*> queue(QueueId id) const;
  [[nodiscard]] Result<const HopDef*> hop(HopId id) const;
  [[nodiscard]] Result<const GenerationDef*> generation(GenerationId id) const;
  [[nodiscard]] Result<const PathDef*> path(PathId id) const;

  [[nodiscard]] Result<const PathDef*> path_by_name(std::string_view name) const;
  [[nodiscard]] Result<const GenerationDef*> generation_by_name(std::string_view name) const;
  [[nodiscard]] Result<const SourceDescriptor*> source_by_name(std::string_view name) const;

  /// True when another defined generation declares \p id as superseded.
  [[nodiscard]] bool is_superseded(GenerationId id) const;
  /// The single generation that nothing supersedes. Invalid when the lineage
  /// branches (several tips) or when no generation is defined: an ambiguous
  /// "current" is reported as absent rather than guessed.
  [[nodiscard]] GenerationId current_generation() const noexcept;

  [[nodiscard]] std::size_t source_count() const noexcept { return sources_.size(); }
  [[nodiscard]] std::size_t path_count() const noexcept { return paths_.size(); }
  [[nodiscard]] std::size_t generation_count() const noexcept { return generations_.size(); }

  /// Deterministic listings, ordered by identity.
  [[nodiscard]] std::vector<const SourceDescriptor*> sources() const;
  [[nodiscard]] std::vector<const GenerationDef*> generations() const;
  [[nodiscard]] std::vector<const PathDef*> paths() const;

 private:
  template <class Key, class Value>
  [[nodiscard]] Result<Value*> upsert(std::map<Key, Value>& table, const Key& id, Value value,
                                      const core::Name& name, std::size_t limit,
                                      const char* what);

  core::Limits limits_;
  std::map<SourceId, SourceDescriptor> sources_;
  std::map<EndpointId, EndpointDef> endpoints_;
  std::map<LinkId, LinkDef> links_;
  std::map<QueueId, QueueDef> queues_;
  std::map<HopId, HopDef> hops_;
  std::map<GenerationId, GenerationDef> generations_;
  std::map<PathId, PathDef> paths_;
  std::map<std::string, GenerationId> generation_names_;
  std::map<std::string, PathId> path_names_;
  std::map<std::string, SourceId> source_names_;
};

}  // namespace latobs::model
