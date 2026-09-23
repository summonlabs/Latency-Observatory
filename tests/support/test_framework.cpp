// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "test_framework.hpp"

#include <cstdio>
#include <string>

namespace latobs::test {
namespace {

std::vector<std::string>& notes() {
  static std::vector<std::string> value;
  return value;
}

}  // namespace

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

bool Registry::add(std::string suite, std::string name, std::function<void()> function) {
  cases_.push_back(TestCase{std::move(suite), std::move(name), std::move(function)});
  return true;
}

void fail(const char* file, int line, std::string message) {
  throw CheckFailure{std::move(message), file, line};
}

void note(std::string text) { notes().push_back(std::move(text)); }

std::string to_text(std::int64_t value) { return std::to_string(value); }
std::string to_text(std::uint64_t value) { return std::to_string(value); }
std::string to_text(int value) { return std::to_string(value); }
std::string to_text(unsigned int value) { return std::to_string(value); }
std::string to_text(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.6f", value);
  return std::string(buffer);
}
std::string to_text(std::string_view value) { return std::string(value); }
std::string to_text(const char* value) { return std::string(value); }
std::string to_text(bool value) { return value ? "true" : "false"; }

int Registry::run(std::string_view filter) {
  std::size_t failures = 0;
  std::size_t skipped = 0;
  std::size_t executed = 0;
  for (const TestCase& test : cases_) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) continue;
    ++executed;
    try {
      test.function();
      std::printf("[ pass ] %s\n", full.c_str());
    } catch (const SkipTest& skip) {
      ++skipped;
      std::printf("[ skip ] %s: %s\n", full.c_str(), skip.reason().c_str());
    } catch (const CheckFailure& failure_check) {
      ++failures;
      std::printf("[ FAIL ] %s\n         %s\n         at %s:%d\n", full.c_str(),
                  failure_check.message.c_str(), failure_check.file.c_str(),
                  failure_check.line);
    } catch (const std::exception& error) {
      ++failures;
      std::printf("[ FAIL ] %s\n         unexpected exception: %s\n", full.c_str(), error.what());
    } catch (...) {
      ++failures;
      std::printf("[ FAIL ] %s\n         unknown exception\n", full.c_str());
    }
  }
  for (const std::string& text : notes()) {
    std::printf("[ note ] %s\n", text.c_str());
  }
  std::printf("[ done ] %zu case(s), %zu failure(s), %zu skipped\n", executed, failures, skipped);
  return static_cast<int>(failures);
}

}  // namespace latobs::test
