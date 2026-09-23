// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace latobs::core {

/// Stable, typed failure vocabulary. The string form is part of the wire and
/// persistence contract: codes are never renamed once released.
enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument,
  OutOfRange,
  Overflow,
  ParseError,
  VersionMismatch,
  IntegrityFailure,
  NotFound,
  AlreadyExists,
  Conflict,
  CapacityExceeded,
  Unsupported,
  Refused,
  IoError,
  Cancelled,
  ShuttingDown,
  Internal,
};

std::string_view to_string(ErrorCode code) noexcept;
bool parse_error_code(std::string_view text, ErrorCode& out) noexcept;

/// Fatal misuse reporting. Never used for data conditions.
[[noreturn]] void abort_with(std::string_view message);

/// An error value. A default constructed Error is Ok.
class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message, std::string context = {})
      : code_(code), message_(std::move(message)), context_(std::move(context)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] std::string_view message() const noexcept { return message_; }
  [[nodiscard]] std::string_view context() const noexcept { return context_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] std::string describe() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
  std::string context_;
};

/// Result of an operation that yields a value. Errors are values, not
/// exceptions: every fallible boundary is explicit in the type system.
template <class T>
class [[nodiscard]] Result {
 public:
  using value_type = T;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  T& value() & {
    if (!has_value()) abort_on_error();
    return std::get<0>(storage_);
  }
  const T& value() const& {
    if (!has_value()) abort_on_error();
    return std::get<0>(storage_);
  }
  T&& value() && {
    if (!has_value()) abort_on_error();
    return std::get<0>(std::move(storage_));
  }

  [[nodiscard]] T* try_value() noexcept { return has_value() ? &std::get<0>(storage_) : nullptr; }
  [[nodiscard]] const T* try_value() const noexcept {
    return has_value() ? &std::get<0>(storage_) : nullptr;
  }
  [[nodiscard]] const Error& error() const noexcept {
    static const Error ok_error{};
    return has_value() ? ok_error : std::get<1>(storage_);
  }
  /// Unchecked access used by the LATOBS_TRY propagation macro.
  T& operator*() & noexcept { return std::get<0>(storage_); }
  const T& operator*() const& noexcept { return std::get<0>(storage_); }
  T&& operator*() && noexcept { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] T value_or(T fallback) const {
    return has_value() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  void abort_on_error() const {
    const std::string text = std::string("Result::value() called on an error: ") + error().describe();
    abort_with(text);
  }

  std::variant<T, Error> storage_;
};

/// Result of an operation that yields no value.
class [[nodiscard]] Status {
 public:
  Status() = default;
  Status(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  Error error_;
};

inline Status ok_status() noexcept { return Status{}; }

}  // namespace latobs::core

/// Propagate a failed Result from the enclosing function and bind the
/// successful value to \p name. The binding is a reference into the result
/// object, which lives for the rest of the enclosing scope.
#define LATOBS_TRY(name, expr)                    \
  auto latobs_result_##name = (expr);             \
  if (!latobs_result_##name) return latobs_result_##name.error(); \
  auto&& name = *latobs_result_##name

#define LATOBS_TRY_STATUS(expr)                \
  do {                                         \
    ::latobs::core::Status latobs_status_ = (expr); \
    if (!latobs_status_.ok()) return latobs_status_.error(); \
  } while (false)

#define LATOBS_ASSERT_MSG(cond, message)                  \
  do {                                                    \
    if (!(cond)) ::latobs::core::abort_with(message);      \
  } while (false)
