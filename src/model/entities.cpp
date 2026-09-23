// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/model/entities.hpp"

#include <algorithm>
#include <utility>

namespace latobs::model {
namespace {

template <class Enum, std::size_t N>
bool parse_named(const std::pair<Enum, std::string_view> (&table)[N], std::string_view text,
                 Enum& out) noexcept {
  for (const auto& entry : table) {
    if (entry.second == text) {
      out = entry.first;
      return true;
    }
  }
  return false;
}

constexpr std::pair<SourceKind, std::string_view> kSourceKinds[] = {
    {SourceKind::Synthetic, "synthetic"},
    {SourceKind::Probe, "probe"},
    {SourceKind::TelemetryAgent, "telemetry_agent"},
    {SourceKind::DerivedAggregate, "derived_aggregate"},
    {SourceKind::Unknown, "unknown"},
};

constexpr std::pair<AuthorityClass, std::string_view> kAuthorityClasses[] = {
    {AuthorityClass::Primary, "primary"},
    {AuthorityClass::Secondary, "secondary"},
    {AuthorityClass::Derived, "derived"},
    {AuthorityClass::Synthetic, "synthetic"},
    {AuthorityClass::Unknown, "unknown"},
};

constexpr std::pair<HopKind, std::string_view> kHopKinds[] = {
    {HopKind::Endpoint, "endpoint"},
    {HopKind::Link, "link"},
    {HopKind::ForwardingStage, "forwarding_stage"},
    {HopKind::QueueingStage, "queueing_stage"},
    {HopKind::Application, "application"},
    {HopKind::Unknown, "unknown"},
};

constexpr std::pair<EndpointRole, std::string_view> kEndpointRoles[] = {
    {EndpointRole::Client, "client"},
    {EndpointRole::Server, "server"},
    {EndpointRole::Intermediate, "intermediate"},
    {EndpointRole::Unknown, "unknown"},
};

constexpr std::pair<SemanticsProfile, std::string_view> kSemantics[] = {
    {SemanticsProfile::Unknown, "unknown"},
    {SemanticsProfile::EndToEndRequestResponse, "end_to_end_request_response"},
    {SemanticsProfile::HopDwell, "hop_dwell"},
    {SemanticsProfile::QueueDwell, "queue_dwell"},
};

}  // namespace

std::string_view to_string(SourceKind kind) noexcept {
  for (const auto& entry : kSourceKinds) {
    if (entry.first == kind) return entry.second;
  }
  return "unknown";
}

bool parse_source_kind(std::string_view text, SourceKind& out) noexcept {
  return parse_named(kSourceKinds, text, out);
}

std::string_view to_string(AuthorityClass authority) noexcept {
  for (const auto& entry : kAuthorityClasses) {
    if (entry.first == authority) return entry.second;
  }
  return "unknown";
}

bool parse_authority_class(std::string_view text, AuthorityClass& out) noexcept {
  return parse_named(kAuthorityClasses, text, out);
}

std::string_view to_string(HopKind kind) noexcept {
  for (const auto& entry : kHopKinds) {
    if (entry.first == kind) return entry.second;
  }
  return "unknown";
}

bool parse_hop_kind(std::string_view text, HopKind& out) noexcept {
  return parse_named(kHopKinds, text, out);
}

std::string_view to_string(EndpointRole role) noexcept {
  for (const auto& entry : kEndpointRoles) {
    if (entry.first == role) return entry.second;
  }
  return "unknown";
}

bool parse_endpoint_role(std::string_view text, EndpointRole& out) noexcept {
  return parse_named(kEndpointRoles, text, out);
}

std::string to_string(SemanticsProfile profile) {
  const auto bits = static_cast<std::uint32_t>(profile);
  if (bits == 0) return "unknown";
  std::string out;
  for (const auto& entry : kSemantics) {
    const auto entry_bits = static_cast<std::uint32_t>(entry.first);
    if (entry_bits == 0 || (bits & entry_bits) == 0) continue;
    if (!out.empty()) out.push_back('|');
    out.append(entry.second);
  }
  if (out.empty()) return "unknown";
  return out;
}

bool parse_semantics_profile(std::string_view text, SemanticsProfile& out) noexcept {
  return parse_named(kSemantics, text, out);
}

bool parse_semantics_list(std::string_view text, SemanticsProfile& out) noexcept {
  out = SemanticsProfile::Unknown;
  if (text.empty()) return false;
  std::uint32_t mask = 0;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const std::size_t separator = text.find('|', offset);
    const std::string_view token = text.substr(
        offset, separator == std::string_view::npos ? std::string_view::npos : separator - offset);
    SemanticsProfile parsed = SemanticsProfile::Unknown;
    if (!token.empty()) {
      if (!parse_semantics_profile(token, parsed)) return false;
      mask |= static_cast<std::uint32_t>(parsed);
    }
    if (separator == std::string_view::npos) break;
    offset = separator + 1;
  }
  out = static_cast<SemanticsProfile>(mask);
  return true;
}

bool has_semantics(SemanticsProfile value, SemanticsProfile flag) noexcept {
  const auto bits = static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag);
  return bits != 0u;
}

SemanticsProfile operator|(SemanticsProfile a, SemanticsProfile b) noexcept {
  return static_cast<SemanticsProfile>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

SemanticsProfile operator&(SemanticsProfile a, SemanticsProfile b) noexcept {
  return static_cast<SemanticsProfile>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

bool SourceDescriptor::operator==(const SourceDescriptor& other) const noexcept {
  return id == other.id && name == other.name && kind == other.kind &&
         authority == other.authority && semantics == other.semantics &&
         revision == other.revision && description == other.description;
}

bool EndpointDef::operator==(const EndpointDef& other) const noexcept {
  return id == other.id && name == other.name && role == other.role &&
         revision == other.revision;
}

bool LinkDef::operator==(const LinkDef& other) const noexcept {
  return id == other.id && name == other.name && from == other.from && to == other.to &&
         revision == other.revision;
}

bool QueueDef::operator==(const QueueDef& other) const noexcept {
  return id == other.id && name == other.name && link == other.link &&
         semantics == other.semantics && revision == other.revision;
}

bool HopDef::operator==(const HopDef& other) const noexcept {
  return id == other.id && name == other.name && kind == other.kind && link == other.link &&
         queue == other.queue && revision == other.revision;
}

bool GenerationDef::operator==(const GenerationDef& other) const noexcept {
  return id == other.id && name == other.name && revision == other.revision &&
         supersedes == other.supersedes && description == other.description;
}

bool PathDef::operator==(const PathDef& other) const noexcept {
  return id == other.id && name == other.name && generation == other.generation &&
         revision == other.revision && hops == other.hops;
}

template <class Key, class Value>
Result<Value*> Catalog::upsert(std::map<Key, Value>& table, const Key& id, Value value,
                               const core::Name& name, std::size_t limit, const char* what) {
  auto existing = table.find(id);
  if (existing == table.end()) {
    if (table.size() >= limit) {
      return core::Error(core::ErrorCode::CapacityExceeded,
                         std::string(what) + " registry is at capacity", name.str());
    }
    auto inserted = table.emplace(id, std::move(value));
    return &inserted.first->second;
  }
  if (!(existing->second.name == name)) {
    // Two distinct canonical names hashed to the same identity. This is
    // reported as a collision; the runtime never merges two different objects.
    return core::Error(core::ErrorCode::Conflict,
                       std::string(what) + " identity collides with a different name",
                       name.str());
  }
  if (existing->second == value) {
    return &existing->second;  // idempotent redefinition of identical content
  }
  if (value.revision <= existing->second.revision) {
    return core::Error(core::ErrorCode::Conflict,
                       std::string(what) +
                           " is already defined with different content at the same or a newer "
                           "revision",
                       name.str());
  }
  existing->second = std::move(value);
  return &existing->second;
}

Result<SourceId> Catalog::define_source(SourceDescriptor descriptor) {
  const core::Name name = descriptor.name;
  const SourceId id = SourceId::derive_from(name.view());
  if (descriptor.id.valid() && descriptor.id != id) {
    return core::Error(core::ErrorCode::Conflict, "source id does not match its canonical name",
                       name.str());
  }
  if (!descriptor.revision.valid()) {
    return core::Error(core::ErrorCode::InvalidArgument, "source revision must be at least 1",
                       name.str());
  }
  descriptor.id = id;
  LATOBS_TRY(pointer, upsert(sources_, id, std::move(descriptor), name, limits_.max_sources, "source"));
  (void)pointer;
  auto existing = source_names_.find(name.str());
  if (existing != source_names_.end() && existing->second != id) {
    return core::Error(core::ErrorCode::Conflict, "source name maps to a different identity",
                       name.str());
  }
  source_names_[name.str()] = id;
  return id;
}

Result<EndpointId> Catalog::define_endpoint(EndpointDef definition) {
  const core::Name name = definition.name;
  const EndpointId id = EndpointId::derive_from(name.view());
  if (!definition.revision.valid()) {
    return core::Error(core::ErrorCode::InvalidArgument, "endpoint revision must be at least 1",
                       name.str());
  }
  definition.id = id;
  LATOBS_TRY(pointer, upsert(endpoints_, id, std::move(definition), name, limits_.max_endpoints,
                             "endpoint"));
  (void)pointer;
  return id;
}

Result<LinkId> Catalog::define_link(LinkDef definition) {
  const core::Name name = definition.name;
  const LinkId id = LinkId::derive_from(name.view());
  if (!definition.revision.valid()) {
    return core::Error(core::ErrorCode::InvalidArgument, "link revision must be at least 1",
                       name.str());
  }
  if (definition.from.valid() && endpoints_.find(definition.from) == endpoints_.end()) {
    return core::Error(core::ErrorCode::NotFound, "link references an unknown source endpoint",
                       name.str());
  }
  if (definition.to.valid() && endpoints_.find(definition.to) == endpoints_.end()) {
    return core::Error(core::ErrorCode::NotFound, "link references an unknown destination endpoint",
                       name.str());
  }
  definition.id = id;
  LATOBS_TRY(pointer, upsert(links_, id, std::move(definition), name, limits_.max_links, "link"));
  (void)pointer;
  return id;
}

Result<QueueId> Catalog::define_queue(QueueDef definition) {
  const core::Name name = definition.name;
  const QueueId id = QueueId::derive_from(name.view());
  if (!definition.revision.valid()) {
    return core::Error(core::ErrorCode::InvalidArgument, "queue revision must be at least 1",
                       name.str());
  }
  if (definition.link.valid() && links_.find(definition.link) == links_.end()) {
    return core::Error(core::ErrorCode::NotFound, "queue references an unknown link", name.str());
  }
  definition.id = id;
  LATOBS_TRY(pointer, upsert(queues_, id, std::move(definition), name, limits_.max_queues, "queue"));
  (void)pointer;
  return id;
}

Result<HopId> Catalog::define_hop(HopDef definition) {
  const core::Name name = definition.name;
  const HopId id = HopId::derive_from(name.view());
  if (!definition.revision.valid()) {
    return core::Error(core::ErrorCode::InvalidArgument, "hop revision must be at least 1",
                       name.str());
  }
  if (definition.link.valid() && links_.find(definition.link) == links_.end()) {
    return core::Error(core::ErrorCode::NotFound, "hop references an unknown link", name.str());
  }
  if (definition.queue.valid() && queues_.find(definition.queue) == queues_.end()) {
    return core::Error(core::ErrorCode::NotFound, "hop references an unknown queue", name.str());
  }
  definition.id = id;
  LATOBS_TRY(pointer, upsert(hops_, id, std::move(definition), name, limits_.max_hops_per_path * 64,
                             "hop"));
  (void)pointer;
  return id;
}

Result<GenerationId> Catalog::define_generation(GenerationDef definition) {
  const core::Name name = definition.name;
  const GenerationId id = GenerationId::derive_from(name.view());
  if (!definition.revision.valid()) {
    return core::Error(core::ErrorCode::InvalidArgument, "generation revision must be at least 1",
                       name.str());
  }
  if (definition.supersedes.valid() && generations_.find(definition.supersedes) == generations_.end()) {
    return core::Error(core::ErrorCode::NotFound,
                       "generation supersedes an unknown generation", name.str());
  }
  if (definition.supersedes == id) {
    return core::Error(core::ErrorCode::InvalidArgument, "a generation cannot supersede itself",
                       name.str());
  }
  definition.id = id;
  LATOBS_TRY(pointer, upsert(generations_, id, std::move(definition), name,
                             limits_.max_generations, "generation"));
  (void)pointer;
  auto existing = generation_names_.find(name.str());
  if (existing != generation_names_.end() && existing->second != id) {
    return core::Error(core::ErrorCode::Conflict, "generation name maps to a different identity",
                       name.str());
  }
  generation_names_[name.str()] = id;
  return id;
}

Result<PathId> Catalog::define_path(PathDef definition) {
  const core::Name name = definition.name;
  const PathId id = PathId::derive_from(name.view());
  if (!definition.revision.valid()) {
    return core::Error(core::ErrorCode::InvalidArgument, "path revision must be at least 1",
                       name.str());
  }
  if (!definition.generation.valid() || generations_.find(definition.generation) == generations_.end()) {
    return core::Error(core::ErrorCode::NotFound, "path references an unknown generation",
                       name.str());
  }
  if (definition.hops.empty()) {
    return core::Error(core::ErrorCode::InvalidArgument, "path must define at least one hop",
                       name.str());
  }
  if (definition.hops.size() > limits_.max_hops_per_path) {
    return core::Error(core::ErrorCode::OutOfRange, "path exceeds the hop limit", name.str());
  }
  for (const HopId hop_id : definition.hops) {
    if (hops_.find(hop_id) == hops_.end()) {
      return core::Error(core::ErrorCode::NotFound, "path references an unknown hop", name.str());
    }
  }
  definition.id = id;
  LATOBS_TRY(pointer, upsert(paths_, id, std::move(definition), name, limits_.max_paths, "path"));
  (void)pointer;
  auto existing = path_names_.find(name.str());
  if (existing != path_names_.end() && existing->second != id) {
    return core::Error(core::ErrorCode::Conflict, "path name maps to a different identity",
                       name.str());
  }
  path_names_[name.str()] = id;
  return id;
}

Result<const SourceDescriptor*> Catalog::source(SourceId id) const {
  const auto found = sources_.find(id);
  if (found == sources_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown source", id.to_hex());
  }
  return &found->second;
}

Result<const EndpointDef*> Catalog::endpoint(EndpointId id) const {
  const auto found = endpoints_.find(id);
  if (found == endpoints_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown endpoint", id.to_hex());
  }
  return &found->second;
}

Result<const LinkDef*> Catalog::link(LinkId id) const {
  const auto found = links_.find(id);
  if (found == links_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown link", id.to_hex());
  }
  return &found->second;
}

Result<const QueueDef*> Catalog::queue(QueueId id) const {
  const auto found = queues_.find(id);
  if (found == queues_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown queue", id.to_hex());
  }
  return &found->second;
}

Result<const HopDef*> Catalog::hop(HopId id) const {
  const auto found = hops_.find(id);
  if (found == hops_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown hop", id.to_hex());
  }
  return &found->second;
}

Result<const GenerationDef*> Catalog::generation(GenerationId id) const {
  const auto found = generations_.find(id);
  if (found == generations_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown generation", id.to_hex());
  }
  return &found->second;
}

Result<const PathDef*> Catalog::path(PathId id) const {
  const auto found = paths_.find(id);
  if (found == paths_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown path", id.to_hex());
  }
  return &found->second;
}

Result<const PathDef*> Catalog::path_by_name(std::string_view name) const {
  const auto found = path_names_.find(std::string(name));
  if (found == path_names_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown path name", std::string(name));
  }
  return path(found->second);
}

Result<const GenerationDef*> Catalog::generation_by_name(std::string_view name) const {
  const auto found = generation_names_.find(std::string(name));
  if (found == generation_names_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown generation name", std::string(name));
  }
  return generation(found->second);
}

Result<const SourceDescriptor*> Catalog::source_by_name(std::string_view name) const {
  const auto found = source_names_.find(std::string(name));
  if (found == source_names_.end()) {
    return core::Error(core::ErrorCode::NotFound, "unknown source name", std::string(name));
  }
  return source(found->second);
}

bool Catalog::is_superseded(GenerationId id) const {
  for (const auto& entry : generations_) {
    if (entry.second.supersedes == id) return true;
  }
  return false;
}

GenerationId Catalog::current_generation() const noexcept {
  GenerationId tip;
  std::size_t tips = 0;
  for (const auto& entry : generations_) {
    if (is_superseded(entry.first)) continue;
    ++tips;
    tip = entry.first;
  }
  if (tips != 1) return GenerationId{};
  return tip;
}

std::vector<const SourceDescriptor*> Catalog::sources() const {
  std::vector<const SourceDescriptor*> out;
  out.reserve(sources_.size());
  for (const auto& entry : sources_) out.push_back(&entry.second);
  return out;
}

std::vector<const GenerationDef*> Catalog::generations() const {
  std::vector<const GenerationDef*> out;
  out.reserve(generations_.size());
  for (const auto& entry : generations_) out.push_back(&entry.second);
  return out;
}

std::vector<const PathDef*> Catalog::paths() const {
  std::vector<const PathDef*> out;
  out.reserve(paths_.size());
  for (const auto& entry : paths_) out.push_back(&entry.second);
  return out;
}

}  // namespace latobs::model
