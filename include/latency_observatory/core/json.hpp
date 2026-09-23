// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "latency_observatory/core/error.hpp"

namespace latobs::core {

/// Canonical JSON writer. Keys are emitted in call order, never from an
/// unordered container, so identical logical content always produces identical
/// bytes. This is what makes exported evidence byte-comparable.
class JsonWriter {
 public:
  explicit JsonWriter(std::string& out, bool pretty = false) : out_(out), pretty_(pretty) {}

  void begin_object();
  void end_object();
  void begin_array();
  void end_array();
  void key(std::string_view name);
  void value_string(std::string_view text);
  void value_int(std::int64_t value);
  void value_uint(std::uint64_t value);
  void value_bool(bool value);
  void value_null();
  /// Emits an already canonical JSON fragment verbatim. The caller guarantees
  /// that the text is a single well formed JSON value; the writer keeps its
  /// balance bookkeeping so structural assertians still hold.
  void raw_value(std::string_view canonical_json);

  void field(std::string_view name, std::string_view value);
  void field(std::string_view name, const char* value);
  void field(std::string_view name, std::int64_t value);
  void field(std::string_view name, int value) { field(name, static_cast<std::int64_t>(value)); }
  /// Also accepts size_t: on 64-bit platforms the two are the same type.
  void field(std::string_view name, std::uint64_t value);
  void field(std::string_view name, bool value);
  void field_null(std::string_view name);
  void field_optional_int(std::string_view name, const std::optional<std::int64_t>& value);
  void field_optional_uint(std::string_view name, const std::optional<std::uint64_t>& value);
  void field_object(std::string_view name);
  void field_array(std::string_view name);

  [[nodiscard]] bool balanced() const noexcept { return stack_is_object_.empty(); }

 private:
  void before_value();
  void write_indent();
  void escape(std::string_view text);

  std::string& out_;
  bool pretty_;
  std::vector<bool> stack_is_object_;
  std::vector<bool> stack_first_;
  bool value_pending_ = false;
};

/// A parsed JSON value. Numbers are kept exact when they are integral.
class JsonValue {
 public:
  enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

  /// A key value pair. The definition follows the class so that the member is
  /// a complete type wherever it is used.
  struct Member;

  JsonValue() = default;

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
  [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
  [[nodiscard]] bool is_integral() const noexcept {
    return type_ == Type::Number && number_is_integral_;
  }
  [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
  [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
  [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

  [[nodiscard]] Result<bool> as_bool() const;
  [[nodiscard]] Result<std::int64_t> as_int64() const;
  [[nodiscard]] Result<std::uint64_t> as_uint64() const;
  [[nodiscard]] Result<std::string_view> as_string() const;
  [[nodiscard]] Result<const std::vector<JsonValue>*> as_array() const;
  [[nodiscard]] Result<const std::vector<Member>*> as_object() const;

  /// Returns nullptr when the member is absent; use require() when the member
  /// is mandatory so the error carries the field name.
  [[nodiscard]] const JsonValue* find(std::string_view name) const;
  [[nodiscard]] Result<const JsonValue*> require(std::string_view name) const;

  [[nodiscard]] static JsonValue make_string(std::string text);
  [[nodiscard]] static JsonValue make_int(std::int64_t value);
  [[nodiscard]] static JsonValue make_bool(bool value);
  [[nodiscard]] static JsonValue make_null();
  [[nodiscard]] static JsonValue make_array();
  [[nodiscard]] static JsonValue make_object();

  void push_back(JsonValue value);
  void set(std::string name, JsonValue value);

 private:
  friend Result<JsonValue> parse_json(std::string_view text, std::size_t max_depth);

  Type type_ = Type::Null;
  bool bool_value_ = false;
  bool number_is_integral_ = false;
  std::int64_t int_value_ = 0;
  std::string text_;
  std::vector<JsonValue> items_;
  std::vector<Member> members_;
};

struct JsonValue::Member {
  std::string name;
  JsonValue value;
};

/// Parses a complete JSON document. The parser is iterative in its bounds: the
/// depth limit protects the C++ stack against adversarial nesting, and the
/// caller is responsible for the byte limit.
[[nodiscard]] Result<JsonValue> parse_json(std::string_view text, std::size_t max_depth);

/// Convenience accessors used by decoders: they produce uniformly worded errors.
[[nodiscard]] Result<std::string_view> json_require_string(const JsonValue& object,
                                                           std::string_view name);
[[nodiscard]] Result<std::int64_t> json_require_int(const JsonValue& object, std::string_view name);
[[nodiscard]] Result<std::uint64_t> json_require_uint(const JsonValue& object, std::string_view name);
[[nodiscard]] Result<bool> json_require_bool(const JsonValue& object, std::string_view name);
[[nodiscard]] Result<const JsonValue*> json_require_object(const JsonValue& object,
                                                           std::string_view name);
[[nodiscard]] Result<const JsonValue*> json_require_array(const JsonValue& object,
                                                          std::string_view name);
[[nodiscard]] Result<std::optional<std::int64_t>> json_optional_int(const JsonValue& object,
                                                                    std::string_view name);
[[nodiscard]] Result<std::optional<std::uint64_t>> json_optional_uint(const JsonValue& object,
                                                                      std::string_view name);

}  // namespace latobs::core
