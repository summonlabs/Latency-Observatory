// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The request/response protocol: a stable envelope, typed errors, bounded
// payloads and deterministic responses.

#include <string>
#include <vector>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/runtime/service.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

namespace {

struct ServiceFixture {
  TempDir directory{"service"};
  std::unique_ptr<runtime::Service> service;

  ServiceFixture() {
    runtime::RuntimeConfig config;
    config.store_directory = directory.path();
    Result<std::unique_ptr<runtime::Service>> created = runtime::Service::create(config);
    CHECK(created.has_value());
    service = std::move(created.value());
  }

  [[nodiscard]] std::string run(const std::string& request) const {
    return service->execute(request);
  }

  [[nodiscard]] std::string run_ok(const std::string& request) const {
    const std::string response = run(request);
    if (response.find("\"ok\":true") == std::string::npos) {
      fail(__FILE__, __LINE__, "request failed: " + response);
    }
    return response;
  }

  [[nodiscard]] JsonValue parse(const std::string& response) const {
    const Result<JsonValue> document = core::parse_json(response, 64);
    if (!document.has_value()) {
      const std::string excerpt = response.size() > 400 ? response.substr(0, 400) + "..." : response;
      fail(__FILE__, __LINE__, "response is not valid JSON: " + document.error().describe() +
                                   " | " + excerpt);
    }
    return document.value();
  }

  /// Runs a request that must succeed when its response is not inspected.
  void apply(const std::string& request) const { (void)run_ok(request); }

  void define_catalog() const {
    apply(R"({"op":"define_source","name":"protocol.source","source_kind":"probe","authority":"primary","semantics":"end_to_end_request_response|hop_dwell","revision":1})");
    apply(R"({"op":"define_generation","name":"protocol.generation","revision":1})");
    apply(R"({"op":"define_clock_domain","name":"lobs.clock.reference.utc","is_reference":true})");
    apply(R"({"op":"define_endpoint","name":"protocol.endpoint.client","role":"client","revision":1})");
    apply(R"({"op":"define_endpoint","name":"protocol.endpoint.server","role":"server","revision":1})");
    apply(R"({"op":"define_link","name":"protocol.link","from":"protocol.endpoint.client","to":"protocol.endpoint.server","revision":1})");
    apply(R"({"op":"define_queue","name":"protocol.queue","link":"protocol.link","semantics":"queue_dwell","revision":1})");
    apply(R"({"op":"define_hop","name":"protocol.hop.client","hop_kind":"endpoint","revision":1})");
    apply(R"({"op":"define_hop","name":"protocol.hop.link","hop_kind":"link","link":"protocol.link","revision":1})");
    apply(R"({"op":"define_path","name":"protocol.path","generation":"protocol.generation","revision":1,"hops":["protocol.hop.client","protocol.hop.link"]})");
  }
};

std::string ingest_request(std::uint64_t count) {
  std::string text;
  core::JsonWriter writer(text);
  writer.begin_object();
  writer.field("op", "ingest");
  writer.field("received_at_ns", static_cast<std::int64_t>(2000000));
  writer.field_array("records");
  for (std::uint64_t index = 0; index < count; ++index) {
    const std::int64_t request = 1000000 + static_cast<std::int64_t>(index);
    writer.begin_object();
    writer.field("path", "protocol.path");
    writer.field("generation", "protocol.generation");
    writer.field("source", "protocol.source");
    writer.field("epoch", "protocol.epoch");
    writer.field("incarnation", "protocol.incarnation");
    writer.field("source_revision", static_cast<std::uint64_t>(1));
    writer.field("sequence", index);
    writer.field("domain", "lobs.clock.reference.utc");
    writer.field("request_ns", request);
    writer.field("response_ns", request + 600);
    writer.field("rtt_ns", static_cast<std::int64_t>(600));
    writer.field("observed_at_ns", request + 600);
    writer.field("observed_at_domain", "lobs.clock.reference.utc");
    writer.field_array("hops");
    writer.begin_object();
    writer.field("index", static_cast<std::uint64_t>(0));
    writer.field("hop", "protocol.hop.client");
    writer.field("entry_ns", request);
    writer.field("entry_domain", "lobs.clock.reference.utc");
    writer.field("exit_ns", request + 200);
    writer.field("exit_domain", "lobs.clock.reference.utc");
    writer.end_object();
    writer.begin_object();
    writer.field("index", static_cast<std::uint64_t>(1));
    writer.field("hop", "protocol.hop.link");
    writer.field("entry_ns", request + 200);
    writer.field("entry_domain", "lobs.clock.reference.utc");
    writer.field("exit_ns", request + 600);
    writer.field("exit_domain", "lobs.clock.reference.utc");
    writer.end_object();
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return text;
}

}  // namespace

LATOBS_TEST(protocol, envelope_shape_is_stable) {
  ServiceFixture fixture;
  const std::string response = fixture.run_ok(R"({"op":"capabilities"})");
  const JsonValue document = fixture.parse(response);
  CHECK(document.find("ok") != nullptr);
  CHECK(document.find("op") != nullptr);
  CHECK(document.find("policy_digest") != nullptr);
  CHECK(document.find("result") != nullptr);
  CHECK_OK(op, core::json_require_string(document, "op"));
  CHECK_EQ(op, std::string_view("capabilities"));
  CHECK_OK(result, core::json_require_object(document, "result"));
  CHECK_OK(version, core::json_require_string(*result, "version"));
  CHECK_EQ(version, std::string_view("1.0.0"));
  CHECK_OK(entries_value, core::json_require_array(*result, "entries"));
  CHECK_OK(entries, entries_value->as_array());
  std::size_t unsupported = 0;
  std::size_t real = 0;
  for (const JsonValue& entry : *entries) {
    CHECK_OK(status, core::json_require_string(entry, "status"));
    if (status == "unsupported") ++unsupported;
    if (status == "real") ++real;
  }
  CHECK(real > 0);
  // The unsupported surface is declared, not implied.
  CHECK(unsupported >= 4);
  CHECK_OK(digest, core::json_require_string(document, "policy_digest"));
  CHECK_EQ(digest.size(), std::size_t{64});
}

LATOBS_TEST(protocol, typed_errors_are_returned_not_thrown) {
  ServiceFixture fixture;
  // Unknown operation.
  const JsonValue unknown = fixture.parse(fixture.run(R"({"op":"teleport"})"));
  CHECK(!unknown.find("ok")->as_bool().value());
  CHECK_OK(error, core::json_require_object(unknown, "error"));
  CHECK_OK(code, core::json_require_string(*error, "code"));
  CHECK_EQ(code, std::string_view("unsupported"));

  // Malformed document.
  const JsonValue malformed = fixture.parse(fixture.run("{not json"));
  CHECK(!malformed.find("ok")->as_bool().value());
  CHECK_OK(malformed_error, core::json_require_object(malformed, "error"));
  CHECK_OK(malformed_code, core::json_require_string(*malformed_error, "code"));
  CHECK_EQ(malformed_code, std::string_view("parse_error"));

  // Missing required field.
  const JsonValue missing = fixture.parse(fixture.run(R"({"op":"summarize"})"));
  CHECK(!missing.find("ok")->as_bool().value());
  CHECK_OK(missing_error, core::json_require_object(missing, "error"));
  CHECK_OK(missing_code, core::json_require_string(*missing_error, "code"));
  CHECK_EQ(missing_code, std::string_view("not_found"));

  // Wrong field type.
  const JsonValue wrong_type =
      fixture.parse(fixture.run(R"({"op":"summarize","path":17,"generation":"x"})"));
  CHECK(!wrong_type.find("ok")->as_bool().value());

  // Unknown name.
  const JsonValue unknown_name = fixture.parse(
      fixture.run(R"({"op":"summarize","path":"nope","generation":"nope","from_ns":0,"to_ns":10})"));
  CHECK(!unknown_name.find("ok")->as_bool().value());
}

LATOBS_TEST(protocol, request_size_is_bounded) {
  runtime::RuntimeConfig config;
  config.policy.limits.max_json_bytes = 2048;
  config.policy.limits.max_transport_line_bytes = 2048;
  CHECK_OK(service, runtime::Service::create(config));
  std::string large = "{\"op\":\"ingest\",\"records\":[";
  while (large.size() < 4096) large.append("{\"path\":\"x\"},");
  large.append("]}");
  const std::string response = service->execute(large);
  CHECK(response.find("\"ok\":false") != std::string::npos);
  CHECK(response.find("out_of_range") != std::string::npos);
}

LATOBS_TEST(protocol, full_pipeline_through_the_protocol) {
  ServiceFixture fixture;
  fixture.define_catalog();
  CHECK_EQ(fixture.run_ok(ingest_request(8)).find("\"accepted_current\":8") != std::string::npos,
           true);
  const std::string summarize =
      R"({"op":"summarize","path":"protocol.path","generation":"protocol.generation","mode":"current","from_ns":0,"to_ns":100000000})";
  const JsonValue summary = fixture.parse(fixture.run_ok(summarize));
  CHECK_OK(result, core::json_require_object(summary, "result"));
  CHECK_OK(included, core::json_require_uint(*result, "exchanges_included"));
  CHECK_EQ(included, std::uint64_t{8});
  CHECK_OK(end_to_end, core::json_require_object(*result, "end_to_end"));
  CHECK_OK(mean, core::json_require_int(*end_to_end, "mean_ns"));
  CHECK_EQ(mean, 600);

  // The same request produces the same bytes.
  CHECK_EQ(fixture.run_ok(summarize), fixture.run_ok(summarize));

  // A text explanation is returned as a string result.
  const JsonValue explanation = fixture.parse(fixture.run_ok(
      R"({"op":"explain","path":"protocol.path","generation":"protocol.generation","from_ns":0,"to_ns":100000000,"format":"text"})"));
  CHECK_OK(text, core::json_require_string(explanation, "result"));
  CHECK(text.find("subject: path.summary") == 0);
  CHECK(text.find("no causal claim") != std::string::npos);

  const JsonValue json_explanation = fixture.parse(fixture.run_ok(
      R"({"op":"explain","path":"protocol.path","generation":"protocol.generation","from_ns":0,"to_ns":100000000})"));
  CHECK_OK(explanation_result, core::json_require_object(json_explanation, "result"));
  CHECK_OK(notes_value, core::json_require_array(*explanation_result, "notes"));
  CHECK_OK(notes, notes_value->as_array());
  CHECK(!notes->empty());
}

LATOBS_TEST(protocol, history_baseline_attribution_and_export) {
  ServiceFixture fixture;
  fixture.define_catalog();
  fixture.apply(ingest_request(16));

  const JsonValue history = fixture.parse(fixture.run_ok(
      R"({"op":"history","path":"protocol.path","generation":"protocol.generation","mode":"historical","from_ns":0,"to_ns":100000000,"bucket_ns":10000000})"));
  CHECK_OK(history_result, core::json_require_object(history, "result"));
  CHECK_OK(buckets_value, core::json_require_array(*history_result, "buckets"));
  CHECK_OK(buckets, buckets_value->as_array());
  CHECK(!buckets->empty());
  CHECK_OK(with_evidence, core::json_require_uint(*history_result, "buckets_with_evidence"));
  CHECK(with_evidence > 0);

  const JsonValue baseline = fixture.parse(fixture.run_ok(
      R"({"op":"baseline_create","name":"protocol.baseline","path":"protocol.path","generation":"protocol.generation","mode":"current","from_ns":0,"to_ns":100000000})"));
  CHECK_OK(baseline_result, core::json_require_object(baseline, "result"));
  CHECK_OK(baseline_id, core::json_require_string(*baseline_result, "baseline"));

  const std::string attribute_request = std::string(
      R"({"op":"attribute","path":"protocol.path","generation":"protocol.generation","mode":"current","from_ns":0,"to_ns":100000000,"baseline":")") +
      std::string(baseline_id) + "\"}";
  const JsonValue attribution = fixture.parse(fixture.run_ok(attribute_request));
  CHECK_OK(attribution_result, core::json_require_object(attribution, "result"));
  CHECK_OK(kind, core::json_require_string(*attribution_result, "kind"));
  CHECK_EQ(kind, std::string_view("observed_decomposition"));

  const JsonValue listed = fixture.parse(fixture.run_ok(R"({"op":"baseline_list"})"));
  CHECK_OK(list_result, core::json_require_object(listed, "result"));
  CHECK_OK(baselines_value, core::json_require_array(*list_result, "baselines"));
  CHECK_OK(baselines, baselines_value->as_array());
  CHECK_EQ(baselines->size(), std::size_t{1});

  const JsonValue exported = fixture.parse(fixture.run_ok(
      R"({"op":"export","kind":"samples","format":"csv","path":"protocol.path","limit":4})"));
  CHECK_OK(export_result, core::json_require_object(exported, "result"));
  CHECK_OK(records, core::json_require_uint(*export_result, "records_exported"));
  CHECK_EQ(records, std::uint64_t{4});
  CHECK_OK(truncated, core::json_require_bool(*export_result, "truncated"));
  CHECK(truncated);
  CHECK_OK(content, core::json_require_string(*export_result, "content"));
  CHECK(content.find("measurement,path,generation") == 0);

  const JsonValue json_export = fixture.parse(fixture.run_ok(
      R"({"op":"export","kind":"summary","format":"json","path":"protocol.path","generation":"protocol.generation","from_ns":0,"to_ns":100000000})"));
  CHECK_OK(json_result, core::json_require_object(json_export, "result"));
  CHECK_OK(digest, core::json_require_string(*json_result, "content_digest"));
  CHECK_EQ(digest.size(), std::size_t{64});
}

LATOBS_TEST(protocol, status_reports_runtime_and_recovery) {
  ServiceFixture fixture;
  fixture.define_catalog();
  fixture.apply(ingest_request(4));
  const JsonValue status = fixture.parse(fixture.run_ok(R"({"op":"status"})"));
  CHECK_OK(result, core::json_require_object(status, "result"));
  CHECK_OK(stats, core::json_require_object(*result, "stats"));
  CHECK_OK(stored, core::json_require_uint(*stats, "measurements_stored"));
  CHECK_EQ(stored, std::uint64_t{4});
  CHECK_OK(recovery, core::json_require_object(*result, "recovery"));
  CHECK_OK(truncated, core::json_require_bool(*recovery, "truncated"));
  CHECK(!truncated);
  CHECK_OK(shut_down, core::json_require_bool(*result, "shut_down"));
  CHECK(!shut_down);
  const JsonValue operations_document = fixture.parse(fixture.run_ok(R"({"op":"operations"})"));
  CHECK_OK(operations_result, core::json_require_object(operations_document, "result"));
  CHECK_OK(operations_value, core::json_require_array(*operations_result, "operations"));
  CHECK_OK(operations, operations_value->as_array());
  CHECK(operations->size() >= 20);
}

LATOBS_TEST_MAIN()
