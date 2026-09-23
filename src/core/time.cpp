// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/core/time.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

#include "latency_observatory/core/checked.hpp"

namespace latobs::core {
namespace {
constexpr std::string_view kReferenceDomainName = "lobs.clock.reference.utc";

constexpr Nanos kNanosPerSecond = 1000000000LL;
constexpr Nanos kNanosPerDay = 86400LL * kNanosPerSecond;

/// Howard Hinnant's civil-from-days algorithm, integer only.
void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::uint64_t doe = static_cast<std::uint64_t>(days - era * 146097);
  const std::uint64_t yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::uint64_t mp = (5 * doy + 2) / 153;
  const std::uint64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::uint64_t m = mp < 10 ? mp + 3 : mp - 9;
  year = y + (m <= 2 ? 1 : 0);
  month = static_cast<unsigned>(m);
  day = static_cast<unsigned>(d);
}

[[nodiscard]] std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const std::uint64_t yoe = static_cast<std::uint64_t>(year - era * 400);
  const std::uint64_t doy =
      (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const std::uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
}  // namespace

ClockDomainId reference_clock_domain() noexcept {
  static const ClockDomainId id = ClockDomainId::derive_from(kReferenceDomainName);
  return id;
}

std::string_view reference_clock_domain_name() noexcept { return kReferenceDomainName; }

Timestamp Clock::now_reference() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return Timestamp{static_cast<Nanos>(nanos), reference_clock_domain()};
}

MonoTime Clock::mono_now() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return MonoTime{static_cast<Nanos>(nanos)};
}

Timestamp Clock::now_in(ClockDomainId domain) noexcept {
  return Timestamp{now_reference().ns, domain};
}

std::string format_utc(Nanos unix_ns) {
  std::int64_t seconds = unix_ns / kNanosPerSecond;
  std::int64_t fraction = unix_ns % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    seconds -= 1;
  }
  // Floor division: before 1970 the day index must round towards negative
  // infinity, otherwise the rendered date is one day early.
  std::int64_t days = seconds / 86400;
  std::int64_t second_of_day = seconds % 86400;
  if (second_of_day < 0) {
    second_of_day += 86400;
    --days;
  }

  std::int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(days, year, month, day);

  const unsigned hour = static_cast<unsigned>(second_of_day / 3600);
  const unsigned minute = static_cast<unsigned>((second_of_day % 3600) / 60);
  const unsigned second = static_cast<unsigned>(second_of_day % 60);

  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u.%09lldZ",
                static_cast<long long>(year), month, day, hour, minute, second,
                static_cast<long long>(fraction));
  return std::string(buffer);
}

Result<Nanos> parse_utc(std::string_view text) {
  // Accepted form: YYYY-MM-DDTHH:MM:SS[.fffffffff]Z
  if (text.size() < 20 || text.size() > 30) {
    return Error(ErrorCode::ParseError, "timestamp has an unsupported length", std::string(text));
  }
  auto digits = [&](std::size_t offset, std::size_t count) -> std::optional<std::int64_t> {
    if (offset + count > text.size()) return std::nullopt;
    return parse_i64(text.substr(offset, count));
  };
  const auto year = digits(0, 4);
  const auto month = digits(5, 2);
  const auto day = digits(8, 2);
  const auto hour = digits(11, 2);
  const auto minute = digits(14, 2);
  const auto second = digits(17, 2);
  if (!year || !month || !day || !hour || !minute || !second) {
    return Error(ErrorCode::ParseError, "timestamp field is malformed", std::string(text));
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
    return Error(ErrorCode::ParseError, "timestamp separators are malformed", std::string(text));
  }
  if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour < 0 || *hour > 23 ||
      *minute < 0 || *minute > 59 || *second < 0 || *second > 60) {
    return Error(ErrorCode::OutOfRange, "timestamp field is out of range", std::string(text));
  }
  Nanos fraction = 0;
  std::size_t index = 19;
  if (index < text.size() && text[index] == '.') {
    ++index;
    std::size_t fraction_digits = 0;
    Nanos scale = 100000000LL;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
      if (fraction_digits < 9) {
        fraction += static_cast<Nanos>(text[index] - '0') * scale;
        scale /= 10;
        ++fraction_digits;
      }
      ++index;
    }
    if (fraction_digits == 0) {
      return Error(ErrorCode::ParseError, "timestamp fraction has no digits", std::string(text));
    }
  }
  if (index >= text.size() || text[index] != 'Z' || index + 1 != text.size()) {
    return Error(ErrorCode::ParseError, "timestamp must end with 'Z'", std::string(text));
  }
  const std::int64_t days = days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day));
  const std::optional<std::int64_t> seconds =
      checked_mul_i64(days, 86400LL);
  if (!seconds.has_value()) {
    return Error(ErrorCode::Overflow, "timestamp day count overflows", std::string(text));
  }
  std::optional<std::int64_t> total = checked_add_i64(*seconds, *hour * 3600LL + *minute * 60LL + *second);
  if (!total.has_value()) {
    return Error(ErrorCode::Overflow, "timestamp seconds overflow", std::string(text));
  }
  total = checked_mul_i64(*total, kNanosPerSecond);
  if (!total.has_value()) {
    return Error(ErrorCode::Overflow, "timestamp nanoseconds overflow", std::string(text));
  }
  const std::optional<std::int64_t> with_fraction = checked_add_i64(*total, fraction);
  if (!with_fraction.has_value()) {
    return Error(ErrorCode::Overflow, "timestamp nanoseconds overflow", std::string(text));
  }
  return *with_fraction;
}

}  // namespace latobs::core
