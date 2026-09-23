// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/runtime/export.hpp"

#include "latency_observatory/core/digest.hpp"

namespace latobs::runtime {
namespace {

constexpr std::pair<ExportKind, std::string_view> kKinds[] = {
    {ExportKind::Samples, "samples"},
    {ExportKind::Summary, "summary"},
    {ExportKind::Baselines, "baselines"},
};

constexpr std::pair<ExportFormat, std::string_view> kFormats[] = {
    {ExportFormat::Json, "json"},
    {ExportFormat::Csv, "csv"},
};

}  // namespace

std::string_view to_string(ExportKind kind) noexcept {
  for (const auto& entry : kKinds) {
    if (entry.first == kind) return entry.second;
  }
  return "summary";
}

bool parse_export_kind(std::string_view text, ExportKind& out) noexcept {
  for (const auto& entry : kKinds) {
    if (entry.second == text) {
      out = entry.first;
      return true;
    }
  }
  return false;
}

std::string_view to_string(ExportFormat format) noexcept {
  for (const auto& entry : kFormats) {
    if (entry.first == format) return entry.second;
  }
  return "json";
}

bool parse_export_format(std::string_view text, ExportFormat& out) noexcept {
  for (const auto& entry : kFormats) {
    if (entry.second == text) {
      out = entry.first;
      return true;
    }
  }
  return false;
}

void write_json(core::JsonWriter& writer, const ExportResult& result) {
  writer.begin_object();
  writer.field("records_exported", result.records_exported);
  writer.field("records_skipped", result.records_skipped);
  writer.field("truncated", result.truncated);
  writer.field("content_digest", result.content_digest);
  writer.field_array("notes");
  for (const std::string& note : result.notes) writer.value_string(note);
  writer.end_array();
  writer.end_object();
}

}  // namespace latobs::runtime
