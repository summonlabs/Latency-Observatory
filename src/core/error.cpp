// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/core/error.hpp"

#include <cstdio>
#include <cstdlib>

namespace latobs::core {
namespace {

struct CodeName {
  ErrorCode code;
  std::string_view name;
};

constexpr CodeName kCodeNames[] = {
    {ErrorCode::Ok, "ok"},
    {ErrorCode::InvalidArgument, "invalid_argument"},
    {ErrorCode::OutOfRange, "out_of_range"},
    {ErrorCode::Overflow, "overflow"},
    {ErrorCode::ParseError, "parse_error"},
    {ErrorCode::VersionMismatch, "version_mismatch"},
    {ErrorCode::IntegrityFailure, "integrity_failure"},
    {ErrorCode::NotFound, "not_found"},
    {ErrorCode::AlreadyExists, "already_exists"},
    {ErrorCode::Conflict, "conflict"},
    {ErrorCode::CapacityExceeded, "capacity_exceeded"},
    {ErrorCode::Unsupported, "unsupported"},
    {ErrorCode::Refused, "refused"},
    {ErrorCode::IoError, "io_error"},
    {ErrorCode::Cancelled, "cancelled"},
    {ErrorCode::ShuttingDown, "shutting_down"},
    {ErrorCode::Internal, "internal"},
};

}  // namespace

std::string_view to_string(ErrorCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) return entry.name;
  }
  return "unknown";
}

bool parse_error_code(std::string_view text, ErrorCode& out) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.name == text) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

std::string Error::describe() const {
  std::string result;
  result.reserve(message_.size() + context_.size() + 24);
  result.append(to_string(code_));
  result.append(": ");
  result.append(message_);
  if (!context_.empty()) {
    result.append(" [");
    result.append(context_);
    result.push_back(']');
  }
  return result;
}

void abort_with(std::string_view message) {
  std::fputs("latency_observatory fatal: ", stderr);
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

}  // namespace latobs::core
