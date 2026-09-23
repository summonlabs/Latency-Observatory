// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/core/json.hpp"

#include <cstdio>
#include <cstdlib>

#include "latency_observatory/core/checked.hpp"

namespace latobs::core {
namespace {

[[nodiscard]] bool is_ws(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  }
}

[[nodiscard]] bool parse_hex4(std::string_view text, std::size_t offset, std::uint32_t& out) noexcept {
  if (offset + 4 > text.size()) return false;
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    const char c = text[offset + index];
    std::uint32_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint32_t>(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint32_t>(c - 'A') + 10u;
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  out = value;
  return true;
}

class Parser {
 public:
  Parser(std::string_view text, std::size_t max_depth) : text_(text), max_depth_(max_depth) {}

  [[nodiscard]] Result<JsonValue> run() {
    skip_ws();
    JsonValue value;
    LATOBS_TRY(parsed, parse_value(0));
    value = std::move(parsed);
    skip_ws();
    if (position_ != text_.size()) {
      return Error(ErrorCode::ParseError, "trailing content after the JSON document",
                   context());
    }
    return value;
  }

 private:
  [[nodiscard]] std::string context() const {
    const std::size_t begin = position_ > 16 ? position_ - 16 : 0;
    return "offset " + std::to_string(position_) + " near '" +
           std::string(text_.substr(begin, 24)) + "'";
  }

  void skip_ws() noexcept {
    while (position_ < text_.size() && is_ws(text_[position_])) ++position_;
  }

  [[nodiscard]] Result<JsonValue> parse_value(std::size_t depth) {
    if (depth > max_depth_) {
      return Error(ErrorCode::OutOfRange, "JSON nesting exceeds the configured depth limit");
    }
    if (position_ >= text_.size()) {
      return Error(ErrorCode::ParseError, "unexpected end of JSON document", context());
    }
    const char c = text_[position_];
    switch (c) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"': {
        LATOBS_TRY(text, parse_string());
        return JsonValue::make_string(std::move(text));
      }
      case 't':
        return parse_literal("true", JsonValue::make_bool(true));
      case 'f':
        return parse_literal("false", JsonValue::make_bool(false));
      case 'n':
        return parse_literal("null", JsonValue::make_null());
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        return Error(ErrorCode::ParseError, "unexpected character in JSON document", context());
    }
  }

  [[nodiscard]] Result<JsonValue> parse_literal(std::string_view literal, JsonValue value) {
    if (text_.substr(position_, literal.size()) != literal) {
      return Error(ErrorCode::ParseError, "invalid JSON literal", context());
    }
    position_ += literal.size();
    return value;
  }

  [[nodiscard]] Result<JsonValue> parse_object(std::size_t depth) {
    ++position_;  // consume '{'
    JsonValue object = JsonValue::make_object();
    skip_ws();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      return object;
    }
    while (true) {
      skip_ws();
      if (position_ >= text_.size() || text_[position_] != '"') {
        return Error(ErrorCode::ParseError, "expected an object key", context());
      }
      LATOBS_TRY(name, parse_string());
      if (object.find(name) != nullptr) {
        return Error(ErrorCode::ParseError, "duplicate object key in JSON document",
                     std::string(name));
      }
      skip_ws();
      if (position_ >= text_.size() || text_[position_] != ':') {
        return Error(ErrorCode::ParseError, "expected ':' after an object key", context());
      }
      ++position_;
      skip_ws();
      LATOBS_TRY(value, parse_value(depth + 1));
      object.set(std::move(name), std::move(value));
      skip_ws();
      if (position_ >= text_.size()) {
        return Error(ErrorCode::ParseError, "unterminated object in JSON document", context());
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == '}') {
        ++position_;
        return object;
      }
      return Error(ErrorCode::ParseError, "expected ',' or '}' in JSON object", context());
    }
  }

  [[nodiscard]] Result<JsonValue> parse_array(std::size_t depth) {
    ++position_;  // consume '['
    JsonValue array = JsonValue::make_array();
    skip_ws();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      return array;
    }
    while (true) {
      skip_ws();
      LATOBS_TRY(value, parse_value(depth + 1));
      array.push_back(std::move(value));
      skip_ws();
      if (position_ >= text_.size()) {
        return Error(ErrorCode::ParseError, "unterminated array in JSON document", context());
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == ']') {
        ++position_;
        return array;
      }
      return Error(ErrorCode::ParseError, "expected ',' or ']' in JSON array", context());
    }
  }

  [[nodiscard]] Result<std::string> parse_string() {
    ++position_;  // consume opening quote
    std::string out;
    while (true) {
      if (position_ >= text_.size()) {
        return Error(ErrorCode::ParseError, "unterminated string in JSON document", context());
      }
      const char c = text_[position_];
      if (c == '"') {
        ++position_;
        return out;
      }
      if (static_cast<unsigned char>(c) < 0x20u) {
        return Error(ErrorCode::ParseError, "control character in JSON string", context());
      }
      if (c != '\\') {
        out.push_back(c);
        ++position_;
        continue;
      }
      ++position_;
      if (position_ >= text_.size()) {
        return Error(ErrorCode::ParseError, "unterminated escape in JSON string", context());
      }
      const char escape = text_[position_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t code_point = 0;
          if (!parse_hex4(text_, position_, code_point)) {
            return Error(ErrorCode::ParseError, "invalid \\u escape in JSON string", context());
          }
          position_ += 4;
          if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
            if (position_ + 1 < text_.size() && text_[position_] == '\\' && text_[position_ + 1] == 'u') {
              position_ += 2;
              std::uint32_t low = 0;
              if (!parse_hex4(text_, position_, low)) {
                return Error(ErrorCode::ParseError, "invalid surrogate pair in JSON string", context());
              }
              position_ += 4;
              if (low < 0xDC00u || low > 0xDFFFu) {
                return Error(ErrorCode::ParseError, "invalid low surrogate in JSON string", context());
              }
              const std::uint32_t combined =
                  0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
              append_utf8(out, combined);
            } else {
              return Error(ErrorCode::ParseError, "unpaired high surrogate in JSON string", context());
            }
          } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
            return Error(ErrorCode::ParseError, "unpaired low surrogate in JSON string", context());
          } else {
            append_utf8(out, code_point);
          }
          break;
        }
        default:
          return Error(ErrorCode::ParseError, "unknown escape sequence in JSON string", context());
      }
    }
  }

  [[nodiscard]] Result<JsonValue> parse_number() {
    const std::size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-') ++position_;
    const std::size_t digits_start = position_;
    while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    if (position_ == digits_start) {
      return Error(ErrorCode::ParseError, "number has no digits", context());
    }
    bool integral = true;
    if (position_ < text_.size() && text_[position_] == '.') {
      integral = false;
      ++position_;
      const std::size_t frac_start = position_;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
      if (position_ == frac_start) {
        return Error(ErrorCode::ParseError, "number fraction has no digits", context());
      }
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      integral = false;
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
      const std::size_t exp_start = position_;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
      if (position_ == exp_start) {
        return Error(ErrorCode::ParseError, "number exponent has no digits", context());
      }
    }
    const std::string_view literal = text_.substr(start, position_ - start);
    if (integral && literal.size() <= 20) {
      const std::optional<std::int64_t> value = parse_i64(literal);
      if (value.has_value()) return JsonValue::make_int(*value);
    }
    // Non-integral or out-of-range integers are rejected rather than silently
    // rounded: dynamic evidence is always exact.
    return Error(ErrorCode::OutOfRange, "JSON number is not an exact 64-bit integer", std::string(literal));
  }

  std::string_view text_;
  std::size_t max_depth_;
  std::size_t position_ = 0;
};

}  // namespace

void JsonWriter::write_indent() {
  if (!pretty_) return;
  out_.push_back('\n');
  out_.append(stack_is_object_.size() * 2, ' ');
}

void JsonWriter::before_value() {
  if (value_pending_) {
    value_pending_ = false;
    return;
  }
  if (!stack_first_.empty()) {
    if (!stack_first_.back()) out_.push_back(',');
    stack_first_.back() = false;
    write_indent();
  }
}

void JsonWriter::begin_object() {
  before_value();
  out_.push_back('{');
  stack_is_object_.push_back(true);
  stack_first_.push_back(true);
}

void JsonWriter::end_object() {
  LATOBS_ASSERT_MSG(!stack_is_object_.empty() && stack_is_object_.back(), "unbalanced JSON writer");
  LATOBS_ASSERT_MSG(!value_pending_, "JSON object ended without a value for the last key");
  const bool was_empty = stack_first_.back();
  if (!was_empty && pretty_) {
    out_.push_back('\n');
    out_.append((stack_is_object_.size() - 1) * 2, ' ');
  }
  out_.push_back('}');
  stack_is_object_.pop_back();
  stack_first_.pop_back();
}

void JsonWriter::begin_array() {
  before_value();
  out_.push_back('[');
  stack_is_object_.push_back(false);
  stack_first_.push_back(true);
}

void JsonWriter::end_array() {
  LATOBS_ASSERT_MSG(!stack_is_object_.empty() && !stack_is_object_.back(), "unbalanced JSON writer");
  LATOBS_ASSERT_MSG(!value_pending_, "JSON array ended without a value for the last key");
  const bool was_empty = stack_first_.back();
  if (!was_empty && pretty_) {
    out_.push_back('\n');
    out_.append((stack_is_object_.size() - 1) * 2, ' ');
  }
  out_.push_back(']');
  stack_is_object_.pop_back();
  stack_first_.pop_back();
}

void JsonWriter::key(std::string_view name) {
  LATOBS_ASSERT_MSG(!stack_is_object_.empty() && stack_is_object_.back(),
                    "JSON key outside of an object");
  LATOBS_ASSERT_MSG(!value_pending_, "JSON key written before the previous value");
  if (!stack_first_.back()) out_.push_back(',');
  stack_first_.back() = false;
  write_indent();
  escape(name);
  out_.push_back(':');
  if (pretty_) out_.push_back(' ');
  value_pending_ = true;
}

void JsonWriter::escape(std::string_view text) {
  static constexpr char kDigits[] = "0123456789abcdef";
  out_.push_back('"');
  for (const char c : text) {
    const unsigned char byte = static_cast<unsigned char>(c);
    switch (byte) {
      case '"': out_.append("\\\""); break;
      case '\\': out_.append("\\\\"); break;
      case '\b': out_.append("\\b"); break;
      case '\f': out_.append("\\f"); break;
      case '\n': out_.append("\\n"); break;
      case '\r': out_.append("\\r"); break;
      case '\t': out_.append("\\t"); break;
      default:
        if (byte < 0x20u) {
          out_.append("\\u00");
          out_.push_back(kDigits[(byte >> 4) & 0x0Fu]);
          out_.push_back(kDigits[byte & 0x0Fu]);
        } else {
          out_.push_back(c);
        }
        break;
    }
  }
  out_.push_back('"');
}

void JsonWriter::value_string(std::string_view text) {
  before_value();
  escape(text);
}

void JsonWriter::value_int(std::int64_t value) {
  before_value();
  out_.append(std::to_string(value));
}

void JsonWriter::value_uint(std::uint64_t value) {
  before_value();
  out_.append(std::to_string(value));
}

void JsonWriter::value_bool(bool value) {
  before_value();
  out_.append(value ? "true" : "false");
}

void JsonWriter::raw_value(std::string_view canonical_json) {
  LATOBS_ASSERT_MSG(!canonical_json.empty(), "raw JSON values must not be empty");
  before_value();
  out_.append(canonical_json);
}

void JsonWriter::value_null() {
  before_value();
  out_.append("null");
}

void JsonWriter::field(std::string_view name, std::string_view value) {
  key(name);
  value_string(value);
}

void JsonWriter::field(std::string_view name, const char* value) { field(name, std::string_view(value)); }

void JsonWriter::field(std::string_view name, std::int64_t value) {
  key(name);
  value_int(value);
}

void JsonWriter::field(std::string_view name, std::uint64_t value) {
  key(name);
  value_uint(value);
}

void JsonWriter::field(std::string_view name, bool value) {
  key(name);
  value_bool(value);
}

void JsonWriter::field_null(std::string_view name) {
  key(name);
  value_null();
}

void JsonWriter::field_optional_int(std::string_view name, const std::optional<std::int64_t>& value) {
  key(name);
  if (value.has_value()) {
    value_int(*value);
  } else {
    value_null();
  }
}

void JsonWriter::field_optional_uint(std::string_view name,
                                     const std::optional<std::uint64_t>& value) {
  key(name);
  if (value.has_value()) {
    value_uint(*value);
  } else {
    value_null();
  }
}

void JsonWriter::field_object(std::string_view name) {
  key(name);
  begin_object();
}

void JsonWriter::field_array(std::string_view name) {
  key(name);
  begin_array();
}

Result<JsonValue> parse_json(std::string_view text, std::size_t max_depth) {
  Parser parser(text, max_depth);
  return parser.run();
}

Result<bool> JsonValue::as_bool() const {
  if (type_ != Type::Bool) {
    return Error(ErrorCode::InvalidArgument, "JSON value is not a boolean");
  }
  return bool_value_;
}

Result<std::int64_t> JsonValue::as_int64() const {
  if (type_ != Type::Number) {
    return Error(ErrorCode::InvalidArgument, "JSON value is not a number");
  }
  if (!number_is_integral_) {
    return Error(ErrorCode::InvalidArgument, "JSON number is not integral");
  }
  return int_value_;
}

Result<std::uint64_t> JsonValue::as_uint64() const {
  LATOBS_TRY(value, as_int64());
  if (value < 0) {
    return Error(ErrorCode::InvalidArgument, "JSON number is negative where an unsigned value is required");
  }
  return static_cast<std::uint64_t>(value);
}

Result<std::string_view> JsonValue::as_string() const {
  if (type_ != Type::String) {
    return Error(ErrorCode::InvalidArgument, "JSON value is not a string");
  }
  return std::string_view(text_);
}

Result<const std::vector<JsonValue>*> JsonValue::as_array() const {
  if (type_ != Type::Array) {
    return Error(ErrorCode::InvalidArgument, "JSON value is not an array");
  }
  return &items_;
}

Result<const std::vector<JsonValue::Member>*> JsonValue::as_object() const {
  if (type_ != Type::Object) {
    return Error(ErrorCode::InvalidArgument, "JSON value is not an object");
  }
  return &members_;
}

const JsonValue* JsonValue::find(std::string_view name) const {
  if (type_ != Type::Object) return nullptr;
  for (const Member& member : members_) {
    if (member.name == name) return &member.value;
  }
  return nullptr;
}

Result<const JsonValue*> JsonValue::require(std::string_view name) const {
  if (type_ != Type::Object) {
    return Error(ErrorCode::InvalidArgument, "JSON value is not an object");
  }
  const JsonValue* found = find(name);
  if (found == nullptr) {
    return Error(ErrorCode::NotFound, "required JSON field is missing", std::string(name));
  }
  return found;
}

JsonValue JsonValue::make_string(std::string text) {
  JsonValue value;
  value.type_ = Type::String;
  value.text_ = std::move(text);
  return value;
}

JsonValue JsonValue::make_int(std::int64_t number) {
  JsonValue value;
  value.type_ = Type::Number;
  value.number_is_integral_ = true;
  value.int_value_ = number;
  return value;
}

JsonValue JsonValue::make_bool(bool flag) {
  JsonValue value;
  value.type_ = Type::Bool;
  value.bool_value_ = flag;
  return value;
}

JsonValue JsonValue::make_null() { return JsonValue{}; }

JsonValue JsonValue::make_array() {
  JsonValue value;
  value.type_ = Type::Array;
  return value;
}

JsonValue JsonValue::make_object() {
  JsonValue value;
  value.type_ = Type::Object;
  return value;
}

void JsonValue::push_back(JsonValue value) {
  LATOBS_ASSERT_MSG(type_ == Type::Array, "push_back on a non-array JSON value");
  items_.push_back(std::move(value));
}

void JsonValue::set(std::string name, JsonValue value) {
  LATOBS_ASSERT_MSG(type_ == Type::Object, "set on a non-object JSON value");
  members_.push_back(Member{std::move(name), std::move(value)});
}

Result<std::string_view> json_require_string(const JsonValue& object, std::string_view name) {
  LATOBS_TRY(value, object.require(name));
  return value->as_string();
}

Result<std::int64_t> json_require_int(const JsonValue& object, std::string_view name) {
  LATOBS_TRY(value, object.require(name));
  return value->as_int64();
}

Result<std::uint64_t> json_require_uint(const JsonValue& object, std::string_view name) {
  LATOBS_TRY(value, object.require(name));
  return value->as_uint64();
}

Result<bool> json_require_bool(const JsonValue& object, std::string_view name) {
  LATOBS_TRY(value, object.require(name));
  return value->as_bool();
}

Result<const JsonValue*> json_require_object(const JsonValue& object, std::string_view name) {
  LATOBS_TRY(value, object.require(name));
  if (!value->is_object()) {
    return Error(ErrorCode::InvalidArgument, "JSON field is not an object", std::string(name));
  }
  return value;
}

Result<const JsonValue*> json_require_array(const JsonValue& object, std::string_view name) {
  LATOBS_TRY(value, object.require(name));
  if (!value->is_array()) {
    return Error(ErrorCode::InvalidArgument, "JSON field is not an array", std::string(name));
  }
  return value;
}

Result<std::optional<std::int64_t>> json_optional_int(const JsonValue& object, std::string_view name) {
  const JsonValue* value = object.find(name);
  if (value == nullptr || value->is_null()) return std::optional<std::int64_t>{};
  LATOBS_TRY(number, value->as_int64());
  return std::optional<std::int64_t>{number};
}

Result<std::optional<std::uint64_t>> json_optional_uint(const JsonValue& object, std::string_view name) {
  const JsonValue* value = object.find(name);
  if (value == nullptr || value->is_null()) return std::optional<std::uint64_t>{};
  LATOBS_TRY(number, value->as_uint64());
  return std::optional<std::uint64_t>{number};
}

}  // namespace latobs::core
