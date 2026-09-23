// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/runtime/engine.hpp"

#include <algorithm>
#include <shared_mutex>
#include <utility>

#include "latency_observatory/baseline/codec.hpp"
#include "latency_observatory/core/checked.hpp"
#include "latency_observatory/core/digest.hpp"
#include "latency_observatory/core/json.hpp"
#include "latency_observatory/core/time.hpp"
#include "latency_observatory/ingest/codec.hpp"
#include "latency_observatory/model/codec.hpp"
#include "latency_observatory/stats/codec.hpp"

namespace latobs::runtime {
namespace {

constexpr std::size_t kProvenanceLimit = 64;

[[nodiscard]] std::string encode_with(auto&& writer_function) {
  std::string text;
  core::JsonWriter writer(text);
  writer_function(writer);
  LATOBS_ASSERT_MSG(writer.balanced(), "canonical encoding must produce balanced JSON");
  return text;
}

[[nodiscard]] std::string csv_escape(std::string_view text) {
  bool needs_quotes = false;
  for (const char c : text) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) return std::string(text);
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

/// An unknown quantity is an empty field, never a zero: a downstream reader can
/// always distinguish "not observed" from "observed as zero".
[[nodiscard]] std::string optional_nanos_text(const std::optional<Nanos>& value) {
  return value.has_value() ? std::to_string(*value) : std::string();
}

[[nodiscard]] ExportResult finish_export(ExportResult result, std::string text,
                                         const ExportRequest& request) {
  result.text = std::move(text);
  result.content_digest = core::sha256_hex(result.text);
  result.notes.push_back("unknown quantities are exported as null or an empty field, never as zero");
  if (result.truncated) {
    result.notes.push_back("the export stopped at the requested limit; records_skipped reports how "
                           "many matched records were left out");
  }
  if (request.format == ExportFormat::Csv) {
    result.notes.push_back("csv columns are positional and stable for this format version");
  }
  return result;
}

}  // namespace

std::string_view build_type_name() noexcept {
#if defined(NDEBUG)
  return "release";
#else
  return "debug";
#endif
}

std::string_view compiler_name() noexcept {
#if defined(_MSC_VER)
  return "msvc";
#elif defined(__clang__)
  return "clang";
#elif defined(__GNUC__)
  return "gcc";
#else
  return "unknown";
#endif
}

Engine::Engine(RuntimeConfig config)
    : policy_(config.policy),
      config_(std::move(config)),
      catalog_(policy_.limits),
      clocks_(policy_),
      gate_(policy_, catalog_, clocks_),
      baselines_(policy_.limits, policy_) {}

Result<std::unique_ptr<Engine>> Engine::create(RuntimeConfig config) {
  LATOBS_TRY_STATUS(config.policy.validate());
  std::unique_ptr<Engine> engine(new Engine(std::move(config)));
  engine->session_start_ = core::Clock::now_reference();
  engine->build_type_ = std::string(build_type_name());
  engine->record_capacity_ = engine->policy_.limits.max_total_samples;

  if (engine->config_.store_directory.has_value()) {
    const std::filesystem::path directory = *engine->config_.store_directory;
    std::error_code error;
    const bool manifest_exists = std::filesystem::exists(directory / "manifest.lobs", error);
    if (manifest_exists) {
      LATOBS_TRY_STATUS(engine->load_store(directory));
    }
    store::StoreConfig store_config;
    store_config.directory = directory;
    store_config.limits = engine->policy_.limits;
    store_config.policy_digest = engine->policy_.digest();
    store_config.create_if_missing = true;
    LATOBS_TRY(writer, store::StoreWriter::open(store_config));
    engine->writer_ = std::make_unique<store::StoreWriter>(std::move(writer));
  }

  LATOBS_TRY(pool, WorkerPool::create(engine->config_.workers));
  engine->pool_ = std::move(pool);
  return engine;
}

Status Engine::load_store(const std::filesystem::path& directory) {
  store::StoreConfig store_config;
  store_config.directory = directory;
  store_config.limits = policy_.limits;
  store_config.policy_digest = policy_.digest();
  store_config.create_if_missing = false;
  LATOBS_TRY(reader, store::StoreReader::open(store_config));
  std::vector<store::StoredRecord> stored;
  LATOBS_TRY_STATUS(reader.read_all(stored, recovery_));
  if (stored.size() > record_capacity_) {
    return Error(ErrorCode::CapacityExceeded,
                 "the store holds more records than this runtime is configured to load",
                 std::to_string(stored.size()));
  }

  for (store::StoredRecord& record : stored) {
    LATOBS_TRY(document, core::parse_json(record.payload, policy_.limits.max_json_depth));
    switch (record.type) {
      case store::RecordType::Source: {
        LATOBS_TRY(value, model::decode_source(document));
        const Result<SourceId> defined = catalog_.define_source(std::move(value));
        if (!defined) return defined.error();
        break;
      }
      case store::RecordType::Endpoint: {
        LATOBS_TRY(value, model::decode_endpoint(document));
        const Result<EndpointId> defined = catalog_.define_endpoint(std::move(value));
        if (!defined) return defined.error();
        break;
      }
      case store::RecordType::Link: {
        LATOBS_TRY(value, model::decode_link(document));
        const Result<LinkId> defined = catalog_.define_link(std::move(value));
        if (!defined) return defined.error();
        break;
      }
      case store::RecordType::Queue: {
        LATOBS_TRY(value, model::decode_queue(document));
        const Result<QueueId> defined = catalog_.define_queue(std::move(value));
        if (!defined) return defined.error();
        break;
      }
      case store::RecordType::Hop: {
        LATOBS_TRY(value, model::decode_hop(document));
        const Result<HopId> defined = catalog_.define_hop(std::move(value));
        if (!defined) return defined.error();
        break;
      }
      case store::RecordType::Generation: {
        LATOBS_TRY(value, model::decode_generation(document));
        const Result<GenerationId> defined = catalog_.define_generation(std::move(value));
        if (!defined) return defined.error();
        break;
      }
      case store::RecordType::Path: {
        LATOBS_TRY(value, model::decode_path(document));
        const Result<PathId> defined = catalog_.define_path(std::move(value));
        if (!defined) return defined.error();
        break;
      }
      case store::RecordType::ClockDomain: {
        LATOBS_TRY(value, model::decode_clock_domain(document));
        const Result<ClockDomainId> defined = clocks_.define_domain(std::move(value));
        if (!defined) return defined.error();
        break;
      }
      case store::RecordType::ClockSync: {
        LATOBS_TRY(value, model::decode_clock_sync(document));
        LATOBS_TRY_STATUS(clocks_.record_sync(std::move(value)));
        break;
      }
      case store::RecordType::MeasurementCurrent:
      case store::RecordType::MeasurementHistorical: {
        LATOBS_TRY(value, model::decode_measurement(document));
        reclassify_loaded(value);
        records_.push_back(std::move(value));
        break;
      }
      case store::RecordType::Baseline: {
        LATOBS_TRY(value, baseline::decode_baseline(document));
        const Result<BaselineId> added = baselines_.add(std::move(value));
        if (!added) return added.error();
        break;
      }
      case store::RecordType::FenceState: {
        LATOBS_TRY(restored, ingest::decode_fence_state(document));
        gate_.restore_fence(restored.first, std::move(restored.second));
        break;
      }
    }
  }
  return core::ok_status();
}

void Engine::reclassify_loaded(model::MeasurementRecord& record) {
  // A restart never makes historical timing current: the freshness recorded by
  // the previous session is discarded and recomputed against the session start
  // of this process, while observation and receive times are preserved.
  const model::ClockRegistry::AgeEstimate age =
      clocks_.estimate_age(record.stamp.observed_at, session_start_);
  if (age.age_ns.has_value()) {
    record.evidence.set_freshness(policy_.freshness.classify(*age.age_ns));
  } else {
    record.evidence.set_freshness(core::Freshness::Unknown);
  }
  record.evidence.add_reason(core::ReasonCode::RestartLoadedEvidence);
  if (!record.evidence.usable() && record.evidence.state() == core::EvidenceState::Observed) {
    record.evidence.add_reason(core::ReasonCode::EvidenceStale);
  }
}

Status Engine::persist(store::RecordType type, std::string_view payload, bool sync_now) {
  if (config_.readonly) {
    return Error(ErrorCode::Refused, "the runtime is configured as read only");
  }
  if (writer_ == nullptr) return core::ok_status();
  LATOBS_TRY_STATUS(writer_->append(type, payload));
  if (config_.durable_writes && sync_now) {
    LATOBS_TRY_STATUS(writer_->sync());
  }
  return core::ok_status();
}

Result<SourceId> Engine::define_source(model::SourceDescriptor descriptor) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  LATOBS_TRY(id, catalog_.define_source(descriptor));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    model::write_json(writer, descriptor);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::Source, text));
  return id;
}

Result<EndpointId> Engine::define_endpoint(model::EndpointDef definition) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  LATOBS_TRY(id, catalog_.define_endpoint(definition));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    model::write_json(writer, definition);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::Endpoint, text));
  return id;
}

Result<LinkId> Engine::define_link(model::LinkDef definition) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  LATOBS_TRY(id, catalog_.define_link(definition));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    model::write_json(writer, definition);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::Link, text));
  return id;
}

Result<QueueId> Engine::define_queue(model::QueueDef definition) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  LATOBS_TRY(id, catalog_.define_queue(definition));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    model::write_json(writer, definition);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::Queue, text));
  return id;
}

Result<HopId> Engine::define_hop(model::HopDef definition) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  LATOBS_TRY(id, catalog_.define_hop(definition));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    model::write_json(writer, definition);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::Hop, text));
  return id;
}

Result<GenerationId> Engine::define_generation(model::GenerationDef definition) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  LATOBS_TRY(id, catalog_.define_generation(definition));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    model::write_json(writer, definition);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::Generation, text));
  return id;
}

Result<PathId> Engine::define_path(model::PathDef definition) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  LATOBS_TRY(id, catalog_.define_path(definition));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    model::write_json(writer, definition);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::Path, text));
  return id;
}

Result<ClockDomainId> Engine::define_clock_domain(model::ClockDomainDef definition) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  LATOBS_TRY(id, clocks_.define_domain(definition));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    model::write_json(writer, definition);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::ClockDomain, text));
  return id;
}

Status Engine::record_clock_sync(model::ClockSync sync) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  LATOBS_TRY_STATUS(clocks_.record_sync(sync));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    model::write_json(writer, sync);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::ClockSync, text));
  return core::ok_status();
}

Result<ingest::IngestReport> Engine::ingest(ingest::IngestRequest request) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  if (config_.readonly) return Error(ErrorCode::Refused, "the runtime is read only");

  const std::optional<std::size_t> projected =
      core::checked_add_size(records_.size(), request.records.size());
  if (!projected.has_value() || *projected > record_capacity_) {
    return Error(ErrorCode::CapacityExceeded,
                 "the runtime has reached its configured evidence capacity",
                 std::to_string(records_.size()));
  }

  std::vector<model::MeasurementRecord> accepted_current;
  std::vector<model::MeasurementRecord> accepted_history;
  ingest::IngestReport report;
  LATOBS_TRY_STATUS(gate_.admit(std::move(request), accepted_current, accepted_history, report));
  ++stats_.ingest_batches;

  // The whole batch is appended first and made durable once: a durable flush
  // per record would cost a device sync per observation.
  for (model::MeasurementRecord& record : accepted_current) {
    const std::string text = encode_with(
        [&](core::JsonWriter& writer) { model::write_json(writer, record); });
    LATOBS_TRY_STATUS(persist(store::RecordType::MeasurementCurrent, text, false));
    ++stats_.records_persisted;
    records_.push_back(std::move(record));
  }
  for (model::MeasurementRecord& record : accepted_history) {
    const std::string text = encode_with(
        [&](core::JsonWriter& writer) { model::write_json(writer, record); });
    LATOBS_TRY_STATUS(persist(store::RecordType::MeasurementHistorical, text, false));
    ++stats_.records_persisted;
    records_.push_back(std::move(record));
  }

  // The fence state is persisted as well, so a replay is still refused after a
  // restart instead of being accepted as fresh evidence.
  for (const auto& entry : gate_.fences()) {
    const std::string text = encode_with([&](core::JsonWriter& writer) {
      ingest::write_json(writer, entry.first, entry.second);
    });
    LATOBS_TRY_STATUS(persist(store::RecordType::FenceState, text, false));
    ++stats_.records_persisted;
  }
  if (config_.durable_writes && writer_ != nullptr) {
    LATOBS_TRY_STATUS(writer_->sync());
  }

  stats_.measurements_stored = records_.size();
  stats_.measurements_current += report.accepted_current;
  stats_.measurements_historical += report.accepted_historical;
  stats_.measurements_rejected += report.rejected;
  return report;
}

std::vector<const model::MeasurementRecord*> Engine::records_locked() const {
  std::vector<const model::MeasurementRecord*> pointers;
  pointers.reserve(records_.size());
  for (const model::MeasurementRecord& record : records_) pointers.push_back(&record);
  return pointers;
}

Result<stats::PathSummary> Engine::summarize(const stats::SummaryRequest& request) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  query_count_.fetch_add(1, std::memory_order_relaxed);
  const std::vector<const model::MeasurementRecord*> pointers = records_locked();
  return stats::summarize(pointers, catalog_, request, policy_);
}

Result<HistoryResult> Engine::history(const HistoryRequest& request) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  query_count_.fetch_add(1, std::memory_order_relaxed);
  if (!request.window.valid()) {
    return Error(ErrorCode::InvalidArgument, "history requires a valid time window");
  }
  if (request.bucket_ns <= 0) {
    return Error(ErrorCode::InvalidArgument, "history bucket width must be positive");
  }
  if (!request.generation.valid()) {
    return Error(ErrorCode::InvalidArgument, "history requires an explicit generation");
  }
  LATOBS_TRY(path_definition, catalog_.path(request.path));
  const std::optional<Nanos> span =
      core::checked_sub_i64(request.window.to.ns, request.window.from.ns);
  if (!span.has_value()) {
    return Error(ErrorCode::Overflow, "history window span overflows");
  }
  const Nanos bucket_ns = request.bucket_ns;
  // Round the bucket count up without letting the addition overflow: a window
  // and a bucket width can both be close to the representable maximum.
  const std::optional<Nanos> rounded = core::checked_add_i64(*span, bucket_ns - 1);
  if (!rounded.has_value()) {
    return Error(ErrorCode::Overflow, "history bucket rounding overflows");
  }
  const std::uint64_t bucket_count =
      static_cast<std::uint64_t>(*rounded / bucket_ns);
  if (bucket_count == 0 || bucket_count > policy_.limits.max_history_buckets) {
    return Error(ErrorCode::OutOfRange, "history bucket count exceeds the configured limit",
                 std::to_string(bucket_count));
  }

  HistoryResult result;
  result.path = request.path;
  result.generation = request.generation;
  result.domain = request.window.from.domain;
  result.mode = request.mode;
  result.bucket_ns = bucket_ns;
  result.policy_digest = policy_.digest();
  result.buckets.reserve(static_cast<std::size_t>(bucket_count));
  std::vector<std::vector<Nanos>> values(static_cast<std::size_t>(bucket_count));
  for (std::uint64_t index = 0; index < bucket_count; ++index) {
    HistoryBucket bucket;
    bucket.from = Timestamp{request.window.from.ns + static_cast<Nanos>(index) * bucket_ns,
                            request.window.from.domain};
    bucket.to = Timestamp{bucket.from.ns + bucket_ns, request.window.from.domain};
    bucket.end_to_end.histogram = stats::Histogram::empty(request.histogram);
    result.buckets.push_back(std::move(bucket));
  }

  core::Evidence evidence = core::Evidence::observed(core::Freshness::Fresh, core::Confidence::High);
  bool saw_any = false;
  for (const model::MeasurementRecord& record : records_) {
    const stats::RecordClass classification =
        stats::classify_record(record, request.path, request.generation, request.mode,
                               request.window, request.restrict_to_source);
    if (classification != stats::RecordClass::Included &&
        classification != stats::RecordClass::Conflicting &&
        classification != stats::RecordClass::Unsupported &&
        classification != stats::RecordClass::Stale) {
      continue;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(record.stamp.received_at.ns - request.window.from.ns);
    const std::uint64_t index = offset / static_cast<std::uint64_t>(bucket_ns);
    if (index >= bucket_count) continue;
    HistoryBucket& bucket = result.buckets[static_cast<std::size_t>(index)];
    ++bucket.exchanges_considered;
    if (classification == stats::RecordClass::Conflicting) {
      ++bucket.excluded_conflicting;
      continue;
    }
    if (classification == stats::RecordClass::Unsupported) {
      ++bucket.excluded_unsupported;
      continue;
    }
    if (classification == stats::RecordClass::Stale) {
      ++bucket.excluded_stale;
      continue;
    }
    ++bucket.exchanges_included;
    values[static_cast<std::size_t>(index)].push_back(record.rtt_ns);
    saw_any = true;
    evidence = core::Evidence::merge(evidence, record.evidence);
  }

  for (std::size_t index = 0; index < result.buckets.size(); ++index) {
    HistoryBucket& bucket = result.buckets[index];
    LATOBS_TRY(distribution, stats::aggregate(std::move(values[index]), request.histogram,
                                              request.probes, policy_.limits));
    bucket.end_to_end = std::move(distribution);
    bucket.evidence.set_state(core::EvidenceState::Observed);
    bucket.evidence.set_freshness(core::Freshness::Fresh);
    bucket.evidence.set_confidence(core::Confidence::High);
    if (bucket.exchanges_included == 0) {
      // An empty bucket is unknown, never a zero distribution.
      bucket.evidence = core::Evidence::merge(
          bucket.evidence,
          core::Evidence::unknown(core::ReasonCode::NoSamples, std::to_string(bucket.from.ns)));
      ++result.buckets_empty;
    } else {
      if (bucket.excluded_stale != 0) {
        bucket.evidence.add_reason(core::ReasonCode::EvidenceStale,
                                   std::to_string(bucket.excluded_stale));
      }
      if (bucket.excluded_conflicting != 0) {
        bucket.evidence.add_reason(core::ReasonCode::HopConflicting,
                                   std::to_string(bucket.excluded_conflicting));
      }
      ++result.buckets_with_evidence;
    }
    bucket.evidence.add_reason(core::ReasonCode::AggregateDeterministic);
  }
  if (!saw_any) {
    evidence = core::Evidence::merge(
        evidence, core::Evidence::unknown(core::ReasonCode::NoSamples, "no evidence in window"));
  }
  evidence.add_reason(core::ReasonCode::AggregateDeterministic);
  result.evidence = evidence;
  (void)path_definition;
  return result;
}

Result<attribute::AttributionResult> Engine::attribute(
    const attribute::AttributionRequest& request) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  query_count_.fetch_add(1, std::memory_order_relaxed);
  const std::vector<const model::MeasurementRecord*> pointers = records_locked();
  return attribute::attribute(pointers, catalog_, clocks_, &baselines_, request, policy_);
}

Result<BaselineId> Engine::create_baseline(const BaselineRequest& request) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (shut_down_) return Error(ErrorCode::ShuttingDown, "the runtime is shut down");
  stats::SummaryRequest summary_request;
  summary_request.path = request.path;
  summary_request.generation = request.generation;
  summary_request.mode = request.mode;
  summary_request.window = request.window;
  summary_request.histogram = request.histogram;
  summary_request.probes = request.probes;
  summary_request.restrict_to_source = request.source;
  const std::vector<const model::MeasurementRecord*> pointers = records_locked();
  LATOBS_TRY(summary, stats::summarize(pointers, catalog_, summary_request, policy_));
  if (summary.exchanges_included == 0) {
    return Error(ErrorCode::Refused,
                 "a baseline cannot be created from a window with no usable evidence");
  }
  baseline::Baseline value;
  value.name = request.name;
  value.path = request.path;
  value.generation = request.generation;
  value.domain = request.window.from.domain;
  value.revision = Revision::first();
  value.histogram = request.histogram;
  value.probes = request.probes;
  value.source = request.source;
  value.synthetic = request.source.valid();
  value.created_at = core::Clock::now_reference();
  value.window = request.window;
  value.exchange_count = summary.exchanges_included;
  value.end_to_end = summary.end_to_end;
  value.hops = summary.hops;
  value.evidence = summary.evidence;
  value.policy_digest = policy_.digest();
  LATOBS_TRY(id, baselines_.add(value));
  LATOBS_TRY(stored, baselines_.get(id));
  const std::string text = encode_with([&](core::JsonWriter& writer) {
    baseline::write_json(writer, *stored);
  });
  LATOBS_TRY_STATUS(persist(store::RecordType::Baseline, text));
  ++stats_.records_persisted;
  stats_.baselines = baselines_.size();
  return id;
}

Result<std::vector<const baseline::Baseline*>> Engine::list_baselines() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return baselines_.list();
}

Result<Explanation> Engine::explain_locked(const stats::SummaryRequest& request,
                                           const stats::PathSummary& summary) {
  Explanation explanation;
  explanation.subject = "path.summary";
  explanation.policy_digest = policy_.digest();
  explanation.policy_text = policy_.canonical_text();
  explanation.histogram_digest = request.histogram.digest();
  explanation.window_from_ns = static_cast<std::uint64_t>(
      summary.window.from.ns < 0 ? 0 : summary.window.from.ns);
  explanation.window_to_ns =
      static_cast<std::uint64_t>(summary.window.to.ns < 0 ? 0 : summary.window.to.ns);
  explanation.as_of_ns = summary.as_of.ns;
  explanation.evidence_state = std::string(core::to_string(summary.evidence.state()));
  explanation.freshness = std::string(core::to_string(summary.evidence.freshness()));
  explanation.confidence = std::string(core::to_string(summary.evidence.confidence()));
  explanation.reasons = summary.evidence.reasons();
  explanation.notes.emplace_back(
      "no causal claim: the runtime reports observed time and deviations from a generation bound "
      "baseline, never causes");
  explanation.notes.emplace_back(
      "aggregation is deterministic: identical evidence yields identical output regardless of "
      "arrival order");
  explanation.notes.emplace_back(
      "missing hops are unknown, never zero: coverage is reported separately from the distribution");
  if (summary.mode == stats::AggregationMode::Historical) {
    explanation.notes.emplace_back(
        "historical mode: stale evidence is included and its freshness is reported per bucket");
  }
  if (summary.generation != catalog_.current_generation() && catalog_.current_generation().valid()) {
    explanation.notes.emplace_back(
        "the queried generation is not the current generation of this lineage");
  }
  if (recovery_.truncated || recovery_.segments_rejected != 0) {
    explanation.recovery_applied = true;
    explanation.recovery_reasons = recovery_.reasons;
  }

  for (const model::MeasurementRecord& record : records_) {
    if (explanation.provenance.size() >= kProvenanceLimit) {
      explanation.provenance_truncated = true;
      break;
    }
    if (stats::classify_record(record, request.path, request.generation, request.mode,
                               request.window, request.restrict_to_source) !=
        stats::RecordClass::Included) {
      continue;
    }
    ProvenanceEntry entry;
    entry.measurement = record.id.to_hex();
    entry.source = record.source.to_hex();
    entry.epoch = record.epoch.to_hex();
    entry.incarnation = record.incarnation.to_hex();
    entry.sequence = record.sequence.value();
    const Result<const model::SourceDescriptor*> source = catalog_.source(record.source);
    if (source.has_value()) {
      entry.source_kind = std::string(model::to_string(source.value()->kind));
      entry.authority = std::string(model::to_string(source.value()->authority));
    }
    entry.observed_at_ns = record.stamp.observed_at.ns;
    entry.observed_at_domain = record.stamp.observed_at.domain.to_hex();
    entry.received_at_ns = record.stamp.received_at.ns;
    entry.state = std::string(core::to_string(record.evidence.state()));
    entry.freshness = std::string(core::to_string(record.evidence.freshness()));
    entry.synthetic = record.synthetic;
    explanation.provenance.push_back(std::move(entry));
  }
  return explanation;
}

Result<Explanation> Engine::explain(const stats::SummaryRequest& request) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  query_count_.fetch_add(1, std::memory_order_relaxed);
  const std::vector<const model::MeasurementRecord*> pointers = records_locked();
  LATOBS_TRY(summary, stats::summarize(pointers, catalog_, request, policy_));
  return explain_locked(request, summary);
}

Result<ExportResult> Engine::export_locked(const ExportRequest& request) {
  ExportResult result;
  if (request.limit == 0) {
    return Error(ErrorCode::InvalidArgument, "an export limit of zero would export nothing");
  }
  const std::size_t limit = std::min(request.limit, policy_.limits.max_export_records);
  std::string text;

  if (request.kind == ExportKind::Summary) {
    if (!request.path.valid()) {
      return Error(ErrorCode::InvalidArgument, "a summary export requires a path");
    }
    if (!request.generation.valid()) {
      return Error(ErrorCode::InvalidArgument, "a summary export requires a generation");
    }
    const Timestamp now = core::Clock::now_reference();
    const Timestamp from = request.from.valid() ? request.from : Timestamp{0, now.domain};
    const Timestamp to = request.to.valid() ? request.to : Timestamp{now.ns + 1, now.domain};
    LATOBS_TRY(window, stats::TimeWindow::make(from, to));
    stats::SummaryRequest summary_request;
    summary_request.path = request.path;
    summary_request.generation = request.generation;
    summary_request.mode = stats::AggregationMode::Historical;
    summary_request.window = window;
    summary_request.histogram = stats::HistogramSpec::latency_default();
    const std::vector<const model::MeasurementRecord*> pointers = records_locked();
    LATOBS_TRY(summary, stats::summarize(pointers, catalog_, summary_request, policy_));
    core::JsonWriter writer(text, request.pretty);
    stats::write_json(writer, summary);
    result.records_exported = summary.exchanges_included;
    return finish_export(std::move(result), std::move(text), request);
  }

  if (request.kind == ExportKind::Baselines) {
    core::JsonWriter writer(text, request.pretty);
    writer.begin_object();
    writer.field("kind", "baselines");
    writer.field("policy_digest", policy_.digest());
    writer.field_array("baselines");
    std::uint64_t exported = 0;
    for (const baseline::Baseline* value : baselines_.list()) {
      if (exported >= limit) {
        result.truncated = true;
        ++result.records_skipped;
        continue;
      }
      baseline::write_json(writer, *value);
      ++exported;
    }
    writer.end_array();
    writer.end_object();
    result.records_exported = exported;
    return finish_export(std::move(result), std::move(text), request);
  }

  std::uint64_t exported = 0;
  std::uint64_t skipped = 0;
  auto matches = [&](const model::MeasurementRecord& record) {
    if (request.path.valid() && record.path != request.path) return false;
    if (request.generation.valid() && record.generation != request.generation) return false;
    if (request.from.valid() && record.stamp.received_at.ns < request.from.ns) return false;
    if (request.to.valid() && record.stamp.received_at.ns >= request.to.ns) return false;
    return true;
  };

  if (request.format == ExportFormat::Csv) {
    text.append("measurement,path,generation,source,epoch,incarnation,sequence,observed_at_ns,"
                "observed_at_domain,received_at_ns,state,freshness,confidence,rtt_ns");
    // Columns are keyed by hop index, so an exchange that never reported a
    // short hop still leaves an empty column instead of shifting the row.
    std::size_t max_hops = 0;
    for (const model::MeasurementRecord& record : records_) {
      for (const model::HopObservation& hop : record.hops) {
        max_hops = std::max(max_hops, static_cast<std::size_t>(hop.index.value()) + 1);
      }
    }
    for (std::size_t index = 0; index < max_hops; ++index) {
      text.append(",hop");
      text.append(std::to_string(index));
      text.append("_dwell_ns,hop");
      text.append(std::to_string(index));
      text.append("_queue_dwell_ns");
    }
    text.push_back('\n');
    for (const model::MeasurementRecord& record : records_) {
      if (!matches(record)) continue;
      if (exported >= limit) {
        ++skipped;
        result.truncated = true;
        continue;
      }
      text.append(csv_escape(record.id.to_hex()));
      text.push_back(',');
      text.append(csv_escape(record.path.to_hex()));
      text.push_back(',');
      text.append(csv_escape(record.generation.to_hex()));
      text.push_back(',');
      text.append(csv_escape(record.source.to_hex()));
      text.push_back(',');
      text.append(csv_escape(record.epoch.to_hex()));
      text.push_back(',');
      text.append(csv_escape(record.incarnation.to_hex()));
      text.push_back(',');
      text.append(std::to_string(record.sequence.value()));
      text.push_back(',');
      text.append(std::to_string(record.stamp.observed_at.ns));
      text.push_back(',');
      text.append(csv_escape(record.stamp.observed_at.domain.to_hex()));
      text.push_back(',');
      text.append(std::to_string(record.stamp.received_at.ns));
      text.push_back(',');
      text.append(core::to_string(record.evidence.state()));
      text.push_back(',');
      text.append(core::to_string(record.evidence.freshness()));
      text.push_back(',');
      text.append(core::to_string(record.evidence.confidence()));
      text.push_back(',');
      text.append(std::to_string(record.rtt_ns));
      for (std::size_t index = 0; index < max_hops; ++index) {
        text.push_back(',');
        const Result<HopIndex> hop_index = HopIndex::from_value(static_cast<std::uint32_t>(index));
        const model::HopObservation* observation =
            hop_index.has_value() ? record.hop_at(*hop_index) : nullptr;
        if (observation != nullptr) {
          text.append(optional_nanos_text(observation->dwell_ns));
          text.push_back(',');
          text.append(optional_nanos_text(observation->queue_dwell_ns));
        } else {
          // The hop was not reported: two empty fields, never zero.
          text.append(",");
        }
      }
      text.push_back('\n');
      ++exported;
    }
  } else {
    core::JsonWriter writer(text, request.pretty);
    writer.begin_object();
    writer.field("kind", "samples");
    writer.field("policy_digest", policy_.digest());
    writer.field_array("measurements");
    for (const model::MeasurementRecord& record : records_) {
      if (!matches(record)) continue;
      if (exported >= limit) {
        ++skipped;
        result.truncated = true;
        continue;
      }
      model::write_json(writer, record);
      ++exported;
    }
    writer.end_array();
    writer.end_object();
  }
  result.records_exported = exported;
  result.records_skipped = skipped;
  return finish_export(std::move(result), std::move(text), request);
}

Result<ExportResult> Engine::export_data(const ExportRequest& request) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  query_count_.fetch_add(1, std::memory_order_relaxed);
  return export_locked(request);
}

Status Engine::flush() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (writer_ == nullptr) return core::ok_status();
  return writer_->sync();
}

Status Engine::cancel_pending_work(std::string reason) {
  pool_.cancel_pending(std::move(reason));
  return core::ok_status();
}

Status Engine::shutdown() {
  {
    std::unique_lock<std::shared_mutex> lock(shutdown_mutex_);
    if (shut_down_) return core::ok_status();
    shut_down_ = true;
  }
  LATOBS_TRY_STATUS(pool_.shutdown(true));
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (writer_ != nullptr) {
    LATOBS_TRY_STATUS(writer_->close());
    writer_.reset();
  }
  return core::ok_status();
}

bool Engine::shut_down() const noexcept {
  std::shared_lock<std::shared_mutex> lock(shutdown_mutex_);
  return shut_down_;
}

EngineStats Engine::stats() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  EngineStats copy;
  copy.session_started_at = session_start_;
  copy.session_uptime_ns = core::Clock::now_reference().ns - session_start_.ns;
  copy.measurements_stored = records_.size();
  copy.measurements_current = stats_.measurements_current;
  copy.measurements_historical = stats_.measurements_historical;
  copy.measurements_rejected = stats_.measurements_rejected;
  copy.ingest_batches = stats_.ingest_batches;
  copy.records_persisted = stats_.records_persisted;
  copy.baselines = baselines_.size();
  copy.sources = catalog_.source_count();
  copy.paths = catalog_.path_count();
  copy.generations = catalog_.generation_count();
  copy.clock_domains = clocks_.domain_count();
  copy.queries = query_count_.load(std::memory_order_relaxed);
  copy.records_recovered = recovery_.records_read;
  copy.recovery_truncated = recovery_.truncated;
  copy.store_bytes = writer_ != nullptr ? writer_->bytes_written() : 0;
  copy.workers = pool_.stats();
  return copy;
}

store::RecoveryReport Engine::recovery() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return recovery_;
}

Capabilities Engine::capabilities() const {
  Capabilities capabilities;
  capabilities.version = "1.0.0";
  capabilities.build_type = std::string(build_type_name());
  capabilities.compiler = std::string(compiler_name());
  capabilities.persistence = writer_ != nullptr;
  capabilities.tcp_transport = true;
#if defined(LATOBS_ASAN)
  capabilities.sanitizers = true;
#endif
  capabilities.entries = {
      Capability{"latency observation (end to end and per hop)", "real",
                 "measured from submitted observations with full provenance"},
      Capability{"deterministic aggregation", "real",
                 "count, min, max, sum, exact rational quantiles, explicit histograms"},
      Capability{"clock comparability gating", "real",
                 "dwells are only derived when both clock domains are comparable"},
      Capability{"generation bound baselines", "real",
                 "a baseline is refused across generations, paths, domains and bucketings"},
      Capability{"versioned integrity checked persistence", "real",
                 capabilities.persistence
                     ? "segment framing with CRC-32C and a versioned manifest"
                     : "not configured for this process"},
      Capability{"network ingest transport", "real",
                 "line oriented TCP transport, proven with independent processes"},
      Capability{"switch, ASIC, RDMA, InfiniBand or NVLink telemetry", "unsupported",
                 "the runtime never claims hardware counters it did not receive"},
      Capability{"causality inference", "unsupported",
                 "anomalies are statistical deviations from a baseline, never causes"},
      Capability{"latency SLO enforcement or traffic control", "unsupported",
                 "out of scope: this runtime observes and attributes only"},
      Capability{"multi host fabric observation", "unsupported",
                 "sources are declarations; the runtime does not verify remote topology"},
      Capability{"synthetic scenario generator", "synthetic",
                 "the demo scenario is generated in process and labelled synthetic"},
  };
  return capabilities;
}

void write_json(core::JsonWriter& writer, const HistoryBucket& bucket) {
  writer.begin_object();
  writer.field("from_ns", bucket.from.ns);
  writer.field("to_ns", bucket.to.ns);
  writer.field("exchanges_considered", bucket.exchanges_considered);
  writer.field("exchanges_included", bucket.exchanges_included);
  writer.field("excluded_stale", bucket.excluded_stale);
  writer.field("excluded_conflicting", bucket.excluded_conflicting);
  writer.field("excluded_unsupported", bucket.excluded_unsupported);
  writer.key("end_to_end");
  stats::write_json(writer, bucket.end_to_end);
  writer.field("evidence_state", core::to_string(bucket.evidence.state()));
  writer.field_array("reasons");
  for (const core::Reason& reason : bucket.evidence.reasons()) {
    writer.begin_object();
    writer.field("code", core::to_string(reason.code));
    writer.field("detail", reason.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const HistoryResult& result) {
  writer.begin_object();
  writer.field("path", result.path.to_hex());
  writer.field("generation", result.generation.to_hex());
  writer.field("clock_domain", result.domain.to_hex());
  writer.field("mode", stats::to_string(result.mode));
  writer.field("bucket_ns", result.bucket_ns);
  writer.field("buckets_with_evidence", result.buckets_with_evidence);
  writer.field("buckets_empty", result.buckets_empty);
  writer.field("policy_digest", result.policy_digest);
  writer.field_array("buckets");
  for (const HistoryBucket& bucket : result.buckets) write_json(writer, bucket);
  writer.end_array();
  writer.field("evidence_state", core::to_string(result.evidence.state()));
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const Capability& capability) {
  writer.begin_object();
  writer.field("capability", capability.name);
  writer.field("status", capability.status);
  writer.field("detail", capability.detail);
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const Capabilities& capabilities) {
  writer.begin_object();
  writer.field("version", capabilities.version);
  writer.field("build_type", capabilities.build_type);
  writer.field("compiler", capabilities.compiler);
  writer.field("executable_process", capabilities.executable_process);
  writer.field("tcp_transport", capabilities.tcp_transport);
  writer.field("persistence", capabilities.persistence);
  writer.field("sanitizers", capabilities.sanitizers);
  writer.field("hardware_telemetry", capabilities.hardware_telemetry);
  writer.field_array("entries");
  for (const Capability& capability : capabilities.entries) write_json(writer, capability);
  writer.end_array();
  writer.end_object();
}

void write_json(core::JsonWriter& writer, const EngineStats& stats) {
  writer.begin_object();
  writer.field("session_started_at_ns", stats.session_started_at.ns);
  writer.field("session_uptime_ns", stats.session_uptime_ns);
  writer.field("measurements_stored", stats.measurements_stored);
  writer.field("measurements_current", stats.measurements_current);
  writer.field("measurements_historical", stats.measurements_historical);
  writer.field("measurements_rejected", stats.measurements_rejected);
  writer.field("ingest_batches", stats.ingest_batches);
  writer.field("records_persisted", stats.records_persisted);
  writer.field("baselines", stats.baselines);
  writer.field("sources", stats.sources);
  writer.field("paths", stats.paths);
  writer.field("generations", stats.generations);
  writer.field("clock_domains", stats.clock_domains);
  writer.field("queries", stats.queries);
  writer.field("records_recovered", stats.records_recovered);
  writer.field("recovery_truncated", stats.recovery_truncated);
  writer.field("store_bytes", stats.store_bytes);
  writer.field_object("workers");
  writer.field("submitted", stats.workers.submitted);
  writer.field("completed", stats.workers.completed);
  writer.field("failed", stats.workers.failed);
  writer.field("cancelled", stats.workers.cancelled);
  writer.field("refused", stats.workers.refused);
  writer.field("reentrant_refusals", stats.workers.reentrant_refusals);
  writer.field("peak_pending", stats.workers.peak_pending);
  writer.field("pending", stats.workers.pending);
  writer.field("shutting_down", stats.workers.shutting_down);
  writer.end_object();
  writer.end_object();
}

}  // namespace latobs::runtime
