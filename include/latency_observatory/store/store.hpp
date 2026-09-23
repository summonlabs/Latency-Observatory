// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/core/policy.hpp"
#include "latency_observatory/store/vocabulary.hpp"

namespace latobs::store {

/// On disk layout version. A store written by a newer format version is never
/// guessed at: opening it fails with a typed version error.
inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::string_view kStoreMagic = "LATOBS-STORE";
inline constexpr std::string_view kSegmentMagic = "LOBSSEG1";
inline constexpr std::string_view kSegmentFooterMagic = "LOBSEND1";
inline constexpr std::size_t kSegmentHeaderBytes = 48;
inline constexpr std::size_t kRecordHeaderBytes = 12;
inline constexpr std::size_t kSegmentFooterBytes = 40;
inline constexpr std::size_t kMaxRecordPayloadBytes = 1024 * 1024;

/// The record kinds a segment may contain. The payload of every record is
/// canonical JSON; the framing around it is binary and integrity checked.
enum class RecordType : std::uint8_t {
  Source = 1,
  Endpoint = 2,
  Link = 3,
  Queue = 4,
  Hop = 5,
  Generation = 6,
  Path = 7,
  ClockDomain = 8,
  ClockSync = 9,
  MeasurementCurrent = 10,
  MeasurementHistorical = 11,
  Baseline = 12,
  FenceState = 13,
};

std::string_view to_string(RecordType type) noexcept;
bool parse_record_type(std::string_view text, RecordType& out) noexcept;

struct StoredRecord {
  RecordType type = RecordType::Source;
  std::string payload;
  std::uint32_t segment_id = 0;
  std::uint64_t sequence_in_segment = 0;
};

struct StoreConfig {
  std::filesystem::path directory;
  core::Limits limits;
  std::string policy_digest;
  /// When false, opening a store that does not exist yet fails instead of
  /// creating one.
  bool create_if_missing = true;
};

/// Everything the recovery discovered while reading the store. Conservative
/// truncation is always reported here; it is never silent.
struct RecoveryReport {
  std::uint64_t segments_scanned = 0;
  std::uint64_t segments_accepted = 0;
  std::uint64_t segments_rejected = 0;
  std::uint64_t records_read = 0;
  std::uint64_t records_discarded = 0;
  std::uint64_t bytes_read = 0;
  bool truncated = false;
  bool manifest_created = false;
  std::string detail;
  std::vector<core::Reason> reasons;

  [[nodiscard]] bool clean() const noexcept {
    return !truncated && segments_rejected == 0 && records_discarded == 0;
  }
  void note(core::ReasonCode code, std::string text);
};

/// Appends records to a directory of segments. The writer rotates segments at
/// the configured record and byte limits and refuses to grow the store beyond
/// the configured total size.
class StoreWriter {
 public:
  StoreWriter() = default;
  StoreWriter(const StoreWriter&) = delete;
  StoreWriter& operator=(const StoreWriter&) = delete;

  [[nodiscard]] static Result<StoreWriter> open(const StoreConfig& config);
  [[nodiscard]] Status append(RecordType type, std::string_view payload);
  /// Flushes user space buffers and asks the platform to make the file durable.
  [[nodiscard]] Status sync();
  [[nodiscard]] Status close();
  [[nodiscard]] std::uint32_t segment_id() const noexcept { return segment_id_; }
  [[nodiscard]] std::uint64_t bytes_written() const noexcept { return total_bytes_; }
  [[nodiscard]] std::uint64_t records_written() const noexcept { return total_records_; }

  StoreWriter(StoreWriter&& other) noexcept;
  StoreWriter& operator=(StoreWriter&& other) noexcept;
  ~StoreWriter();

 private:
  [[nodiscard]] Status open_segment();
  [[nodiscard]] Status close_segment();

  StoreConfig config_;
  std::FILE* file_ = nullptr;
  std::uint32_t segment_id_ = 0;
  std::uint64_t segment_bytes_ = 0;
  std::uint64_t segment_records_ = 0;
  std::uint64_t segment_payload_bytes_ = 0;
  std::uint64_t segment_digest_ = 0;
  std::uint64_t total_bytes_ = 0;
  std::uint64_t total_records_ = 0;
  bool closed_ = false;
};

/// Reads every intact record from a store directory, in write order. A record
/// whose framing or CRC does not validate is never interpreted: the reader
/// stops at the first damaged record and reports the truncation.
class StoreReader {
 public:
  [[nodiscard]] static Result<StoreReader> open(const StoreConfig& config);
  [[nodiscard]] Status read_all(std::vector<StoredRecord>& records, RecoveryReport& report);
  [[nodiscard]] const std::string& manifest_policy_digest() const noexcept {
    return manifest_policy_digest_;
  }
  [[nodiscard]] std::uint16_t manifest_format() const noexcept { return manifest_format_; }

 private:
  StoreConfig config_;
  std::string manifest_policy_digest_;
  std::uint16_t manifest_format_ = 0;
};

/// Lists the segment files of a store directory in write order.
[[nodiscard]] Result<std::vector<std::filesystem::path>> list_segments(
    const std::filesystem::path& directory);

/// Durable flush of an open file. Returns an IO error when the platform cannot
/// guarantee durability; a store that cannot be made durable never claims it.
[[nodiscard]] Status durable_flush(std::FILE* file);

}  // namespace latobs::store
