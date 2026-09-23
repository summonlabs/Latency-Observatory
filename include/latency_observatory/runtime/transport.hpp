// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "latency_observatory/runtime/service.hpp"
#include "latency_observatory/runtime/vocabulary.hpp"

namespace latobs::runtime {

struct TransportConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;  // 0 selects an ephemeral port
  std::size_t max_connections = 8;
  std::size_t max_line_bytes = 1024 * 1024;
  /// When true, the bound port is written to standard output as
  /// "listening port=<n>" so that a supervising process can discover it
  /// without polling.
  bool announce_on_stdout = false;
};

struct TransportStats {
  std::uint64_t connections_accepted = 0;
  std::uint64_t connections_rejected = 0;
  std::uint64_t requests_served = 0;
  std::uint64_t requests_refused = 0;
  std::uint64_t bytes_received = 0;
  std::uint64_t bytes_sent = 0;
  bool stopped = false;
};

/// A line oriented TCP server for the canonical JSON request protocol. One
/// request per line, one response per line. The server is the proof surface for
/// the transport claim: it is exercised from independent processes.
class LineServer {
 public:
  LineServer(const LineServer&) = delete;
  LineServer& operator=(const LineServer&) = delete;
  ~LineServer();

  [[nodiscard]] static Result<std::unique_ptr<LineServer>> start(Service& service,
                                                                 TransportConfig config);
  /// Serves until the shutdown operation is received or request_stop() is
  /// called. Every connection is handled on its own bounded worker thread.
  [[nodiscard]] Status run();
  void request_stop();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] TransportStats stats() const;
  [[nodiscard]] bool stopped() const noexcept { return stop_requested_.load(); }

 private:
  /// Defined in the implementation file: the private implementation type is
  /// incomplete here on purpose.
  LineServer(Service& service, TransportConfig config);

  struct Impl;
  Service& service_;
  TransportConfig config_;
  std::unique_ptr<Impl> impl_;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_requested_{false};
};

/// Sends one request to a server and returns the response line. This is the
/// client used by tooling and by the independent process transport tests.
[[nodiscard]] Result<std::string> send_request(std::string_view host, std::uint16_t port,
                                               std::string_view request,
                                               std::size_t max_response_bytes);

}  // namespace latobs::runtime
