// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "latency_observatory/attribute/attribute.hpp"
#include "latency_observatory/baseline/baseline.hpp"
#include "latency_observatory/ingest/ingest.hpp"
#include "latency_observatory/runtime/explain.hpp"
#include "latency_observatory/runtime/export.hpp"
#include "latency_observatory/runtime/vocabulary.hpp"
#include "latency_observatory/runtime/worker_pool.hpp"
#include "latency_observatory/store/store.hpp"

namespace latobs::runtime {

struct RuntimeConfig {
  core::RuntimePolicy policy = core::default_policy();
  std::optional<std::filesystem::path> store_directory;
  bool durable_writes = true;
  bool readonly = false;
  WorkerPool::Config workers{};
};

/// One bucket of a historical distribution.
struct HistoryBucket {
  Timestamp from;
  Timestamp to;
  std::uint64_t exchanges_considered = 0;
  std::uint64_t exchanges_included = 0;
  std::uint64_t excluded_stale = 0;
  std::uint64_t excluded_conflicting = 0;
  std::uint64_t excluded_unsupported = 0;
  stats::Distribution end_to_end;
  core::Evidence evidence;
};

struct HistoryRequest {
  PathId path;
  GenerationId generation;
  stats::AggregationMode mode = stats::AggregationMode::Historical;
  stats::TimeWindow window;
  Nanos bucket_ns = 60LL * 1000LL * 1000LL * 1000LL;
  stats::HistogramSpec histogram = stats::HistogramSpec::latency_default();
  std::vector<stats::QuantileProbe> probes = stats::default_quantile_probes();
  SourceId restrict_to_source;
};

struct HistoryResult {
  PathId path;
  GenerationId generation;
  ClockDomainId domain;
  stats::AggregationMode mode = stats::AggregationMode::Historical;
  Nanos bucket_ns = 0;
  std::uint64_t buckets_with_evidence = 0;
  std::uint64_t buckets_empty = 0;
  std::vector<HistoryBucket> buckets;
  core::Evidence evidence;
  std::string policy_digest;
};

struct BaselineRequest {
  Name name;
  PathId path;
  GenerationId generation;
  stats::AggregationMode mode = stats::AggregationMode::Current;
  stats::TimeWindow window;
  stats::HistogramSpec histogram = stats::HistogramSpec::latency_default();
  std::vector<stats::QuantileProbe> probes = stats::default_quantile_probes();
  SourceId source;
};

/// What the runtime can and cannot prove about itself. This structure is the
/// machine readable form of the REAL / SYNTHETIC / UNSUPPORTED boundary.
struct Capability {
  std::string name;
  std::string status;  // "real" | "synthetic" | "unsupported"
  std::string detail;
};

struct Capabilities {
  std::string version;
  std::string build_type;
  std::string compiler;
  bool executable_process = true;
  bool tcp_transport = true;
  bool persistence = false;
  bool sanitizers = false;
  bool hardware_telemetry = false;
  std::vector<Capability> entries;
};

struct EngineStats {
  Timestamp session_started_at;
  Nanos session_uptime_ns = 0;
  std::uint64_t measurements_stored = 0;
  std::uint64_t measurements_current = 0;
  std::uint64_t measurements_historical = 0;
  std::uint64_t measurements_rejected = 0;
  std::uint64_t ingest_batches = 0;
  std::uint64_t records_persisted = 0;
  std::uint64_t baselines = 0;
  std::uint64_t sources = 0;
  std::uint64_t paths = 0;
  std::uint64_t generations = 0;
  std::uint64_t clock_domains = 0;
  std::uint64_t queries = 0;
  std::uint64_t records_recovered = 0;
  bool recovery_truncated = false;
  std::uint64_t store_bytes = 0;
  WorkerPool::Stats workers;
};

/// The latency observatory runtime.
///
/// Concurrency contract (see docs/concurrency.md):
///   * one shared mutex protects all engine state;
///   * writers (definitions, clock synchronization, ingestion, baselines,
///     shutdown) take it exclusively, readers (summaries, history, attribution,
///     explanation, export) take it shared;
///   * no public entry point takes the lock twice: internal helpers are marked
///     "_locked" and assume the caller holds it;
///   * the engine never calls back into the worker pool while holding the lock.
class Engine {
 public:
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<Engine>> create(RuntimeConfig config);

  // --- definitions (writers) ---
  [[nodiscard]] Result<SourceId> define_source(model::SourceDescriptor descriptor);
  [[nodiscard]] Result<EndpointId> define_endpoint(model::EndpointDef definition);
  [[nodiscard]] Result<LinkId> define_link(model::LinkDef definition);
  [[nodiscard]] Result<QueueId> define_queue(model::QueueDef definition);
  [[nodiscard]] Result<HopId> define_hop(model::HopDef definition);
  [[nodiscard]] Result<GenerationId> define_generation(model::GenerationDef definition);
  [[nodiscard]] Result<PathId> define_path(model::PathDef definition);
  [[nodiscard]] Result<ClockDomainId> define_clock_domain(model::ClockDomainDef definition);
  [[nodiscard]] Status record_clock_sync(model::ClockSync sync);

  // --- observation (writer) ---
  [[nodiscard]] Result<ingest::IngestReport> ingest(ingest::IngestRequest request);

  // --- queries (readers) ---
  [[nodiscard]] Result<stats::PathSummary> summarize(const stats::SummaryRequest& request);
  [[nodiscard]] Result<HistoryResult> history(const HistoryRequest& request);
  [[nodiscard]] Result<attribute::AttributionResult> attribute(
      const attribute::AttributionRequest& request);
  [[nodiscard]] Result<Explanation> explain(const stats::SummaryRequest& request);

  // --- baselines ---
  [[nodiscard]] Result<BaselineId> create_baseline(const BaselineRequest& request);
  [[nodiscard]] Result<std::vector<const baseline::Baseline*>> list_baselines() const;

  // --- export ---
  [[nodiscard]] Result<ExportResult> export_data(const ExportRequest& request);

  // --- lifecycle ---
  [[nodiscard]] Status flush();
  [[nodiscard]] Status shutdown();
  [[nodiscard]] bool shut_down() const noexcept;
  [[nodiscard]] Status cancel_pending_work(std::string reason);

  // --- introspection (readers) ---
  [[nodiscard]] EngineStats stats() const;
  [[nodiscard]] store::RecoveryReport recovery() const;
  [[nodiscard]] Capabilities capabilities() const;
  [[nodiscard]] const core::RuntimePolicy& policy() const noexcept { return policy_; }
  [[nodiscard]] WorkerPool& workers() noexcept { return pool_; }
  [[nodiscard]] const model::Catalog& catalog() const noexcept { return catalog_; }
  [[nodiscard]] const model::ClockRegistry& clocks() const noexcept { return clocks_; }

 private:
  explicit Engine(RuntimeConfig config);

  [[nodiscard]] Status load_store(const std::filesystem::path& directory);
  /// Appends one record. When \p sync_now is false the caller is responsible
  /// for syncing before the operation is reported as complete, which turns a
  /// batch of records into a single durable flush.
  [[nodiscard]] Status persist(store::RecordType type, std::string_view payload,
                               bool sync_now = true);
  void reclassify_loaded(model::MeasurementRecord& record);
  [[nodiscard]] Result<Explanation> explain_locked(const stats::SummaryRequest& request,
                                                   const stats::PathSummary& summary);
  [[nodiscard]] Result<ExportResult> export_locked(const ExportRequest& request);
  [[nodiscard]] std::vector<const model::MeasurementRecord*> records_locked() const;

  core::RuntimePolicy policy_;
  RuntimeConfig config_;
  model::Catalog catalog_;
  model::ClockRegistry clocks_;
  ingest::IngestGate gate_;
  baseline::BaselineStore baselines_;
  std::vector<model::MeasurementRecord> records_;
  store::RecoveryReport recovery_;
  EngineStats stats_;
  Timestamp session_start_;
  std::size_t record_capacity_ = 0;

  mutable std::shared_mutex mutex_;
  /// Read only counters that are updated by concurrent readers.
  mutable std::atomic<std::uint64_t> query_count_{0};
  std::unique_ptr<store::StoreWriter> writer_;
  WorkerPool pool_;
  bool shut_down_ = false;
  mutable std::shared_mutex shutdown_mutex_;
  std::string build_type_;
};

/// Canonical JSON rendering of the runtime structures.
void write_json(core::JsonWriter& writer, const HistoryBucket& bucket);
void write_json(core::JsonWriter& writer, const HistoryResult& result);
void write_json(core::JsonWriter& writer, const Capability& capability);
void write_json(core::JsonWriter& writer, const Capabilities& capabilities);
void write_json(core::JsonWriter& writer, const EngineStats& stats);

/// Compile time build description used by capabilities and the CLI.
[[nodiscard]] std::string_view build_type_name() noexcept;
[[nodiscard]] std::string_view compiler_name() noexcept;

}  // namespace latobs::runtime
