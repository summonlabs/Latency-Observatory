// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/runtime/engine.hpp"

namespace latobs::runtime {

/// The request/response service. Both the command line tool and the network
/// transport call exactly this interface, so the two can never drift apart.
///
/// A request is a JSON object with an "op" member. A response is always a
/// canonical JSON document:
///   {"ok":true,"op":"...","policy_digest":"...","result":{...}}
///   {"ok":false,"op":"...","error":{"code":"...","message":"...","context":"..."}}
/// The service never throws and never returns a partial document.
class Service {
 public:
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<Service>> create(RuntimeConfig config);

  [[nodiscard]] std::string execute(std::string_view request_text);
  [[nodiscard]] std::string execute(const core::JsonValue& request);

  [[nodiscard]] Engine& engine() noexcept { return *engine_; }
  [[nodiscard]] const Engine& engine() const noexcept { return *engine_; }
  [[nodiscard]] std::size_t max_request_bytes() const noexcept { return max_request_bytes_; }
  [[nodiscard]] Status shutdown();

 private:
  Service(std::unique_ptr<Engine> engine, core::Limits limits)
      : engine_(std::move(engine)), limits_(limits), max_request_bytes_(limits.max_json_bytes) {}

  [[nodiscard]] Result<std::string> dispatch(const core::JsonValue& request, std::string& op_name);

  std::unique_ptr<Engine> engine_;
  core::Limits limits_;
  std::size_t max_request_bytes_ = 0;
};

/// Operations supported by the service, with the fields each one accepts.
[[nodiscard]] const std::vector<std::string>& supported_operations();
[[nodiscard]] std::string describe_operations();

}  // namespace latobs::runtime
