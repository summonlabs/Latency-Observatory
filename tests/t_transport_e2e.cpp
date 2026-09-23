// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Transport proof. The runtime claims a network ingest transport, so the tests
// exercise it over real sockets, and the decisive case runs three independent
// processes: a server, a client started by this test, and a second client
// process started by the tool itself.

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "latency_observatory/runtime/service.hpp"
#include "latency_observatory/runtime/transport.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

std::string write_request_file(const TempDir& directory, const std::string& name,
                               const std::string& content) {
  const std::filesystem::path path = directory.path() / name;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << content;
  stream.close();
  return path.string();
}

std::uint16_t parse_port(const std::string& line) {
  const std::size_t position = line.find("port=");
  if (position == std::string::npos) {
    fail(__FILE__, __LINE__, "the server did not announce a port: " + line);
  }
  const std::optional<std::uint64_t> port =
      core::parse_u64(std::string_view(line).substr(position + 5));
  if (!port.has_value() || *port == 0 || *port > 65535) {
    fail(__FILE__, __LINE__, "the announced port is not usable: " + line);
  }
  return static_cast<std::uint16_t>(*port);
}

bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

LATOBS_TEST(transport, serves_requests_over_a_real_socket) {
  TempDir directory("transport-socket");
  runtime::RuntimeConfig config;
  config.store_directory = directory.path();
  CHECK_OK(service, runtime::Service::create(config));

  runtime::TransportConfig transport_config;
  transport_config.port = 0;
  transport_config.max_connections = 4;
  transport_config.max_line_bytes = 64 * 1024;
  CHECK_OK(server, runtime::LineServer::start(*service, transport_config));
  CHECK(server->port() != 0);

  std::thread server_thread([&server]() {
    const Status status = server->run();
    CHECK_OK_STATUS(status);
  });

  // A real TCP connection from this process.
  CHECK_OK(response_one, runtime::send_request("127.0.0.1", server->port(),
                                               R"({"op":"capabilities"})", 1024 * 1024));
  CHECK(contains(response_one, "\"ok\":true"));
  CHECK(contains(response_one, "\"op\":\"capabilities\""));

  // Malformed input is answered with a typed error, not a dropped connection.
  CHECK_OK(response_two, runtime::send_request("127.0.0.1", server->port(), "{not json",
                                               1024 * 1024));
  CHECK(contains(response_two, "\"ok\":false"));
  CHECK(contains(response_two, "parse_error"));

  // A request larger than the line limit is refused without killing the server.
  std::string oversized = "{\"op\":\"operations\",\"pad\":\"";
  oversized.append(80 * 1024, 'x');
  oversized.append("\"}");
  const Result<std::string> response_three =
      runtime::send_request("127.0.0.1", server->port(), oversized, 1024 * 1024);
  CHECK(!response_three.has_value() || contains(response_three.value(), "capacity_exceeded"));

  // The server still answers a normal request afterwards.
  CHECK_OK(response_four, runtime::send_request("127.0.0.1", server->port(),
                                                R"({"op":"operations"})", 1024 * 1024));
  CHECK(contains(response_four, "\"ok\":true"));

  CHECK_OK(shutdown_response, runtime::send_request("127.0.0.1", server->port(),
                                                    R"({"op":"shutdown"})", 1024 * 1024));
  CHECK(contains(shutdown_response, "shutting_down"));
  server_thread.join();
  CHECK(server->stats().requests_served >= 4);
  CHECK_OK_STATUS(service->shutdown());
}

LATOBS_TEST(transport, independent_processes_exchange_evidence) {
  TempDir directory("transport-processes");
  const std::string store = directory.text();

  // Process one: the server, started with an ephemeral port. The port is
  // discovered through a pipe read, which blocks until the server announces it.
  ChildProcess server = ChildProcess::spawn(
      tool_path(), {"serve", "--store", store, "--port", "0", "--announce-stdout"});
  CHECK(server.valid());
  const std::string announcement = server.read_line();
  const std::uint16_t port = parse_port(announcement);

  // Process two: a client that defines the scenario through the network.
  const std::vector<std::string> definitions = {
      R"({"op":"define_source","name":"wire.source","source_kind":"probe","authority":"primary","semantics":"end_to_end_request_response|hop_dwell","revision":1})",
      R"({"op":"define_generation","name":"wire.generation","revision":1})",
      R"({"op":"define_clock_domain","name":"lobs.clock.reference.utc","is_reference":true})",
      R"({"op":"define_hop","name":"wire.hop.client","hop_kind":"endpoint","revision":1})",
      R"({"op":"define_path","name":"wire.path","generation":"wire.generation","revision":1,"hops":["wire.hop.client"]})",
  };
  for (std::size_t index = 0; index < definitions.size(); ++index) {
    const std::string file =
        write_request_file(directory, "define-" + std::to_string(index) + ".json",
                           definitions[index]);
    const ChildResult result =
        run_child(tool_path(), {"client", "--port", std::to_string(port), "--request", "@" + file});
    CHECK_EQ(result.exit_code, 0);
    CHECK(contains(result.output, "\"ok\":true"));
  }

  // This test process ingests through the same socket.
  std::string ingest = R"({"op":"ingest","received_at_ns":2000000,"records":[)";
  for (int index = 0; index < 8; ++index) {
    const std::int64_t request = 1000000 + index;
    ingest.append(R"({"path":"wire.path","generation":"wire.generation","source":"wire.source",)");
    ingest.append(R"("epoch":"wire.epoch","incarnation":"wire.incarnation","source_revision":1,)");
    ingest.append("\"sequence\":" + std::to_string(index) + ",");
    ingest.append(R"("domain":"lobs.clock.reference.utc",)");
    ingest.append("\"request_ns\":" + std::to_string(request) + ",");
    ingest.append("\"response_ns\":" + std::to_string(request + 500) + ",");
    ingest.append(R"("observed_at_ns":)" + std::to_string(request + 500) + ",");
    ingest.append(R"("observed_at_domain":"lobs.clock.reference.utc",)");
    ingest.append(R"("hops":[{"index":0,"hop":"wire.hop.client","entry_ns":)");
    ingest.append(std::to_string(request));
    ingest.append(R"(,"entry_domain":"lobs.clock.reference.utc","exit_ns":)");
    ingest.append(std::to_string(request + 500));
    ingest.append(R"(,"exit_domain":"lobs.clock.reference.utc"}]})");
    if (index != 7) ingest.push_back(',');
  }
  ingest.append("]}");
  CHECK_OK(ingest_response, runtime::send_request("127.0.0.1", port, ingest, 1024 * 1024));
  CHECK(contains(ingest_response, "\"accepted_current\":8"));

  // Process two again: a summary computed by a separate client process.
  const std::string summarize_file = write_request_file(
      directory, "summarize.json",
      R"({"op":"summarize","path":"wire.path","generation":"wire.generation","mode":"current","from_ns":0,"to_ns":100000000})");
  const ChildResult summary = run_child(
      tool_path(), {"client", "--port", std::to_string(port), "--request", "@" + summarize_file});
  CHECK_EQ(summary.exit_code, 0);
  CHECK(contains(summary.output, "\"exchanges_included\":8"));
  CHECK(contains(summary.output, "\"mean_ns\":500"));

  // Process three: a client process asks the server to stop.
  const std::string shutdown_file =
      write_request_file(directory, "shutdown.json", R"({"op":"shutdown"})");
  const ChildResult shutdown = run_child(
      tool_path(), {"client", "--port", std::to_string(port), "--request", "@" + shutdown_file});
  CHECK_EQ(shutdown.exit_code, 0);
  CHECK(contains(shutdown.output, "shutting_down"));

  const int exit_code = server.wait();
  CHECK_EQ(exit_code, 0);
  const std::string server_output = server.read_to_end();
  CHECK(contains(server_output, "stopped served="));

  // Process four: a fresh process opens the store the server wrote and reports
  // what it recovered, proving the evidence crossed both the network and the
  // process boundary.
  const ChildResult status = run_child(tool_path(), {"request", "--store", store, "--request",
                                                     R"({"op":"status"})"});
  CHECK_EQ(status.exit_code, 0);
  CHECK(contains(status.output, "\"measurements_stored\":8"));
  CHECK(contains(status.output, "\"truncated\":false"));
}

LATOBS_TEST(transport, client_reports_connection_failures) {
  // Nothing is listening on port 1 for this test; the client must report the
  // failure instead of hanging or succeeding.
  const Result<std::string> response =
      runtime::send_request("127.0.0.1", 1, R"({"op":"status"})", 4096);
  CHECK(!response.has_value());
  CHECK(response.error().code() == ErrorCode::IoError);
}

LATOBS_TEST_MAIN()
