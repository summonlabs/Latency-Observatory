// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/runtime/transport.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace latobs::runtime {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

std::once_flag network_once;
bool network_ready = false;

void initialise_network() {
  std::call_once(network_once, []() {
#if defined(_WIN32)
    WSADATA data{};
    network_ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    network_ready = true;
#endif
  });
}

void close_socket(SocketHandle handle) {
  if (handle == kInvalidSocket) return;
#if defined(_WIN32)
  closesocket(handle);
#else
  ::close(handle);
#endif
}

[[nodiscard]] bool would_block() noexcept {
#if defined(_WIN32)
  return WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

struct AddressInfoDeleter {
  void operator()(addrinfo* info) const {
    if (info != nullptr) freeaddrinfo(info);
  }
};

[[nodiscard]] Result<SocketHandle> connect_to(std::string_view host, std::uint16_t port) {
  initialise_network();
  if (!network_ready) {
    return Error(ErrorCode::IoError, "the network stack is not available");
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  const std::string host_text(host);
  const std::string port_text = std::to_string(port);
  addrinfo* raw = nullptr;
  if (getaddrinfo(host_text.c_str(), port_text.c_str(), &hints, &raw) != 0) {
    return Error(ErrorCode::IoError, "cannot resolve the server address", host_text);
  }
  std::unique_ptr<addrinfo, AddressInfoDeleter> addresses(raw);
  for (addrinfo* entry = addresses.get(); entry != nullptr; entry = entry->ai_next) {
    const SocketHandle handle =
        ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (handle == kInvalidSocket) continue;
    if (::connect(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
      return handle;
    }
    close_socket(handle);
  }
  return Error(ErrorCode::IoError, "cannot connect to the server", host_text + ":" + port_text);
}

[[nodiscard]] Result<std::string> read_line(SocketHandle handle, std::size_t max_bytes) {
  std::string line;
  char buffer[4096];
  while (true) {
    const int received = ::recv(handle, buffer, static_cast<int>(sizeof(buffer)), 0);
    if (received == 0) break;
    if (received < 0) {
      if (would_block()) continue;
      return Error(ErrorCode::IoError, "the connection failed while reading");
    }
    for (int index = 0; index < received; ++index) {
      if (buffer[index] == '\n') {
        return line;
      }
      line.push_back(buffer[index]);
      if (line.size() > max_bytes) {
        return Error(ErrorCode::CapacityExceeded, "the request line exceeds the configured limit",
                     std::to_string(line.size()));
      }
    }
  }
  if (line.empty()) {
    return Error(ErrorCode::IoError, "the peer closed the connection without a request");
  }
  return line;
}

[[nodiscard]] Status write_all(SocketHandle handle, std::string_view data) {
  std::size_t written = 0;
  while (written < data.size()) {
    const int sent = ::send(handle, data.data() + written,
                            static_cast<int>(data.size() - written), 0);
    if (sent <= 0) {
      if (would_block()) continue;
      return Error(ErrorCode::IoError, "the connection failed while writing");
    }
    written += static_cast<std::size_t>(sent);
  }
  return core::ok_status();
}

}  // namespace

struct LineServer::Impl {
  SocketHandle listener = kInvalidSocket;
  mutable std::mutex mutex;
  std::vector<std::thread> connection_threads;
  TransportStats stats;
  std::size_t active_connections = 0;
};

LineServer::LineServer(Service& service, TransportConfig config)
    : service_(service), config_(std::move(config)) {}

LineServer::~LineServer() {
  request_stop();
  if (impl_ != nullptr) {
    for (std::thread& thread : impl_->connection_threads) {
      if (thread.joinable()) thread.join();
    }
    close_socket(impl_->listener);
    impl_->listener = kInvalidSocket;
  }
}

Result<std::unique_ptr<LineServer>> LineServer::start(Service& service, TransportConfig config) {
  initialise_network();
  if (!network_ready) {
    return Error(ErrorCode::IoError, "the network stack is not available");
  }
  if (config.max_connections == 0) {
    return Error(ErrorCode::InvalidArgument, "the transport needs at least one connection slot");
  }
  if (config.max_line_bytes == 0) {
    return Error(ErrorCode::InvalidArgument, "the transport needs a non-zero line limit");
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  const std::string port_text = std::to_string(config.port);
  addrinfo* raw = nullptr;
  if (getaddrinfo(config.host.c_str(), port_text.c_str(), &hints, &raw) != 0) {
    return Error(ErrorCode::IoError, "cannot resolve the listening address", config.host);
  }
  std::unique_ptr<addrinfo, AddressInfoDeleter> addresses(raw);
  SocketHandle listener = kInvalidSocket;
  for (addrinfo* entry = addresses.get(); entry != nullptr; entry = entry->ai_next) {
    const SocketHandle handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (handle == kInvalidSocket) continue;
    const int reuse = 1;
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), static_cast<int>(sizeof(reuse)));
    if (::bind(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0 &&
        ::listen(handle, static_cast<int>(config.max_connections)) == 0) {
      listener = handle;
      break;
    }
    close_socket(handle);
  }
  if (listener == kInvalidSocket) {
    return Error(ErrorCode::IoError, "cannot bind the listening socket", config.host);
  }

  sockaddr_storage address{};
  int address_length = static_cast<int>(sizeof(address));
  if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
    close_socket(listener);
    return Error(ErrorCode::IoError, "cannot determine the bound port");
  }
  std::uint16_t bound_port = 0;
  if (address.ss_family == AF_INET) {
    bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
  } else {
    bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
  }

  std::unique_ptr<LineServer> server(new LineServer(service, config));
  server->impl_ = std::make_unique<Impl>();
  server->impl_->listener = listener;
  server->port_ = bound_port;
  if (config.announce_on_stdout) {
    std::printf("listening port=%u\n", static_cast<unsigned>(bound_port));
    std::fflush(stdout);
  }
  return server;
}

void LineServer::request_stop() {
  if (stop_requested_.exchange(true)) return;
  // Wake the blocking accept by connecting to ourselves, then close the
  // listener so that no new connection can be accepted.
  if (impl_ != nullptr && impl_->listener != kInvalidSocket) {
    const Result<SocketHandle> wake = connect_to(config_.host, port_);
    if (wake.has_value()) close_socket(wake.value());
    close_socket(impl_->listener);
    impl_->listener = kInvalidSocket;
  }
}

Status LineServer::run() {
  if (impl_ == nullptr) {
    return Error(ErrorCode::InvalidArgument, "the server is not started");
  }
  while (!stop_requested_.load()) {
    sockaddr_storage address{};
    int address_length = static_cast<int>(sizeof(address));
    const SocketHandle connection =
        ::accept(impl_->listener, reinterpret_cast<sockaddr*>(&address), &address_length);
    if (connection == kInvalidSocket) {
      if (stop_requested_.load()) break;
      if (would_block()) continue;
      return Error(ErrorCode::IoError, "the accept call failed");
    }
    bool accepted = false;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (impl_->active_connections < config_.max_connections) {
        ++impl_->active_connections;
        ++impl_->stats.connections_accepted;
        accepted = true;
      } else {
        ++impl_->stats.connections_rejected;
      }
    }
    if (!accepted) {
      static const std::string busy =
          "{\"ok\":false,\"op\":\"unknown\",\"error\":{\"code\":\"capacity_exceeded\","
          "\"message\":\"the server is at its connection limit\",\"context\":\"\"}}\n";
      (void)write_all(connection, busy);
      close_socket(connection);
      continue;
    }
    impl_->connection_threads.emplace_back([this, connection]() {
      SocketHandle handle = connection;
      while (!stop_requested_.load()) {
        const Result<std::string> line = read_line(handle, config_.max_line_bytes);
        if (!line.has_value()) break;
        {
          std::lock_guard<std::mutex> lock(impl_->mutex);
          impl_->stats.bytes_received += line.value().size() + 1;
        }
        const std::string response = service_.execute(line.value());
        const Status sent = write_all(handle, response);
        const Status terminator = sent.ok() ? write_all(handle, "\n") : sent;
        {
          std::lock_guard<std::mutex> lock(impl_->mutex);
          if (sent.ok()) {
            ++impl_->stats.requests_served;
            impl_->stats.bytes_sent += response.size() + 1;
          } else {
            ++impl_->stats.requests_refused;
          }
        }
        if (!terminator.ok()) break;
        if (line.value().find("\"shutdown\"") != std::string::npos) {
          request_stop();
          break;
        }
      }
      close_socket(handle);
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (impl_->active_connections > 0) --impl_->active_connections;
    });
    // Join finished connection threads so the bookkeeping stays bounded.
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->connection_threads.size() > config_.max_connections * 4) {
      std::vector<std::thread> live;
      for (std::thread& thread : impl_->connection_threads) {
        if (thread.joinable() && impl_->active_connections == 0) {
          thread.join();
        } else {
          live.push_back(std::move(thread));
        }
      }
      impl_->connection_threads = std::move(live);
    }
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stats.stopped = true;
  }
  for (std::thread& thread : impl_->connection_threads) {
    if (thread.joinable()) thread.join();
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->connection_threads.clear();
  }
  return core::ok_status();
}

TransportStats LineServer::stats() const {
  TransportStats copy;
  if (impl_ == nullptr) return copy;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  copy = impl_->stats;
  return copy;
}

Result<std::string> send_request(std::string_view host, std::uint16_t port,
                                 std::string_view request, std::size_t max_response_bytes) {
  LATOBS_TRY(handle, connect_to(host, port));
  const SocketHandle socket = handle;
  LATOBS_TRY_STATUS(write_all(socket, request));
  const Status terminator = write_all(socket, "\n");
  if (!terminator.ok()) {
    close_socket(socket);
    return terminator.error();
  }
  const Result<std::string> line = read_line(socket, max_response_bytes);
  close_socket(socket);
  if (!line.has_value()) return line.error();
  return line.value();
}

}  // namespace latobs::runtime
