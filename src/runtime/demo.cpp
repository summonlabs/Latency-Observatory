// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The synthetic scenario. Every number produced here is generated from a seeded
// deterministic generator inside this process: nothing in this file observes a
// real network, switch or host.

#include <cstdio>
#include <string>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/runtime/engine.hpp"
#include "latency_observatory/runtime/service.hpp"

namespace latobs::runtime {
namespace {

constexpr std::uint64_t kMixConstant = 0x9E3779B97F4A7C15ULL;

/// SplitMix64: small, deterministic and dependency free.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  [[nodiscard]] std::uint64_t next() {
    state_ += kMixConstant;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
  }
  [[nodiscard]] std::uint64_t range(std::uint64_t low, std::uint64_t high) {
    if (high <= low) return low;
    return low + (next() % (high - low + 1));
  }

 private:
  std::uint64_t state_;
};

constexpr const char* kReferenceDomain = "lobs.clock.reference.utc";
constexpr const char* kRemoteDomain = "demo.clock.edge";
constexpr const char* kGenerationName = "demo.generation.one";
constexpr const char* kSourceName = "demo.source.synthetic";
constexpr const char* kPathName = "demo.path.alpha";

}  // namespace

std::string build_demo_request(std::uint64_t seed, std::size_t samples) {
  Rng rng(seed);
  std::string text;
  core::JsonWriter writer(text);
  writer.begin_object();
  writer.field("op", "ingest");
  writer.field("received_at_ns", static_cast<std::int64_t>(1000000000000LL));
  writer.field_array("records");
  const std::int64_t base = 900000000000LL;
  for (std::size_t index = 0; index < samples; ++index) {
    const std::int64_t observed = base + static_cast<std::int64_t>(index) * 1000000LL;
    const std::int64_t hop_one = static_cast<std::int64_t>(rng.range(1000, 60000));
    const std::int64_t hop_two = static_cast<std::int64_t>(rng.range(2000, 90000));
    const std::int64_t hop_three = static_cast<std::int64_t>(rng.range(3000, 400000));
    const std::int64_t rtt = hop_one + hop_two + hop_three;
    writer.begin_object();
    writer.field("path", kPathName);
    writer.field("generation", kGenerationName);
    writer.field("source", kSourceName);
    writer.field("epoch", "demo.epoch.one");
    writer.field("incarnation", "demo.incarnation.one");
    writer.field("source_revision", static_cast<std::uint64_t>(1));
    writer.field("sequence", static_cast<std::uint64_t>(index));
    writer.field("domain", kReferenceDomain);
    writer.field("request_ns", observed);
    writer.field("response_ns", observed + rtt);
    writer.field("observed_at_ns", observed + rtt);
    writer.field("observed_at_domain", kReferenceDomain);
    writer.field("synthetic", static_cast<std::int64_t>(1));
    writer.field_array("hops");
    const std::int64_t durations[3] = {hop_one, hop_two, hop_three};
    const char* hops[3] = {"demo.hop.client", "demo.hop.link", "demo.hop.queue"};
    std::int64_t cursor = observed;
    for (int hop = 0; hop < 3; ++hop) {
      writer.begin_object();
      writer.field("index", static_cast<std::uint64_t>(hop));
      writer.field("hop", hops[hop]);
      writer.field("entry_ns", cursor);
      writer.field("entry_domain", kReferenceDomain);
      writer.field("exit_ns", cursor + durations[hop]);
      writer.field("exit_domain", kReferenceDomain);
      if (hop == 2) {
        const std::int64_t queue_entry = cursor + 100;
        writer.field("queue_entry_ns", queue_entry);
        writer.field("queue_exit_ns", queue_entry + durations[hop] - 200);
        writer.field("queue", "demo.queue.egress");
      }
      writer.end_object();
      cursor += durations[hop];
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return text;
}

int run_demo(const std::string& store_directory, std::size_t samples, std::uint64_t seed,
             bool pretty) {
  RuntimeConfig config;
  if (!store_directory.empty()) config.store_directory = store_directory;
  Result<std::unique_ptr<Service>> created = Service::create(config);
  if (!created.has_value()) {
    std::fprintf(stderr, "latobs demo: %s\n", created.error().describe().c_str());
    return 1;
  }
  std::unique_ptr<Service> service = std::move(created.value());
  const char* setup[] = {
      R"({"op":"define_source","name":"demo.source.synthetic","source_kind":"synthetic","authority":"synthetic","semantics":"end_to_end_request_response|hop_dwell","revision":1,"description":"in-process synthetic generator"})",
      R"({"op":"define_generation","name":"demo.generation.one","revision":1,"description":"synthetic generation"})",
      R"({"op":"define_clock_domain","name":"lobs.clock.reference.utc","is_reference":true,"description":"local reference"})",
      R"({"op":"define_endpoint","name":"demo.endpoint.client","role":"client","revision":1})",
      R"({"op":"define_endpoint","name":"demo.endpoint.server","role":"server","revision":1})",
      R"({"op":"define_link","name":"demo.link.alpha","from":"demo.endpoint.client","to":"demo.endpoint.server","revision":1})",
      R"({"op":"define_queue","name":"demo.queue.egress","link":"demo.link.alpha","semantics":"queue_dwell","revision":1})",
      R"({"op":"define_hop","name":"demo.hop.client","hop_kind":"endpoint","revision":1})",
      R"({"op":"define_hop","name":"demo.hop.link","hop_kind":"link","link":"demo.link.alpha","revision":1})",
      R"({"op":"define_hop","name":"demo.hop.queue","hop_kind":"queueing_stage","link":"demo.link.alpha","queue":"demo.queue.egress","revision":1})",
      R"({"op":"define_path","name":"demo.path.alpha","generation":"demo.generation.one","revision":1,"hops":["demo.hop.client","demo.hop.link","demo.hop.queue"]})",
  };
  for (const char* request : setup) {
    const std::string response = service->execute(request);
    if (response.find("\"ok\":true") == std::string::npos) {
      std::fprintf(stderr, "latobs demo: setup failed: %s\n", response.c_str());
      const Status shutdown = service->shutdown();
      (void)shutdown;
      return 1;
    }
  }

  const std::string ingest = build_demo_request(seed, samples);
  const std::string ingest_response = service->execute(ingest);
  if (ingest_response.find("\"ok\":true") == std::string::npos) {
    std::fprintf(stderr, "latobs demo: ingest failed: %s\n", ingest_response.c_str());
    const Status shutdown = service->shutdown();
    (void)shutdown;
    return 1;
  }

  const char* queries[] = {
      R"({"op":"summarize","path":"demo.path.alpha","generation":"demo.generation.one","mode":"historical","from_ns":900000000000,"to_ns":9000000000000})",
      R"({"op":"attribute","path":"demo.path.alpha","generation":"demo.generation.one","mode":"historical","from_ns":900000000000,"to_ns":9000000000000})",
      R"({"op":"history","path":"demo.path.alpha","generation":"demo.generation.one","mode":"historical","from_ns":900000000000,"to_ns":9000000000000,"bucket_ns":60000000000})",
      R"({"op":"baseline_create","name":"demo.baseline.one","path":"demo.path.alpha","generation":"demo.generation.one","mode":"historical","from_ns":900000000000,"to_ns":9000000000000})",
      R"({"op":"explain","path":"demo.path.alpha","generation":"demo.generation.one","mode":"historical","from_ns":900000000000,"to_ns":9000000000000,"format":"text"})",
      R"({"op":"export","kind":"samples","format":"csv","limit":8})",
  };
  std::printf("SYNTHETIC\n");
  std::printf("scenario: %zu generated exchanges, seed %llu, source=in-process generator\n",
              samples, static_cast<unsigned long long>(seed));
  std::printf("no switch, ASIC, RDMA, InfiniBand or NVLink telemetry is involved\n\n");
  for (const char* request : queries) {
    std::printf(">>> %s\n", request);
    std::printf("%s\n\n", service->execute(request).c_str());
  }
  (void)pretty;
  const Status shutdown = service->shutdown();
  if (!shutdown.ok()) {
    std::fprintf(stderr, "latobs demo: %s\n", shutdown.error().describe().c_str());
    return 1;
  }
  return 0;
}

}  // namespace latobs::runtime
