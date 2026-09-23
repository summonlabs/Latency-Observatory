// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The command line tool is a real process: every test here starts it and checks
// its exit code and output.

#include <fstream>
#include <string>
#include <vector>

#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

std::string write_file(const TempDir& directory, const std::string& name,
                       const std::string& content) {
  const std::filesystem::path path = directory.path() / name;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << content;
  stream.close();
  return path.string();
}

bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

LATOBS_TEST(cli, version_and_operations) {
  const ChildResult version = run_child(tool_path(), {"version"});
  CHECK_EQ(version.exit_code, 0);
  CHECK(contains(version.output, "latobs 1.0.0"));

  const ChildResult operations = run_child(tool_path(), {"operations"});
  CHECK_EQ(operations.exit_code, 0);
  CHECK(contains(operations.output, "ingest"));
  CHECK(contains(operations.output, "attribute"));
  CHECK(contains(operations.output, "summarize"));
}

LATOBS_TEST(cli, capabilities_declare_the_boundary) {
  const ChildResult result = run_child(tool_path(), {"capabilities"});
  CHECK_EQ(result.exit_code, 0);
  CHECK(contains(result.output, "\"ok\":true"));
  CHECK(contains(result.output, "\"status\":\"real\""));
  CHECK(contains(result.output, "\"status\":\"unsupported\""));
  CHECK(contains(result.output, "causality inference"));
  CHECK(contains(result.output, "InfiniBand"));
}

LATOBS_TEST(cli, request_accepts_inline_and_file_documents) {
  const ChildResult inline_request =
      run_child(tool_path(), {"request", "--request", R"({"op":"operations"})"});
  CHECK_EQ(inline_request.exit_code, 0);
  CHECK(contains(inline_request.output, "\"ok\":true"));

  TempDir directory("cli-request");
  const std::string path = write_file(directory, "request.json", R"({"op":"capabilities"})");
  const ChildResult file_request =
      run_child(tool_path(), {"request", "--request", "@" + path});
  CHECK_EQ(file_request.exit_code, 0);
  CHECK(contains(file_request.output, "capabilities"));

  const ChildResult file_option =
      run_child(tool_path(), {"request", "--request-file", path});
  CHECK_EQ(file_option.exit_code, 0);
  CHECK(contains(file_option.output, "\"ok\":true"));
}

LATOBS_TEST(cli, failed_operations_return_a_non_zero_exit_code) {
  const ChildResult unknown = run_child(tool_path(), {"request", "--request", R"({"op":"nope"})"});
  CHECK_EQ(unknown.exit_code, 1);
  CHECK(contains(unknown.output, "\"ok\":false"));
  CHECK(contains(unknown.output, "unsupported"));

  const ChildResult malformed =
      run_child(tool_path(), {"request", "--request", R"({"op":)"});
  CHECK_EQ(malformed.exit_code, 1);
  CHECK(contains(malformed.output, "parse_error"));

  const ChildResult missing_argument = run_child(tool_path(), {"request"});
  CHECK_EQ(missing_argument.exit_code, 1);
}

LATOBS_TEST(cli, usage_errors_are_distinguishable) {
  const ChildResult unknown_option =
      run_child(tool_path(), {"request", "--teleport", "yes"});
  CHECK_EQ(unknown_option.exit_code, 1);
  CHECK(contains(unknown_option.output, "unknown option"));

  const ChildResult unknown_command = run_child(tool_path(), {"teleport"});
  CHECK_EQ(unknown_command.exit_code, 2);

  const ChildResult help = run_child(tool_path(), {"--help"});
  CHECK_EQ(help.exit_code, 0);
  CHECK(contains(help.output, "usage: latobs"));
}

LATOBS_TEST(cli, demo_runs_the_synthetic_scenario) {
  TempDir directory("cli-demo");
  const ChildResult result = run_child(
      tool_path(), {"demo", "--samples", "16", "--seed", "7", "--store", directory.text()});
  CHECK_EQ(result.exit_code, 0);
  CHECK(contains(result.output, "SYNTHETIC"));
  CHECK(contains(result.output, "no switch, ASIC, RDMA, InfiniBand or NVLink telemetry"));
  CHECK(contains(result.output, "observed_decomposition"));
  CHECK(contains(result.output, "no causal claim"));
  CHECK(contains(result.output, "records_exported"));
}

LATOBS_TEST(cli, store_survives_across_processes) {
  TempDir directory("cli-store");
  const std::string store = directory.text();
  const std::string ingest_path = write_file(
      directory, "ingest.json",
      R"({"op":"ingest","received_at_ns":2000000,"records":[{"path":"cli.path","generation":"cli.generation","source":"cli.source","epoch":"cli.epoch","incarnation":"cli.incarnation","source_revision":1,"sequence":0,"domain":"lobs.clock.reference.utc","request_ns":1000000,"response_ns":1000600,"rtt_ns":600,"observed_at_ns":1000600,"observed_at_domain":"lobs.clock.reference.utc","hops":[{"index":0,"hop":"cli.hop","entry_ns":1000000,"entry_domain":"lobs.clock.reference.utc","exit_ns":1000600,"exit_domain":"lobs.clock.reference.utc"}]}]})");

  const std::vector<std::string> definitions = {
      R"({"op":"define_source","name":"cli.source","source_kind":"probe","authority":"primary","semantics":"end_to_end_request_response|hop_dwell","revision":1})",
      R"({"op":"define_generation","name":"cli.generation","revision":1})",
      R"({"op":"define_clock_domain","name":"lobs.clock.reference.utc","is_reference":true})",
      R"({"op":"define_hop","name":"cli.hop","hop_kind":"endpoint","revision":1})",
      R"({"op":"define_path","name":"cli.path","generation":"cli.generation","revision":1,"hops":["cli.hop"]})",
  };
  for (std::size_t index = 0; index < definitions.size(); ++index) {
    const std::string path =
        write_file(directory, "define-" + std::to_string(index) + ".json", definitions[index]);
    const ChildResult result =
        run_child(tool_path(), {"request", "--store", store, "--request", "@" + path});
    CHECK_EQ(result.exit_code, 0);
  }
  const ChildResult ingest =
      run_child(tool_path(), {"request", "--store", store, "--request", "@" + ingest_path});
  CHECK_EQ(ingest.exit_code, 0);
  CHECK(contains(ingest.output, "\"accepted_current\":1"));

  // A separate process opens the same store and reports what it recovered.
  const ChildResult status = run_child(
      tool_path(), {"request", "--store", store, "--request", R"({"op":"status"})"});
  CHECK_EQ(status.exit_code, 0);
  CHECK(contains(status.output, "\"measurements_stored\":1"));
  CHECK(contains(status.output, "\"records_recovered\":"));

  // The historical summary is still available, but nothing is current after the
  // restart: the evidence is reported as history.
  const std::string summarize_path = write_file(
      directory, "summarize.json",
      R"({"op":"summarize","path":"cli.path","generation":"cli.generation","mode":"current","from_ns":0,"to_ns":100000000})");
  const ChildResult summarize =
      run_child(tool_path(), {"request", "--store", store, "--request", "@" + summarize_path});
  CHECK_EQ(summarize.exit_code, 0);
  CHECK(contains(summarize.output, "\"exchanges_included\":0"));
  CHECK(contains(summarize.output, "\"excluded_stale\":1"));

  const std::string historical_path = write_file(
      directory, "historical.json",
      R"({"op":"summarize","path":"cli.path","generation":"cli.generation","mode":"historical","from_ns":0,"to_ns":100000000})");
  const ChildResult historical =
      run_child(tool_path(), {"request", "--store", store, "--request", "@" + historical_path});
  CHECK_EQ(historical.exit_code, 0);
  CHECK(contains(historical.output, "\"exchanges_included\":1"));
  CHECK(contains(historical.output, "\"mean_ns\":600"));
}

LATOBS_TEST_MAIN()
