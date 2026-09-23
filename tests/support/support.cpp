// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "support.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/model/codec.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace latobs::test {
namespace {

/// Moves a successful result into an existing field, propagating the error.
template <class Target, class Value>
Status assign(Target& target, Result<Value> value) {
  if (!value) return value.error();
  target = std::move(value.value());
  return core::ok_status();
}

/// Discards a successful value, propagating the error.
template <class Value>
Status assign_any(Result<Value> value) {
  if (!value) return value.error();
  return core::ok_status();
}

std::string unique_suffix() {
  static std::uint64_t counter = 0;
  const std::uint64_t now = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return std::to_string(now) + "-" + std::to_string(counter++);
}

}  // namespace

TempDir::TempDir(std::string_view label) {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  path_ = base / ("latobs-test-" + std::string(label) + "-" + unique_suffix());
  std::filesystem::create_directories(path_, error);
  if (error) {
    fail(__FILE__, __LINE__, "cannot create a temporary directory: " + error.message());
  }
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::size_t Scenario::generation_count() const {
  return engine == nullptr ? 0 : engine->catalog().generation_count();
}

Result<Scenario> build_scenario(const ScenarioOptions& options) {
  runtime::RuntimeConfig config;
  config.policy = options.policy;
  config.store_directory = options.store_directory;
  LATOBS_TRY(engine, runtime::Engine::create(config));

  Scenario scenario;
  scenario.engine = std::move(engine);
  scenario.reference_domain = core::reference_clock_domain();

  model::ClockDomainDef reference;
  LATOBS_TRY_STATUS(assign(reference.name, Name::parse("lobs.clock.reference.utc")));
  reference.is_reference = true;
  LATOBS_TRY_STATUS(assign_any(scenario.engine->define_clock_domain(reference)));

  if (options.define_edge_domain) {
    model::ClockDomainDef edge;
    LATOBS_TRY_STATUS(assign(edge.name, Name::parse("test.clock.edge")));
    LATOBS_TRY_STATUS(assign_any(scenario.engine->define_clock_domain(edge)));
    scenario.edge_domain = ClockDomainId::derive_from(edge.name.view());
  } else {
    scenario.edge_domain = ClockDomainId::derive_from("test.clock.edge");
  }

  model::SourceDescriptor source;
  LATOBS_TRY_STATUS(assign(source.name, Name::parse("test.source.probe")));
  source.kind = model::SourceKind::Probe;
  source.authority = model::AuthorityClass::Primary;
  source.semantics = model::SemanticsProfile::EndToEndRequestResponse;
  if (options.declare_hop_semantics) {
    source.semantics = source.semantics | model::SemanticsProfile::HopDwell;
  }
  LATOBS_TRY_STATUS(assign(source.revision, Revision::from_value(1)));
  source.description = "deterministic test source";
  CHECK_OK(defined_source, scenario.engine->define_source(std::move(source)));
  scenario.source = defined_source;

  model::GenerationDef generation;
  LATOBS_TRY_STATUS(assign(generation.name, Name::parse("test.generation.one")));
  LATOBS_TRY_STATUS(assign(generation.revision, Revision::from_value(1)));
  CHECK_OK(defined_generation, scenario.engine->define_generation(std::move(generation)));
  scenario.generation = defined_generation;

  model::EndpointDef client;
  LATOBS_TRY_STATUS(assign(client.name, Name::parse("test.endpoint.client")));
  client.role = model::EndpointRole::Client;
  LATOBS_TRY_STATUS(assign(client.revision, Revision::from_value(1)));
  CHECK_OK(defined_client, scenario.engine->define_endpoint(std::move(client)));

  model::EndpointDef server;
  LATOBS_TRY_STATUS(assign(server.name, Name::parse("test.endpoint.server")));
  server.role = model::EndpointRole::Server;
  LATOBS_TRY_STATUS(assign(server.revision, Revision::from_value(1)));
  CHECK_OK(defined_server, scenario.engine->define_endpoint(std::move(server)));

  model::LinkDef link;
  LATOBS_TRY_STATUS(assign(link.name, Name::parse("test.link.alpha")));
  link.from = defined_client;
  link.to = defined_server;
  LATOBS_TRY_STATUS(assign(link.revision, Revision::from_value(1)));
  CHECK_OK(defined_link, scenario.engine->define_link(std::move(link)));
  scenario.link = defined_link;

  model::QueueDef queue;
  LATOBS_TRY_STATUS(assign(queue.name, Name::parse("test.queue.egress")));
  queue.link = defined_link;
  queue.semantics = options.declare_queue_semantics ? model::SemanticsProfile::QueueDwell
                                                    : model::SemanticsProfile::Unknown;
  LATOBS_TRY_STATUS(assign(queue.revision, Revision::from_value(1)));
  CHECK_OK(defined_queue, scenario.engine->define_queue(std::move(queue)));
  scenario.queue = defined_queue;

  model::HopDef client_hop;
  LATOBS_TRY_STATUS(assign(client_hop.name, Name::parse("test.hop.client")));
  client_hop.kind = model::HopKind::Endpoint;
  LATOBS_TRY_STATUS(assign(client_hop.revision, Revision::from_value(1)));
  CHECK_OK(defined_client_hop, scenario.engine->define_hop(std::move(client_hop)));
  scenario.client_hop = defined_client_hop;

  model::HopDef link_hop;
  LATOBS_TRY_STATUS(assign(link_hop.name, Name::parse("test.hop.link")));
  link_hop.kind = model::HopKind::Link;
  link_hop.link = defined_link;
  LATOBS_TRY_STATUS(assign(link_hop.revision, Revision::from_value(1)));
  CHECK_OK(defined_link_hop, scenario.engine->define_hop(std::move(link_hop)));
  scenario.link_hop = defined_link_hop;

  model::HopDef queue_hop;
  LATOBS_TRY_STATUS(assign(queue_hop.name, Name::parse("test.hop.queue")));
  queue_hop.kind = model::HopKind::QueueingStage;
  queue_hop.link = defined_link;
  queue_hop.queue = defined_queue;
  LATOBS_TRY_STATUS(assign(queue_hop.revision, Revision::from_value(1)));
  CHECK_OK(defined_queue_hop, scenario.engine->define_hop(std::move(queue_hop)));
  scenario.queue_hop = defined_queue_hop;

  model::PathDef path;
  LATOBS_TRY_STATUS(assign(path.name, Name::parse("test.path.alpha")));
  path.generation = scenario.generation;
  LATOBS_TRY_STATUS(assign(path.revision, Revision::from_value(1)));
  path.hops = {scenario.client_hop, scenario.link_hop, scenario.queue_hop};
  CHECK_OK(defined_path, scenario.engine->define_path(std::move(path)));
  scenario.path = defined_path;
  return scenario;
}

model::MeasurementRecord make_record(const Scenario& scenario, const RecordSpec& spec) {
  model::MeasurementRecord record;
  record.path = scenario.path;
  record.generation = scenario.generation;
  record.source = scenario.source;
  record.epoch = EpochId::derive_from(spec.epoch);
  record.incarnation = IncarnationId::derive_from(spec.incarnation);
  record.source_revision = Revision::from_validated_value(spec.source_revision);
  record.sequence = Sequence::from_value(spec.sequence);
  record.domain = core::reference_clock_domain();
  const std::int64_t request = spec.request_ns != 0 ? spec.request_ns : spec.observed_at_ns;
  std::int64_t total = 0;
  for (const std::int64_t dwell : spec.hop_dwells_ns) {
    if (dwell > 0) total += dwell;
  }
  record.rtt_ns = total + spec.end_to_end_extra_ns;
  record.request = Timestamp{request, record.domain};
  record.response = Timestamp{request + record.rtt_ns, record.domain};
  record.stamp.observed_at = record.response;
  record.stamp.received_at = Timestamp{request + total, record.domain};
  record.stamp.received_mono = MonoTime{0};

  if (!spec.omit_hops) {
    const HopId hops[3] = {scenario.client_hop, scenario.link_hop, scenario.queue_hop};
    std::int64_t cursor = request;
    for (std::uint32_t index = 0; index < 3; ++index) {
      const std::int64_t dwell = spec.hop_dwells_ns[index];
      if (dwell < 0) {
        // The source did not report this hop at all.
        cursor += 0;
        continue;
      }
      model::HopObservation observation;
      observation.index = HopIndex::from_validated_value(index);
      observation.hop = hops[index];
      observation.link = scenario.link;
      const bool cross = spec.cross_domain_hop && index == 1;
      observation.entry = Timestamp{cursor, cross ? record.domain : record.domain};
      observation.exit =
          Timestamp{cursor + dwell, cross ? scenario.edge_domain : record.domain};
      observation.entry_domain = record.domain;
      observation.exit_domain = cross ? scenario.edge_domain : record.domain;
      if (index == 2) {
        observation.queue = scenario.queue;
        observation.queue_entry = Timestamp{cursor + 10, record.domain};
        observation.queue_exit = Timestamp{cursor + dwell - 10, record.domain};
      }
      record.hops.push_back(observation);
      cursor += dwell;
    }
  }
  return record;
}

std::string make_ingest_request(const std::vector<RecordSpec>& specs, std::int64_t received_at_ns) {
  std::string text;
  core::JsonWriter writer(text);
  writer.begin_object();
  writer.field("op", "ingest");
  writer.field("received_at_ns", received_at_ns);
  writer.field_array("records");
  for (const RecordSpec& spec : specs) {
    writer.begin_object();
    writer.field("path", spec.path_name.empty() ? "test.path.alpha" : spec.path_name);
    writer.field("generation",
                 spec.generation_name.empty() ? "test.generation.one" : spec.generation_name);
    writer.field("source", "test.source.probe");
    writer.field("epoch", spec.epoch);
    writer.field("incarnation", spec.incarnation);
    writer.field("source_revision", static_cast<std::uint64_t>(spec.source_revision));
    writer.field("sequence", spec.sequence);
    writer.field("domain", spec.domain_name.empty() ? "lobs.clock.reference.utc"
                                                    : spec.domain_name);
    std::int64_t total = 0;
    for (const std::int64_t dwell : spec.hop_dwells_ns) {
      if (dwell > 0) total += dwell;
    }
    const std::int64_t request = spec.request_ns != 0 ? spec.request_ns : spec.observed_at_ns;
    const std::int64_t rtt = total + spec.end_to_end_extra_ns;
    writer.field("request_ns", request);
    writer.field("response_ns", request + rtt);
    writer.field("rtt_ns", rtt);
    writer.field("observed_at_ns", request + rtt);
    writer.field("observed_at_domain", "lobs.clock.reference.utc");
    writer.field("synthetic", static_cast<std::int64_t>(1));
    if (!spec.omit_hops) {
      writer.field_array("hops");
      const char* hop_names[3] = {"test.hop.client", "test.hop.link", "test.hop.queue"};
      std::int64_t cursor = request;
      for (std::uint32_t index = 0; index < 3; ++index) {
        const std::int64_t dwell = spec.hop_dwells_ns[index];
        if (dwell < 0) continue;
        writer.begin_object();
        writer.field("index", static_cast<std::uint64_t>(index));
        writer.field("hop", hop_names[index]);
        writer.field("link", "test.link.alpha");
        writer.field("entry_ns", cursor);
        writer.field("entry_domain", "lobs.clock.reference.utc");
        writer.field("exit_ns", cursor + dwell);
        writer.field("exit_domain", "lobs.clock.reference.utc");
        if (index == 2) {
          writer.field("queue", "test.queue.egress");
          writer.field("queue_entry_ns", cursor + 10);
          writer.field("queue_exit_ns", cursor + dwell - 10);
        }
        writer.end_object();
        cursor += dwell;
      }
      writer.end_array();
    }
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return text;
}

stats::TimeWindow make_window(std::int64_t from_ns, std::int64_t to_ns) {
  const Result<stats::TimeWindow> window = stats::TimeWindow::make(
      Timestamp{from_ns, core::reference_clock_domain()},
      Timestamp{to_ns, core::reference_clock_domain()});
  CHECK(window.has_value());
  return window.value();
}

stats::SummaryRequest make_summary_request(const Scenario& scenario, std::int64_t from_ns,
                                           std::int64_t to_ns, stats::AggregationMode mode) {
  stats::SummaryRequest request;
  request.path = scenario.path;
  request.generation = scenario.generation;
  request.mode = mode;
  request.window = make_window(from_ns, to_ns);
  return request;
}

stats::SummaryRequest make_summary_request(PathId path, GenerationId generation,
                                           std::int64_t from_ns, std::int64_t to_ns,
                                           stats::AggregationMode mode) {
  stats::SummaryRequest request;
  request.path = path;
  request.generation = generation;
  request.mode = mode;
  request.window = make_window(from_ns, to_ns);
  return request;
}

attribute::AttributionRequest make_attribution_request(const Scenario& scenario,
                                                       std::int64_t from_ns, std::int64_t to_ns,
                                                       stats::AggregationMode mode) {
  attribute::AttributionRequest request;
  request.path = scenario.path;
  request.generation = scenario.generation;
  request.mode = mode;
  request.window = make_window(from_ns, to_ns);
  return request;
}

model::ClockSync make_edge_sync(const Scenario& scenario, std::int64_t observed_at_ns,
                                Nanos uncertainty_ns, Nanos valid_for_ns,
                                model::ClockSyncState state, std::uint32_t revision) {
  model::ClockSync sync;
  sync.domain = scenario.edge_domain;
  sync.reference = core::reference_clock_domain();
  sync.state = state;
  sync.offset_ns = 0;
  sync.skew_ppb = 0;
  sync.uncertainty_ns = uncertainty_ns;
  sync.valid_for_ns = valid_for_ns;
  sync.observed_at = Timestamp{observed_at_ns, core::reference_clock_domain()};
  sync.generation = scenario.generation;
  sync.epoch = EpochId::derive_from("epoch.one");
  sync.incarnation = IncarnationId::derive_from("incarnation.one");
  sync.revision = Revision::from_validated_value(revision);
  return sync;
}

std::uint64_t Rng::next() {
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

std::uint64_t Rng::range(std::uint64_t low, std::uint64_t high) {
  if (high <= low) return low;
  return low + (next() % (high - low + 1));
}

std::int64_t Rng::signed_range(std::int64_t low, std::int64_t high) {
  return static_cast<std::int64_t>(range(static_cast<std::uint64_t>(low),
                                         static_cast<std::uint64_t>(high)));
}

#if defined(_WIN32)

ChildProcess::ChildProcess(ChildProcess&& other) noexcept { *this = std::move(other); }

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this == &other) return *this;
  close_read_end();
  close_write_end();
  if (process_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(process_handle_));
  process_handle_ = other.process_handle_;
  read_end_ = other.read_end_;
  write_end_ = other.write_end_;
  pid_ = other.pid_;
  handle_valid_ = other.handle_valid_;
  waited_ = other.waited_;
  pending_ = std::move(other.pending_);
  other.process_handle_ = nullptr;
  other.read_end_ = nullptr;
  other.write_end_ = nullptr;
  other.handle_valid_ = false;
  other.waited_ = true;
  return *this;
}

ChildProcess::~ChildProcess() {
  close_read_end();
  close_write_end();
  if (process_handle_ != nullptr && !waited_) {
    TerminateProcess(static_cast<HANDLE>(process_handle_), 1);
    WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE);
  }
  if (process_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(process_handle_));
}

void ChildProcess::close_read_end() {
  if (read_end_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(read_end_));
    read_end_ = nullptr;
  }
}

void ChildProcess::close_write_end() {
  if (write_end_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(write_end_));
    write_end_ = nullptr;
  }
}

ChildProcess ChildProcess::spawn(const std::string& executable,
                                 const std::vector<std::string>& arguments) {
  ChildProcess child;
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &attributes, 0)) {
    fail(__FILE__, __LINE__, "CreatePipe failed");
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  // Windows command line quoting: every argument is wrapped in double quotes
  // and embedded quotes are escaped, so a JSON document survives the boundary.
  auto quote = [](const std::string& argument) {
    std::string quoted = "\"";
    std::size_t backslashes = 0;
    for (const char c : argument) {
      if (c == '\\') {
        ++backslashes;
        quoted.push_back(c);
        continue;
      }
      if (c == '"') {
        quoted.append(backslashes, '\\');
        quoted.append("\\\"");
        backslashes = 0;
        continue;
      }
      backslashes = 0;
      quoted.push_back(c);
    }
    quoted.append(backslashes, '\\');
    quoted.push_back('"');
    return quoted;
  };

  std::string command = quote(executable);
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote(argument));
  }
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &startup, &process);
  CloseHandle(write_end);
  if (!created) {
    CloseHandle(read_end);
    fail(__FILE__, __LINE__, "CreateProcess failed for " + executable);
  }
  CloseHandle(process.hThread);
  child.process_handle_ = process.hProcess;
  child.read_end_ = read_end;
  child.handle_valid_ = true;
  return child;
}

std::string ChildProcess::read_line() {
  if (read_end_ == nullptr) return std::string();
  while (true) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line;
    }
    char buffer[4096];
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(read_end_), buffer, sizeof(buffer), &read, nullptr) ||
        read == 0) {
      close_read_end();
      std::string line = pending_;
      pending_.clear();
      return line;
    }
    pending_.append(buffer, read);
  }
}

std::string ChildProcess::read_to_end() {
  std::string output = pending_;
  pending_.clear();
  if (read_end_ == nullptr) return output;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(static_cast<HANDLE>(read_end_), buffer, sizeof(buffer), &read, nullptr) &&
         read > 0) {
    output.append(buffer, read);
  }
  close_read_end();
  return output;
}

int ChildProcess::wait() {
  if (process_handle_ == nullptr) return -1;
  WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &exit_code);
  waited_ = true;
  return static_cast<int>(exit_code);
}

#else

ChildProcess::ChildProcess(ChildProcess&& other) noexcept { *this = std::move(other); }

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this == &other) return *this;
  close_read_end();
  close_write_end();
  process_handle_ = other.process_handle_;
  read_end_ = other.read_end_;
  write_end_ = other.write_end_;
  pid_ = other.pid_;
  handle_valid_ = other.handle_valid_;
  waited_ = other.waited_;
  pending_ = std::move(other.pending_);
  other.process_handle_ = nullptr;
  other.read_end_ = nullptr;
  other.write_end_ = nullptr;
  other.handle_valid_ = false;
  other.waited_ = true;
  return *this;
}

ChildProcess::~ChildProcess() {
  close_read_end();
  close_write_end();
  if (pid_ > 0 && !waited_) {
    kill(static_cast<pid_t>(pid_), SIGKILL);
    int status = 0;
    waitpid(static_cast<pid_t>(pid_), &status, 0);
  }
}

void ChildProcess::close_read_end() {
  if (read_end_ != nullptr) {
    close(static_cast<int>(reinterpret_cast<intptr_t>(read_end_)));
    read_end_ = nullptr;
  }
}

void ChildProcess::close_write_end() {
  if (write_end_ != nullptr) {
    close(static_cast<int>(reinterpret_cast<intptr_t>(write_end_)));
    write_end_ = nullptr;
  }
}

ChildProcess ChildProcess::spawn(const std::string& executable,
                                 const std::vector<std::string>& arguments) {
  ChildProcess child;
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    fail(__FILE__, __LINE__, "pipe failed");
  }
  const pid_t pid = fork();
  if (pid == 0) {
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    execv(executable.c_str(), argv.data());
    _exit(127);
  }
  close(pipe_fds[1]);
  child.pid_ = static_cast<long long>(pid);
  child.read_end_ = reinterpret_cast<void*>(static_cast<intptr_t>(pipe_fds[0]));
  child.handle_valid_ = true;
  return child;
}

std::string ChildProcess::read_line() {
  if (read_end_ == nullptr) return std::string();
  const int descriptor = static_cast<int>(reinterpret_cast<intptr_t>(read_end_));
  while (true) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line;
    }
    char buffer[4096];
    const ssize_t read = ::read(descriptor, buffer, sizeof(buffer));
    if (read <= 0) {
      close_read_end();
      std::string line = pending_;
      pending_.clear();
      return line;
    }
    pending_.append(buffer, static_cast<std::size_t>(read));
  }
}

std::string ChildProcess::read_to_end() {
  std::string output = pending_;
  pending_.clear();
  if (read_end_ == nullptr) return output;
  const int descriptor = static_cast<int>(reinterpret_cast<intptr_t>(read_end_));
  char buffer[4096];
  ssize_t read = 0;
  while ((read = ::read(descriptor, buffer, sizeof(buffer))) > 0) {
    output.append(buffer, static_cast<std::size_t>(read));
  }
  close_read_end();
  return output;
}

int ChildProcess::wait() {
  if (pid_ <= 0) return -1;
  int status = 0;
  waitpid(static_cast<pid_t>(pid_), &status, 0);
  waited_ = true;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

ChildResult run_child(const std::string& executable, const std::vector<std::string>& arguments) {
  ChildProcess child = ChildProcess::spawn(executable, arguments);
  ChildResult result;
  result.output = child.read_to_end();
  result.exit_code = child.wait();
  return result;
}


const std::string& tool_path() {
  static const std::string path = LATOBS_TOOL_PATH;
  return path;
}

}  // namespace latobs::test
