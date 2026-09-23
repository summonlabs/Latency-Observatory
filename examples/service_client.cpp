// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Drives the runtime through the same request/response protocol the network
// transport uses. This is the interface a fabric controller would embed.

#include <cstdio>
#include <string>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/runtime/service.hpp"

using namespace latobs;
using namespace latobs::core;

int main() {
  runtime::RuntimeConfig config;
  core::Result<std::unique_ptr<runtime::Service>> created =
      runtime::Service::create(config);
  if (!created.has_value()) {
    std::fprintf(stderr, "cannot create the service: %s\n",
                 created.error().describe().c_str());
    return 1;
  }
  std::unique_ptr<runtime::Service> service = std::move(created.value());

  const char* requests[] = {
      R"({"op":"define_source","name":"cli.source","source_kind":"probe","authority":"primary","semantics":"end_to_end_request_response|hop_dwell","revision":1})",
      R"({"op":"define_generation","name":"cli.generation","revision":1})",
      R"({"op":"define_clock_domain","name":"lobs.clock.reference.utc","is_reference":true})",
      R"({"op":"define_hop","name":"cli.hop","hop_kind":"endpoint","revision":1})",
      R"({"op":"define_path","name":"cli.path","generation":"cli.generation","revision":1,"hops":["cli.hop"]})",
      R"({"op":"ingest","received_at_ns":2000000,"records":[{"path":"cli.path","generation":"cli.generation","source":"cli.source","epoch":"e","incarnation":"i","source_revision":1,"sequence":0,"domain":"lobs.clock.reference.utc","request_ns":1000000,"response_ns":1000500,"rtt_ns":500,"observed_at_ns":1000500,"observed_at_domain":"lobs.clock.reference.utc","hops":[{"index":0,"hop":"cli.hop","entry_ns":1000000,"entry_domain":"lobs.clock.reference.utc","exit_ns":1000500,"exit_domain":"lobs.clock.reference.utc"}]}]})",
      R"({"op":"summarize","path":"cli.path","generation":"cli.generation","mode":"current","from_ns":0,"to_ns":100000000})",
      R"({"op":"explain","path":"cli.path","generation":"cli.generation","from_ns":0,"to_ns":100000000,"format":"text"})",
  };

  for (const char* request : requests) {
    const std::string response = service->execute(request);
    std::printf(">>> %s\n%s\n\n", request, response.c_str());
    if (response.find("\"ok\":true") == std::string::npos) {
      std::fprintf(stderr, "request failed\n");
      const core::Status shutdown = service->shutdown();
      (void)shutdown;
      return 1;
    }
  }
  const core::Status shutdown = service->shutdown();
  return shutdown.ok() ? 0 : 1;
}
