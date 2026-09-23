// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Shared test fixtures: temporary directories, a complete synthetic scenario,
/// deterministic record builders and a real child process helper.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "latency_observatory/runtime/engine.hpp"
#include "latency_observatory/runtime/service.hpp"
#include "latency_observatory/stats/summary.hpp"
#include "test_framework.hpp"

namespace latobs::test {

using core::ClockDomainId;
using core::EndpointId;
using core::EpochId;
using core::Error;
using core::ErrorCode;
using core::GenerationId;
using core::HopId;
using core::HopIndex;
using core::IncarnationId;
using core::LinkId;
using core::MeasurementId;
using core::MonoTime;
using core::Name;
using core::Nanos;
using core::PathId;
using core::QueueId;
using core::Result;
using core::Revision;
using core::Sequence;
using core::SourceId;
using core::Status;
using core::Timestamp;

/// A directory under the system temporary directory, removed on destruction.
class TempDir {
 public:
  explicit TempDir(std::string_view label);
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir();

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::string text() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

/// A fully defined synthetic scenario: two clock domains, endpoints, a link with
/// an egress queue, three hops and one path bound to one generation.
struct Scenario {
  std::unique_ptr<runtime::Engine> engine;
  SourceId source;
  GenerationId generation;
  PathId path;
  ClockDomainId reference_domain;
  ClockDomainId edge_domain;
  HopId client_hop;
  HopId link_hop;
  HopId queue_hop;
  QueueId queue;
  LinkId link;

  /// Number of generations currently defined.
  [[nodiscard]] std::size_t generation_count() const;
};

struct ScenarioOptions {
  std::optional<std::filesystem::path> store_directory;
  bool define_edge_domain = true;
  bool declare_queue_semantics = true;
  bool declare_hop_semantics = true;
  core::RuntimePolicy policy = core::default_policy();
};

[[nodiscard]] Result<Scenario> build_scenario(const ScenarioOptions& options = {});

/// Builds one measurement record with the given hop dwells.
/// A dwell of -1 marks a hop that the source did not report at all.
struct RecordSpec {
  std::int64_t observed_at_ns = 1000000000000LL;
  std::int64_t request_ns = 0;
  std::int64_t hop_dwells_ns[3] = {1000, 2000, 3000};
  std::uint64_t sequence = 0;
  std::string epoch = "epoch.one";
  std::string incarnation = "incarnation.one";
  std::uint32_t source_revision = 1;
  std::string generation_name;
  std::string path_name;
  std::string domain_name;
  /// Extra time observed end to end that is not attributed to any hop. A
  /// negative value produces a hop overlap.
  std::int64_t end_to_end_extra_ns = 0;
  bool omit_hops = false;
  /// When true the second hop enters on the reference domain and exits on the
  /// edge domain: the canonical cross clock domain measurement.
  bool cross_domain_hop = false;
};

[[nodiscard]] model::MeasurementRecord make_record(const Scenario& scenario, const RecordSpec& spec);

/// Builds the JSON ingest request for a list of records, using names so the
/// scenario definitions are exercised through the same path a client uses.
[[nodiscard]] std::string make_ingest_request(const std::vector<RecordSpec>& specs,
                                              std::int64_t received_at_ns);

/// A half open window in the local reference clock domain.
[[nodiscard]] stats::TimeWindow make_window(std::int64_t from_ns, std::int64_t to_ns);

/// A summary request for the scenario path covering [from, to).
[[nodiscard]] stats::SummaryRequest make_summary_request(const Scenario& scenario,
                                                         std::int64_t from_ns, std::int64_t to_ns,
                                                         stats::AggregationMode mode);

/// The same request expressed with explicit identities, for runtimes that were
/// reopened from a store rather than built by build_scenario.
[[nodiscard]] stats::SummaryRequest make_summary_request(PathId path, GenerationId generation,
                                                         std::int64_t from_ns, std::int64_t to_ns,
                                                         stats::AggregationMode mode);

/// An attribution request for the scenario path covering [from, to).
[[nodiscard]] attribute::AttributionRequest make_attribution_request(
    const Scenario& scenario, std::int64_t from_ns, std::int64_t to_ns,
    stats::AggregationMode mode);

/// Builds a clock synchronization record for the scenario edge domain.
[[nodiscard]] model::ClockSync make_edge_sync(const Scenario& scenario, std::int64_t observed_at_ns,
                                              Nanos uncertainty_ns, Nanos valid_for_ns,
                                              model::ClockSyncState state,
                                              std::uint32_t revision = 1);

/// A very small deterministic generator (SplitMix64) used by the property and
/// randomized tests. It is seeded, so every failure is reproducible.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  [[nodiscard]] std::uint64_t next();
  [[nodiscard]] std::uint64_t range(std::uint64_t low, std::uint64_t high);
  [[nodiscard]] std::int64_t signed_range(std::int64_t low, std::int64_t high);

 private:
  std::uint64_t state_;
};

/// Runs the command line tool in a separate process and captures its output.
/// The handshake is a pipe read, never a sleep or a timeout.
struct ChildResult {
  int exit_code = -1;
  std::string output;
};

[[nodiscard]] ChildResult run_child(const std::string& executable,
                                    const std::vector<std::string>& arguments);

/// A child process whose standard output is piped. Reading a line blocks until
/// the child writes it: the startup handshake of the transport tests is a pipe
/// read, never a sleep, a poll or a timeout.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ~ChildProcess();

  [[nodiscard]] static ChildProcess spawn(const std::string& executable,
                                          const std::vector<std::string>& arguments);
  /// Reads one line without its terminator. An empty string means end of stream.
  [[nodiscard]] std::string read_line();
  [[nodiscard]] std::string read_to_end();
  /// Waits for the process to exit and returns its exit code.
  int wait();
  [[nodiscard]] bool valid() const noexcept { return handle_valid_; }

 private:
  void close_read_end();
  void close_write_end();

  void* process_handle_ = nullptr;  // HANDLE on Windows, unused on POSIX
  void* read_end_ = nullptr;
  void* write_end_ = nullptr;
  long long pid_ = -1;
  bool handle_valid_ = false;
  bool waited_ = false;
  std::string pending_;
};

/// Path of the latobs executable under test. Provided by the build system.
[[nodiscard]] const std::string& tool_path();

}  // namespace latobs::test
