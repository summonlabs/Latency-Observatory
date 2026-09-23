// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/runtime/vocabulary.hpp"

namespace latobs::runtime {

enum class ExportKind : std::uint8_t { Samples, Summary, Baselines };
enum class ExportFormat : std::uint8_t { Json, Csv };

std::string_view to_string(ExportKind kind) noexcept;
bool parse_export_kind(std::string_view text, ExportKind& out) noexcept;
std::string_view to_string(ExportFormat format) noexcept;
bool parse_export_format(std::string_view text, ExportFormat& out) noexcept;

struct ExportRequest {
  ExportKind kind = ExportKind::Summary;
  ExportFormat format = ExportFormat::Json;
  PathId path;              // invalid: every path
  GenerationId generation;  // invalid: every generation
  Timestamp from;           // invalid: unbounded
  Timestamp to;             // invalid: unbounded
  std::size_t limit = 1000;  // hard bound on the number of exported records
  bool pretty = false;
};

/// The exported text plus the accounting of what was left out. An export never
/// silently drops records: truncation is reported in the result.
struct ExportResult {
  std::string text;
  std::uint64_t records_exported = 0;
  std::uint64_t records_skipped = 0;
  bool truncated = false;
  std::string content_digest;
  std::vector<std::string> notes;
};

void write_json(core::JsonWriter& writer, const ExportResult& result);

}  // namespace latobs::runtime
