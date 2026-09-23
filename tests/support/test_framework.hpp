// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// A deliberately small test framework.
///
/// Design constraints that matter for this repository:
///   * no timeouts of any kind: a test either completes or the suite is broken;
///   * every check records file and line, and a failed check aborts only the
///     current test case;
///   * the exit code is the number of failed checks, so a single failure can
///     never be mistaken for success.

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "latency_observatory/core/ids.hpp"

namespace latobs::test {

struct CheckFailure {
  std::string message;
  std::string file;
  int line = 0;
};

class SkipTest {
 public:
  explicit SkipTest(std::string reason) : reason_(std::move(reason)) {}
  [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

 private:
  std::string reason_;
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> function;
};

class Registry {
 public:
  static Registry& instance();

  bool add(std::string suite, std::string name, std::function<void()> function);
  [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }
  /// Runs every case whose "suite.name" contains \p filter (empty runs all).
  int run(std::string_view filter);

 private:
  std::vector<TestCase> cases_;
};

[[noreturn]] void fail(const char* file, int line, std::string message);

/// Records a non fatal observation: used by the property tests to report how
/// many cases they exercised.
void note(std::string text);

[[nodiscard]] std::string to_text(std::int64_t value);
[[nodiscard]] std::string to_text(std::uint64_t value);
[[nodiscard]] std::string to_text(int value);
[[nodiscard]] std::string to_text(unsigned int value);
[[nodiscard]] std::string to_text(double value);

/// Typed identities render as their canonical hexadecimal form.
template <class Tag>
[[nodiscard]] std::string to_text(const core::StrongId<Tag>& id) {
  return id.valid() ? id.to_hex() : std::string("invalid");
}

[[nodiscard]] inline std::string to_text(const core::Name& name) { return name.str(); }
[[nodiscard]] inline std::string to_text(const core::Sequence& value) {
  return std::to_string(value.value());
}
[[nodiscard]] inline std::string to_text(const core::Revision& value) {
  return std::to_string(value.value());
}
[[nodiscard]] inline std::string to_text(const core::HopIndex& value) {
  return std::to_string(value.value());
}

/// Scoped enumerations render as their underlying number; tests that need the
/// stable name compare the string form explicitly.
template <class Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
[[nodiscard]] std::string to_text(Enum value) {
  return std::to_string(static_cast<long long>(value));
}
[[nodiscard]] std::string to_text(std::string_view value);
[[nodiscard]] std::string to_text(const char* value);
[[nodiscard]] std::string to_text(bool value);

}  // namespace latobs::test

#define LATOBS_TEST(suite_name, case_name)                                          \
  static void latobs_test_##suite_name##_##case_name();                             \
  static const bool latobs_registered_##suite_name##_##case_name =                  \
      ::latobs::test::Registry::instance().add(                                     \
          #suite_name, #case_name, &latobs_test_##suite_name##_##case_name);        \
  static void latobs_test_##suite_name##_##case_name()

#define CHECK(condition)                                                            \
  do {                                                                              \
    if (!(condition)) {                                                             \
      ::latobs::test::fail(__FILE__, __LINE__, "check failed: " #condition);        \
    }                                                                               \
  } while (false)

#define CHECK_MSG(condition, message)                                               \
  do {                                                                              \
    if (!(condition)) {                                                             \
      ::latobs::test::fail(__FILE__, __LINE__,                                      \
                           std::string("check failed: " #condition " - ") +         \
                               ::latobs::test::to_text(message));                   \
    }                                                                               \
  } while (false)

/// The operands are copied on purpose: an expression such as
/// \c *result.value() yields a reference into a temporary that dies at the end
/// of the full expression, and binding it to a reference would dangle.
#define CHECK_EQ(actual, expected)                                                  \
  do {                                                                              \
    const auto latobs_actual_ = (actual);                                           \
    const auto latobs_expected_ = (expected);                                       \
    if (!(latobs_actual_ == latobs_expected_)) {                                    \
      ::latobs::test::fail(__FILE__, __LINE__,                                      \
                           std::string("expected ") + #actual " == " + #expected +  \
                               " but got " + ::latobs::test::to_text(latobs_actual_) + \
                               " vs " + ::latobs::test::to_text(latobs_expected_)); \
    }                                                                               \
  } while (false)

#define CHECK_NE(actual, unexpected)                                                \
  do {                                                                              \
    const auto latobs_actual_ = (actual);                                           \
    const auto latobs_unexpected_ = (unexpected);                                   \
    if (latobs_actual_ == latobs_unexpected_) {                                     \
      ::latobs::test::fail(__FILE__, __LINE__,                                      \
                           std::string("expected ") + #actual " != " + #unexpected); \
    }                                                                               \
  } while (false)

/// Token pasting with __LINE__ needs one level of indirection to expand.
#define LATOBS_DETAIL_CONCAT(prefix, line) prefix##line
#define LATOBS_DETAIL_CONCAT_EXPANDED(prefix, line) LATOBS_DETAIL_CONCAT(prefix, line)
#define LATOBS_DETAIL_UNIQUE(prefix) LATOBS_DETAIL_CONCAT_EXPANDED(prefix, __LINE__)

/// Unwraps a Result, failing the current test when it carries an error. The
/// bound value is a reference into the result object, which lives until the end
/// of the enclosing scope.
#define CHECK_OK(name, expression)                                                  \
  auto LATOBS_DETAIL_UNIQUE(latobs_result_) = (expression);                          \
  if (!LATOBS_DETAIL_UNIQUE(latobs_result_)) {                                       \
    ::latobs::test::fail(__FILE__, __LINE__,                                        \
                         std::string("unexpected error: ") +                        \
                             LATOBS_DETAIL_UNIQUE(latobs_result_).error().describe()); \
  }                                                                                 \
  [[maybe_unused]] auto&& name = *LATOBS_DETAIL_UNIQUE(latobs_result_)

/// Asserts that an operation returning Status succeeded.
#define CHECK_OK_STATUS(expression)                                                 \
  do {                                                                              \
    const ::latobs::core::Status latobs_status_ = (expression);                     \
    if (!latobs_status_.ok()) {                                                     \
      ::latobs::test::fail(__FILE__, __LINE__,                                      \
                           std::string("unexpected error: ") +                      \
                               latobs_status_.error().describe());                  \
    }                                                                               \
  } while (false)

/// Asserts that a call failed, and binds its error for inspection.
#define CHECK_ERR(name, expression)                                                 \
  auto LATOBS_DETAIL_UNIQUE(latobs_result_) = (expression);                          \
  if (LATOBS_DETAIL_UNIQUE(latobs_result_)) {                                        \
    ::latobs::test::fail(__FILE__, __LINE__,                                        \
                         "expected an error but the call succeeded");               \
  }                                                                                 \
  [[maybe_unused]] auto&& name = LATOBS_DETAIL_UNIQUE(latobs_result_).error()

#define SKIP_TEST(reason) throw ::latobs::test::SkipTest(reason)

#define LATOBS_TEST_MAIN()                                                          \
  int main(int argc, char** argv) {                                                 \
    const std::string filter = argc > 1 ? argv[1] : std::string();                  \
    return ::latobs::test::Registry::instance().run(filter);                        \
  }
