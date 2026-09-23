// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/store/store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "latency_observatory/core/checked.hpp"
#include "latency_observatory/core/digest.hpp"
#include "latency_observatory/core/time.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace latobs::store {
namespace {

constexpr std::pair<RecordType, std::string_view> kRecordTypes[] = {
    {RecordType::Source, "source"},
    {RecordType::Endpoint, "endpoint"},
    {RecordType::Link, "link"},
    {RecordType::Queue, "queue"},
    {RecordType::Hop, "hop"},
    {RecordType::Generation, "generation"},
    {RecordType::Path, "path"},
    {RecordType::ClockDomain, "clock_domain"},
    {RecordType::ClockSync, "clock_sync"},
    {RecordType::MeasurementCurrent, "measurement_current"},
    {RecordType::MeasurementHistorical, "measurement_historical"},
    {RecordType::Baseline, "baseline"},
    {RecordType::FenceState, "fence_state"},
};

constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void put_u16(std::uint8_t* buffer, std::uint16_t value) {
  buffer[0] = static_cast<std::uint8_t>(value & 0xFFu);
  buffer[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_u32(std::uint8_t* buffer, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    buffer[index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
}

void put_u64(std::uint8_t* buffer, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    buffer[index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (8 * index);
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (8 * index);
  }
  return value;
}

[[nodiscard]] Result<std::string> read_exact(std::FILE* file, std::size_t size) {
  std::string buffer(size, '\0');
  if (size == 0) return buffer;
  const std::size_t read = std::fread(buffer.data(), 1, size, file);
  if (read != size) {
    return Error(ErrorCode::IoError, "short read from the store");
  }
  return buffer;
}

[[nodiscard]] std::filesystem::path manifest_path(const std::filesystem::path& directory) {
  return directory / "manifest.lobs";
}

[[nodiscard]] std::string segment_file_name(std::uint32_t segment_id) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "seg-%08x.lobs", segment_id);
  return std::string(buffer);
}

/// Parses the hexadecimal identifier out of "seg-XXXXXXXX.lobs".
[[nodiscard]] std::optional<std::uint32_t> segment_id_from_name(const std::string& name) {
  // "seg-" + eight hexadecimal digits + ".lobs"
  if (name.size() != 17 || name.rfind("seg-", 0) != 0 || name.substr(12) != ".lobs") {
    return std::nullopt;
  }
  std::uint64_t identifier = 0;
  for (std::size_t index = 4; index < 12; ++index) {
    const char c = name[index];
    std::uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint64_t>(c - 'a') + 10ULL;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint64_t>(c - 'A') + 10ULL;
    } else {
      return std::nullopt;
    }
    identifier = (identifier << 4) | digit;
  }
  if (identifier > 0xFFFFFFFFULL) return std::nullopt;
  return static_cast<std::uint32_t>(identifier);
}

[[nodiscard]] std::optional<std::string> manifest_field(const std::string& body,
                                                        std::string_view name) {
  const std::string key = std::string(name) + "=";
  const std::size_t start = body.find(key);
  if (start == std::string::npos) return std::nullopt;
  const std::size_t value_start = start + key.size();
  const std::size_t end = body.find('\n', value_start);
  return body.substr(value_start, end == std::string::npos ? std::string::npos
                                                           : end - value_start);
}

}  // namespace

std::string_view to_string(RecordType type) noexcept {
  for (const auto& entry : kRecordTypes) {
    if (entry.first == type) return entry.second;
  }
  return "unknown";
}

bool parse_record_type(std::string_view text, RecordType& out) noexcept {
  for (const auto& entry : kRecordTypes) {
    if (entry.second == text) {
      out = entry.first;
      return true;
    }
  }
  return false;
}

void RecoveryReport::note(core::ReasonCode code, std::string text) {
  reasons.push_back(core::Reason{code, std::move(text)});
  truncated = true;
}

Status durable_flush(std::FILE* file) {
  if (file == nullptr) {
    return Error(ErrorCode::InvalidArgument, "durable flush requires an open file");
  }
  if (std::fflush(file) != 0) {
    return Error(ErrorCode::IoError, "failed to flush the store file");
  }
#if defined(_WIN32)
  if (_commit(_fileno(file)) != 0) {
    return Error(ErrorCode::IoError, "failed to commit the store file to disk");
  }
#else
  if (fsync(fileno(file)) != 0) {
    return Error(ErrorCode::IoError, "failed to commit the store file to disk");
  }
#endif
  return core::ok_status();
}

Result<std::vector<std::filesystem::path>> list_segments(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> segments;
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    return segments;
  }
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return Error(ErrorCode::IoError, "cannot enumerate the store directory", directory.string());
  }
  for (const std::filesystem::directory_entry& entry : iterator) {
    std::error_code entry_error;
    if (!entry.is_regular_file(entry_error)) continue;
    const std::string name = entry.path().filename().string();
    if (segment_id_from_name(name).has_value()) {
      segments.push_back(entry.path());
    }
  }
  std::sort(segments.begin(), segments.end());
  return segments;
}

StoreWriter::StoreWriter(StoreWriter&& other) noexcept { *this = std::move(other); }

StoreWriter& StoreWriter::operator=(StoreWriter&& other) noexcept {
  if (this == &other) return *this;
  if (file_ != nullptr) std::fclose(file_);
  config_ = std::move(other.config_);
  file_ = other.file_;
  segment_id_ = other.segment_id_;
  segment_bytes_ = other.segment_bytes_;
  segment_records_ = other.segment_records_;
  segment_payload_bytes_ = other.segment_payload_bytes_;
  segment_digest_ = other.segment_digest_;
  total_bytes_ = other.total_bytes_;
  total_records_ = other.total_records_;
  closed_ = other.closed_;
  other.file_ = nullptr;
  other.closed_ = true;
  return *this;
}

StoreWriter::~StoreWriter() {
  if (file_ != nullptr) {
    (void)close_segment();
    std::fclose(file_);
    file_ = nullptr;
  }
}

Result<StoreWriter> StoreWriter::open(const StoreConfig& config) {
  if (config.directory.empty()) {
    return Error(ErrorCode::InvalidArgument, "store directory must not be empty");
  }
  std::error_code error;
  const bool exists = std::filesystem::exists(config.directory, error);
  if (!exists) {
    if (!config.create_if_missing) {
      return Error(ErrorCode::NotFound, "store directory does not exist",
                   config.directory.string());
    }
    std::filesystem::create_directories(config.directory, error);
    if (error) {
      return Error(ErrorCode::IoError, "cannot create the store directory",
                   config.directory.string());
    }
  } else if (!std::filesystem::is_directory(config.directory, error)) {
    return Error(ErrorCode::InvalidArgument, "store path is not a directory",
                 config.directory.string());
  }

  const std::filesystem::path manifest = manifest_path(config.directory);
  if (std::filesystem::exists(manifest, error)) {
    LATOBS_TRY(reader, StoreReader::open(config));
    (void)reader;
  } else {
    std::string body;
    body.append("magic=");
    body.append(kStoreMagic);
    body.push_back('\n');
    body.append("format=");
    body.append(std::to_string(kFormatVersion));
    body.push_back('\n');
    body.append("created_ns=");
    body.append(std::to_string(core::Clock::now_reference().ns));
    body.push_back('\n');
    body.append("policy_digest=");
    body.append(config.policy_digest);
    body.push_back('\n');
    std::string text = body;
    char crc_text[16];
    std::snprintf(crc_text, sizeof(crc_text), "%08x", core::crc32c(body.data(), body.size()));
    text.append("crc32c=");
    text.append(crc_text);
    text.push_back('\n');
    std::FILE* file = std::fopen(manifest.string().c_str(), "wb");
    if (file == nullptr) {
      return Error(ErrorCode::IoError, "cannot create the store manifest", manifest.string());
    }
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
    const Status flushed = durable_flush(file);
    std::fclose(file);
    if (written != text.size() || !flushed.ok()) {
      return Error(ErrorCode::IoError, "cannot write the store manifest", manifest.string());
    }
  }

  StoreWriter writer;
  writer.config_ = config;
  LATOBS_TRY(segments, list_segments(config.directory));
  std::uint32_t next_segment = 1;
  if (!segments.empty()) {
    const std::optional<std::uint32_t> last =
        segment_id_from_name(segments.back().filename().string());
    if (last.has_value() && *last < 0xFFFFFFFFu) {
      next_segment = *last + 1u;
    }
  }
  writer.segment_id_ = next_segment;
  // The segment file is created on the first append: a runtime that only reads
  // must not leave an empty segment behind.
  return writer;
}

Status StoreWriter::open_segment() {
  const std::filesystem::path path = config_.directory / segment_file_name(segment_id_);
  file_ = std::fopen(path.string().c_str(), "wb");
  if (file_ == nullptr) {
    return Error(ErrorCode::IoError, "cannot create a store segment", path.string());
  }
  std::array<std::uint8_t, kSegmentHeaderBytes> header{};
  std::memcpy(header.data(), kSegmentMagic.data(), kSegmentMagic.size());
  put_u16(header.data() + 8, kFormatVersion);
  put_u16(header.data() + 10, static_cast<std::uint16_t>(kSegmentHeaderBytes));
  put_u32(header.data() + 12, segment_id_);
  put_u64(header.data() + 16, static_cast<std::uint64_t>(core::Clock::now_reference().ns));
  put_u32(header.data() + 28, 0);  // flags
  put_u32(header.data() + 32, core::crc32c(header.data(), 32));
  if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
    return Error(ErrorCode::IoError, "cannot write the segment header", path.string());
  }
  segment_bytes_ = kSegmentHeaderBytes;
  segment_records_ = 0;
  segment_payload_bytes_ = 0;
  segment_digest_ = 14695981039346656037ULL;
  return core::ok_status();
}

Status StoreWriter::close_segment() {
  if (file_ == nullptr) return core::ok_status();
  std::array<std::uint8_t, kSegmentFooterBytes> footer{};
  std::memcpy(footer.data(), kSegmentFooterMagic.data(), kSegmentFooterMagic.size());
  // Layout: magic[8] record_count[u32] reserved[u32] payload_bytes[u64]
  //         crc[u32 over the first 24 bytes] reserved[u32] segment_digest[u64]
  put_u32(footer.data() + 8, static_cast<std::uint32_t>(segment_records_));
  put_u32(footer.data() + 12, 0);  // reserved
  put_u64(footer.data() + 16, segment_payload_bytes_);
  put_u32(footer.data() + 24, core::crc32c(footer.data(), 24));
  put_u32(footer.data() + 28, 0);  // reserved
  put_u64(footer.data() + 32, segment_digest_);
  if (std::fwrite(footer.data(), 1, footer.size(), file_) != footer.size()) {
    return Error(ErrorCode::IoError, "cannot write the segment footer");
  }
  segment_bytes_ += kSegmentFooterBytes;
  total_bytes_ += kSegmentFooterBytes;
  return durable_flush(file_);
}

Status StoreWriter::append(RecordType type, std::string_view payload) {
  if (closed_) {
    return Error(ErrorCode::InvalidArgument, "the writer is closed");
  }
  if (file_ == nullptr) {
    LATOBS_TRY_STATUS(open_segment());
  }
  if (payload.size() > kMaxRecordPayloadBytes) {
    return Error(ErrorCode::CapacityExceeded, "record payload exceeds the supported maximum",
                 std::to_string(payload.size()));
  }
  const std::uint64_t record_bytes =
      static_cast<std::uint64_t>(kRecordHeaderBytes) + static_cast<std::uint64_t>(payload.size());
  if (record_bytes + kSegmentFooterBytes > config_.limits.max_segment_bytes) {
    return Error(ErrorCode::CapacityExceeded,
                 "the record cannot fit inside the configured segment size",
                 std::to_string(payload.size()));
  }
  if (segment_records_ != 0 &&
      (segment_records_ + 1 > config_.limits.max_segment_records ||
       segment_bytes_ + record_bytes + kSegmentFooterBytes > config_.limits.max_segment_bytes)) {
    LATOBS_TRY_STATUS(close_segment());
    if (std::fclose(file_) != 0) {
      file_ = nullptr;
      return Error(ErrorCode::IoError, "cannot close a rotated segment");
    }
    file_ = nullptr;
    ++segment_id_;
    LATOBS_TRY_STATUS(open_segment());
  }
  const std::optional<std::uint64_t> projected = core::checked_add_u64(total_bytes_, record_bytes);
  if (!projected.has_value() || *projected > config_.limits.max_store_bytes) {
    return Error(ErrorCode::CapacityExceeded,
                 "the store has reached its configured maximum size",
                 std::to_string(total_bytes_));
  }

  std::array<std::uint8_t, kRecordHeaderBytes> header{};
  put_u32(header.data(), static_cast<std::uint32_t>(payload.size()));
  header[4] = static_cast<std::uint8_t>(type);
  header[5] = 0;  // flags
  put_u16(header.data() + 6, 0);
  put_u32(header.data() + 8, core::crc32c(payload.data(), payload.size()));
  if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
    return Error(ErrorCode::IoError, "cannot write a record header");
  }
  if (!payload.empty() &&
      std::fwrite(payload.data(), 1, payload.size(), file_) != payload.size()) {
    return Error(ErrorCode::IoError, "cannot write a record payload");
  }
  // Rolling segment digest: a deterministic fold of every payload in the
  // segment, used to validate a segment as a unit.
  segment_digest_ = (segment_digest_ * kFnvPrime) ^ core::fnv1a64(payload);
  ++segment_records_;
  segment_bytes_ += record_bytes;
  segment_payload_bytes_ += payload.size();
  total_bytes_ += record_bytes;
  ++total_records_;
  return core::ok_status();
}

Status StoreWriter::sync() {
  if (file_ == nullptr) return core::ok_status();  // nothing buffered
  return durable_flush(file_);
}

Status StoreWriter::close() {
  if (closed_) return core::ok_status();
  if (file_ != nullptr) {
    LATOBS_TRY_STATUS(close_segment());
    if (std::fclose(file_) != 0) {
      file_ = nullptr;
      closed_ = true;
      return Error(ErrorCode::IoError, "cannot close the store segment");
    }
    file_ = nullptr;
  }
  closed_ = true;
  return core::ok_status();
}

Result<StoreReader> StoreReader::open(const StoreConfig& config) {
  if (config.directory.empty()) {
    return Error(ErrorCode::InvalidArgument, "store directory must not be empty");
  }
  const std::filesystem::path manifest = manifest_path(config.directory);
  std::error_code error;
  if (!std::filesystem::exists(manifest, error)) {
    return Error(ErrorCode::NotFound, "store manifest is missing", manifest.string());
  }
  std::FILE* file = std::fopen(manifest.string().c_str(), "rb");
  if (file == nullptr) {
    return Error(ErrorCode::IoError, "cannot open the store manifest", manifest.string());
  }
  std::string text;
  char buffer[256];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
    text.append(buffer, read);
    if (text.size() > 4096) break;
  }
  std::fclose(file);
  if (text.size() > 4096) {
    return Error(ErrorCode::IntegrityFailure, "store manifest is implausibly large",
                 manifest.string());
  }

  const std::size_t crc_offset = text.rfind("crc32c=");
  if (crc_offset == std::string::npos) {
    return Error(ErrorCode::IntegrityFailure, "store manifest has no integrity check",
                 manifest.string());
  }
  const std::string body = text.substr(0, crc_offset);
  std::string crc_text = text.substr(crc_offset + 7);
  while (!crc_text.empty() && (crc_text.back() == '\n' || crc_text.back() == '\r')) {
    crc_text.pop_back();
  }
  char expected_crc[16];
  std::snprintf(expected_crc, sizeof(expected_crc), "%08x", core::crc32c(body.data(), body.size()));
  if (crc_text != expected_crc) {
    return Error(ErrorCode::IntegrityFailure, "store manifest integrity check failed",
                 manifest.string());
  }
  const std::optional<std::string> magic = manifest_field(body, "magic");
  if (!magic.has_value() || *magic != kStoreMagic) {
    return Error(ErrorCode::IntegrityFailure, "store manifest magic does not match",
                 manifest.string());
  }
  const std::optional<std::string> format = manifest_field(body, "format");
  const std::optional<std::uint64_t> version =
      format.has_value() ? core::parse_u64(*format) : std::nullopt;
  if (!version.has_value()) {
    return Error(ErrorCode::IntegrityFailure, "store manifest has no usable format version",
                 manifest.string());
  }
  if (*version != kFormatVersion) {
    return Error(ErrorCode::VersionMismatch,
                 "store format version is not supported by this build", *format);
  }
  StoreReader reader;
  reader.config_ = config;
  reader.manifest_format_ = static_cast<std::uint16_t>(*version);
  const std::optional<std::string> digest = manifest_field(body, "policy_digest");
  reader.manifest_policy_digest_ = digest.has_value() ? *digest : std::string();
  return reader;
}

Status StoreReader::read_all(std::vector<StoredRecord>& records, RecoveryReport& report) {
  LATOBS_TRY(segments, list_segments(config_.directory));
  bool stop = false;
  for (const std::filesystem::path& path : segments) {
    if (stop) break;
    ++report.segments_scanned;
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
      return Error(ErrorCode::IoError, "cannot open a store segment", path.string());
    }
    const Result<std::string> header = read_exact(file, kSegmentHeaderBytes);
    if (!header.has_value()) {
      std::fclose(file);
      ++report.segments_rejected;
      report.note(core::ReasonCode::SegmentTruncated, path.filename().string());
      stop = true;
      continue;
    }
    const auto* header_bytes = reinterpret_cast<const std::uint8_t*>(header.value().data());
    const bool magic_ok = std::memcmp(header_bytes, kSegmentMagic.data(), kSegmentMagic.size()) == 0;
    const std::uint16_t format = get_u16(header_bytes + 8);
    const std::uint16_t header_size = get_u16(header_bytes + 10);
    const std::uint32_t header_crc = get_u32(header_bytes + 32);
    if (!magic_ok || format != kFormatVersion || header_size != kSegmentHeaderBytes ||
        header_crc != core::crc32c(header_bytes, 32)) {
      std::fclose(file);
      ++report.segments_rejected;
      report.note(core::ReasonCode::RecordIntegrityFailure, path.filename().string());
      stop = true;
      continue;
    }

    std::uint64_t recovered_records = 0;
    std::uint64_t recovered_payload_bytes = 0;
    bool saw_footer = false;
    bool segment_truncated = false;
    std::uint32_t footer_record_count = 0;

    while (true) {
      const Result<std::string> record_header = read_exact(file, kRecordHeaderBytes);
      if (!record_header.has_value()) {
        // Either the end of a segment that was never closed, or a partial
        // record header. A segment that yielded no record at all carries no
        // lost evidence; anything else is a conservative truncation.
        segment_truncated = recovered_records != 0;
        break;
      }
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(record_header.value().data());
      // A record header can never start with the footer magic: the first four
      // bytes are the payload length, and "LOBS" as a little endian length
      // exceeds the maximum record payload.
      if (std::memcmp(bytes, kSegmentFooterMagic.data(), kSegmentFooterMagic.size()) == 0) {
        saw_footer = true;
        footer_record_count = get_u32(bytes + 8);
        const Result<std::string> tail =
            read_exact(file, kSegmentFooterBytes - kRecordHeaderBytes);
        if (!tail.has_value()) {
          segment_truncated = true;
          break;
        }
        std::array<std::uint8_t, kSegmentFooterBytes> footer{};
        std::memcpy(footer.data(), bytes, kRecordHeaderBytes);
        std::memcpy(footer.data() + kRecordHeaderBytes, tail.value().data(), tail.value().size());
        const std::uint32_t footer_crc = get_u32(footer.data() + 24);
        if (core::crc32c(footer.data(), 24) != footer_crc) {
          segment_truncated = true;
          report.note(core::ReasonCode::RecordIntegrityFailure,
                      path.filename().string() + " footer");
          break;
        }
        if (get_u64(footer.data() + 16) != recovered_payload_bytes) {
          segment_truncated = true;
          report.note(core::ReasonCode::RecordIntegrityFailure,
                      path.filename().string() + " payload byte count");
          break;
        }
        break;
      }
      const std::uint32_t length = get_u32(bytes);
      const std::uint8_t type_byte = bytes[4];
      const std::uint32_t crc = get_u32(bytes + 8);
      if (length > kMaxRecordPayloadBytes) {
        segment_truncated = true;
        report.note(core::ReasonCode::RecordIntegrityFailure,
                    path.filename().string() + " record length");
        break;
      }
      const Result<std::string> payload = read_exact(file, length);
      if (!payload.has_value()) {
        segment_truncated = true;
        break;
      }
      if (core::crc32c(payload.value().data(), payload.value().size()) != crc) {
        segment_truncated = true;
        report.note(core::ReasonCode::RecordIntegrityFailure,
                    path.filename().string() + " record crc");
        break;
      }
      RecordType type = RecordType::Source;
      bool known_type = false;
      for (const auto& entry : kRecordTypes) {
        if (static_cast<std::uint8_t>(entry.first) == type_byte) {
          type = entry.first;
          known_type = true;
          break;
        }
      }
      if (!known_type) {
        segment_truncated = true;
        report.note(core::ReasonCode::RecordIntegrityFailure,
                    path.filename().string() + " record type");
        break;
      }
      StoredRecord record;
      record.type = type;
      record.payload = payload.value();
      record.segment_id = get_u32(header_bytes + 12);
      record.sequence_in_segment = recovered_records;
      records.push_back(std::move(record));
      ++recovered_records;
      recovered_payload_bytes += length;
      ++report.records_read;
    }

    report.bytes_read += static_cast<std::uint64_t>(std::ftell(file));
    std::fclose(file);

    if (saw_footer && !segment_truncated && footer_record_count != recovered_records) {
      segment_truncated = true;
      report.note(core::ReasonCode::RecordIntegrityFailure,
                  path.filename().string() + " record count");
    }
    if (segment_truncated) {
      ++report.records_discarded;
      ++report.segments_rejected;
      report.note(core::ReasonCode::RecoveryConservative,
                  "reading stopped at the first damaged record in " + path.filename().string());
      report.detail = "truncated at " + path.filename().string();
      stop = true;
    } else {
      ++report.segments_accepted;
    }
  }
  if (!report.clean()) {
    bool has_conservative = false;
    for (const core::Reason& reason : report.reasons) {
      if (reason.code == core::ReasonCode::RecoveryConservative) has_conservative = true;
    }
    if (!has_conservative) {
      report.reasons.push_back(
          core::Reason{core::ReasonCode::RecoveryConservative, "conservative recovery applied"});
    }
  }
  return core::ok_status();
}

}  // namespace latobs::store
