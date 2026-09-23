// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// latobs: the Latency Observatory command line tool. Every command is a thin
// wrapper over the same request/response service the network transport uses, so
// the tool can never observe something the runtime does not expose.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/runtime/demo.hpp"
#include "latency_observatory/runtime/engine.hpp"
#include "latency_observatory/runtime/service.hpp"
#include "latency_observatory/runtime/transport.hpp"

namespace {

using latobs::core::Error;
using latobs::core::ErrorCode;
using latobs::core::Result;
using latobs::runtime::Service;

struct Options {
  std::string command;
  std::string store;
  std::string host = "127.0.0.1";
  std::string request;
  std::string request_file;
  std::string announce_file;
  std::uint16_t port = 0;
  std::size_t samples = 256;
  std::uint64_t seed = 20260101;
  std::size_t max_connections = 8;
  bool pretty = false;
  bool readonly = false;
  bool announce_stdout = false;
  bool help = false;
};

void print_usage() {
  std::printf(
      "latobs - Latency Observatory runtime\n"
      "\n"
      "usage: latobs <command> [options]\n"
      "\n"
      "commands:\n"
      "  capabilities                     print the REAL/SYNTHETIC/UNSUPPORTED matrix\n"
      "  operations                       list the service operations\n"
      "  request                          execute one request document\n"
      "  serve                            serve the request protocol over TCP\n"
      "  client                           send one request to a running server\n"
      "  demo                             run the labelled synthetic scenario\n"
      "  version                          print the build description\n"
      "\n"
      "options:\n"
      "  --store <dir>                    store directory for the runtime\n"
      "  --request <json|@file|->         request document, file or standard input\n"
      "  --port <n>                       TCP port (0 selects an ephemeral port)\n"
      "  --host <address>                 interface to bind or connect to\n"
      "  --announce <file>                write the bound port to a file\n"
      "  --announce-stdout                 print the bound port to standard output\n"
      "  --max-connections <n>            connection limit for serve\n"
      "  --samples <n>                    sample count for the demo scenario\n"
      "  --seed <n>                       seed for the demo scenario\n"
      "  --readonly                       refuse ingest and definition writes\n"
      "  --pretty                         pretty print canonical JSON\n"
      "  --help                           print this message\n");
}

Result<Options> parse_options(int argc, char** argv) {
  Options options;
  if (argc < 2) {
    options.help = true;
    return options;
  }
  options.command = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument = argv[index];
    auto next = [&](std::string_view name) -> Result<std::string> {
      if (index + 1 >= argc) {
        return Error(ErrorCode::InvalidArgument, "missing value for option", std::string(name));
      }
      ++index;
      return std::string(argv[index]);
    };
    if (argument == "--store") {
      LATOBS_TRY(value, next(argument));
      options.store = value;
    } else if (argument == "--request") {
      LATOBS_TRY(value, next(argument));
      options.request = value;
    } else if (argument == "--request-file") {
      LATOBS_TRY(value, next(argument));
      options.request_file = value;
    } else if (argument == "--port") {
      LATOBS_TRY(value, next(argument));
      const std::optional<std::uint64_t> port = latobs::core::parse_u64(value);
      if (!port.has_value() || *port > 65535) {
        return Error(ErrorCode::InvalidArgument, "invalid port", value);
      }
      options.port = static_cast<std::uint16_t>(*port);
    } else if (argument == "--host") {
      LATOBS_TRY(value, next(argument));
      options.host = value;
    } else if (argument == "--announce") {
      LATOBS_TRY(value, next(argument));
      options.announce_file = value;
    } else if (argument == "--max-connections") {
      LATOBS_TRY(value, next(argument));
      const std::optional<std::uint64_t> count = latobs::core::parse_u64(value);
      if (!count.has_value() || *count == 0) {
        return Error(ErrorCode::InvalidArgument, "invalid connection limit", value);
      }
      options.max_connections = static_cast<std::size_t>(*count);
    } else if (argument == "--samples") {
      LATOBS_TRY(value, next(argument));
      const std::optional<std::uint64_t> count = latobs::core::parse_u64(value);
      if (!count.has_value() || *count == 0) {
        return Error(ErrorCode::InvalidArgument, "invalid sample count", value);
      }
      options.samples = static_cast<std::size_t>(*count);
    } else if (argument == "--seed") {
      LATOBS_TRY(value, next(argument));
      const std::optional<std::uint64_t> seed = latobs::core::parse_u64(value);
      if (!seed.has_value()) {
        return Error(ErrorCode::InvalidArgument, "invalid seed", value);
      }
      options.seed = *seed;
    } else if (argument == "--readonly") {
      options.readonly = true;
    } else if (argument == "--pretty") {
      options.pretty = true;
    } else if (argument == "--announce-stdout") {
      options.announce_stdout = true;
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else {
      return Error(ErrorCode::InvalidArgument, "unknown option", std::string(argument));
    }
  }
  return options;
}

Result<std::string> read_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Error(ErrorCode::IoError, "cannot open the request file", path);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

Result<std::string> read_stdin() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

Result<std::string> load_request(const Options& options) {
  if (!options.request_file.empty()) return read_file(options.request_file);
  if (options.request.empty()) {
    return Error(ErrorCode::InvalidArgument,
                 "a request is required (--request <json|@file|-> or --request-file <path>)");
  }
  if (options.request == "-") return read_stdin();
  if (options.request.size() > 1 && options.request.front() == '@') {
    return read_file(options.request.substr(1));
  }
  return options.request;
}

int report_error(const Error& error) {
  std::fprintf(stderr, "latobs: %s\n", error.describe().c_str());
  return 1;
}

/// Command handlers return process exit codes, so failures are reported to
/// standard error instead of being propagated as values.
#define CLI_TRY(name, expr)                        \
  auto latobs_cli_##name = (expr);                 \
  if (!latobs_cli_##name) return report_error(latobs_cli_##name.error()); \
  auto&& name = *latobs_cli_##name

#define CLI_TRY_STATUS(expr)                                     \
  do {                                                           \
    ::latobs::core::Status latobs_cli_status_ = (expr);          \
    if (!latobs_cli_status_.ok()) return report_error(latobs_cli_status_.error()); \
  } while (false)

int run_request(const Options& options) {
  CLI_TRY(request, load_request(options));
  latobs::runtime::RuntimeConfig config;
  if (!options.store.empty()) config.store_directory = options.store;
  config.readonly = options.readonly;
  CLI_TRY(service, Service::create(config));
  const std::string response = service->execute(request);
  std::fputs(response.c_str(), stdout);
  std::fputc('\n', stdout);
  CLI_TRY_STATUS(service->shutdown());

  // The exit status reflects the outcome of the operation, not just of the
  // process: a refusal or an error is a failed command.
  CLI_TRY(document, latobs::core::parse_json(response, config.policy.limits.max_json_depth));
  const latobs::core::JsonValue* ok = document.find("ok");
  if (ok != nullptr && ok->is_bool()) {
    const latobs::core::Result<bool> value = ok->as_bool();
    if (value.has_value() && !value.value()) return 1;
  }
  return 0;
}

int run_serve(const Options& options) {
  latobs::runtime::RuntimeConfig config;
  if (!options.store.empty()) config.store_directory = options.store;
  config.readonly = options.readonly;
  CLI_TRY(service, Service::create(config));
  latobs::runtime::TransportConfig transport;
  transport.host = options.host;
  transport.port = options.port;
  transport.max_connections = options.max_connections;
  transport.announce_on_stdout = options.announce_stdout || options.announce_file.empty();
  transport.max_line_bytes = config.policy.limits.max_transport_line_bytes;
  CLI_TRY(server, latobs::runtime::LineServer::start(*service, transport));
  if (!options.announce_file.empty()) {
    std::ofstream announce(options.announce_file, std::ios::trunc);
    if (!announce) {
      return report_error(Error(ErrorCode::IoError, "cannot write the announce file",
                                options.announce_file));
    }
    announce << server->port() << "\n";
    announce.flush();
  }
  CLI_TRY_STATUS(server->run());
  CLI_TRY_STATUS(service->shutdown());
  std::fprintf(stdout, "stopped served=%llu\n",
               static_cast<unsigned long long>(server->stats().requests_served));
  std::fflush(stdout);
  return 0;
}

int run_client(const Options& options) {
  CLI_TRY(request, load_request(options));
  if (options.port == 0) {
    return report_error(Error(ErrorCode::InvalidArgument, "a client needs --port"));
  }
  CLI_TRY(response, latobs::runtime::send_request(options.host, options.port, request,
                                                     latobs::core::default_policy()
                                                         .limits.max_json_bytes));
  std::fputs(response.c_str(), stdout);
  std::fputc('\n', stdout);
  CLI_TRY(document, latobs::core::parse_json(response, 32));
  const latobs::core::JsonValue* ok = document.find("ok");
  if (ok != nullptr && ok->is_bool()) {
    const latobs::core::Result<bool> value = ok->as_bool();
    if (value.has_value() && !value.value()) return 1;
  }
  return 0;
}

int run_capabilities(const Options& options) {
  latobs::runtime::RuntimeConfig config;
  if (!options.store.empty()) config.store_directory = options.store;
  CLI_TRY(service, Service::create(config));
  const std::string response = service->execute(R"({"op":"capabilities"})");
  std::fputs(response.c_str(), stdout);
  std::fputc('\n', stdout);
  CLI_TRY_STATUS(service->shutdown());
  return 0;
}

int run_operations() {
  std::fputs(latobs::runtime::describe_operations().c_str(), stdout);
  std::fputc('\n', stdout);
  return 0;
}

int run_version() {
  std::printf("latobs 1.0.0 (%s, %s)\n",
              std::string(latobs::runtime::build_type_name()).c_str(),
              std::string(latobs::runtime::compiler_name()).c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Result<Options> parsed = parse_options(argc, argv);
  if (!parsed.has_value()) {
    return report_error(parsed.error());
  }
  const Options& options = parsed.value();
  if (options.help || options.command.empty()) {
    print_usage();
    // No arguments at all is a usage error; an explicit help request is not.
    return options.command.empty() ? 2 : 0;
  }
  if (options.command == "--help" || options.command == "-h") {
    print_usage();
    return 0;
  }
  if (options.command == "request") return run_request(options);
  if (options.command == "serve") return run_serve(options);
  if (options.command == "client") return run_client(options);
  if (options.command == "capabilities") return run_capabilities(options);
  if (options.command == "operations") return run_operations();
  if (options.command == "version") return run_version();
  if (options.command == "demo") return latobs::runtime::run_demo(options.store, options.samples,
                                                                  options.seed, options.pretty);
  std::fprintf(stderr, "latobs: unknown command '%s'\n", options.command.c_str());
  print_usage();
  return 2;
}
