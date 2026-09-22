#include "change_planner/core/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <system_error>

#include "change_planner/core/utf8.hpp"

namespace cplan {
namespace {

[[nodiscard]] bool is_digit(char character) noexcept {
  return character >= '0' && character <= '9';
}

[[nodiscard]] int hex_digit(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return 10 + (character - 'a');
  }
  if (character >= 'A' && character <= 'F') {
    return 10 + (character - 'A');
  }
  return -1;
}

class Parser {
 public:
  Parser(std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {
    if (text_.size() > limits_.max_bytes) {
      status_ = Status::error(ErrorCode::size_limit, "json document exceeds the max_bytes limit");
    }
  }

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

  Result<JsonValue> parse_document() {
    if (!ok()) {
      return status_;
    }
    skip_whitespace();
    JsonValue value;
    if (!parse_value(0, value)) {
      return status_;
    }
    skip_whitespace();
    if (position_ != text_.size()) {
      return fail("trailing content after the JSON value");
    }
    return value;
  }

 private:
  Status fail(std::string message) {
    if (status_.ok()) {
      status_ = Status::error(ErrorCode::malformed_input,
                              std::move(message) + " (offset " + std::to_string(position_) + ")");
    }
    return status_;
  }

  bool fail_bool(std::string message) {
    static_cast<void>(fail(std::move(message)));
    return false;
  }

  void skip_whitespace() {
    while (position_ < text_.size()) {
      const char character = text_[position_];
      if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  bool parse_value(std::size_t depth, JsonValue& out) {
    if (depth > limits_.max_depth) {
      return fail_bool("maximum nesting depth exceeded");
    }
    if (position_ >= text_.size()) {
      return fail_bool("unexpected end of input");
    }
    switch (text_[position_]) {
      case '{':
        return parse_object(depth, out);
      case '[':
        return parse_array(depth, out);
      case '"': {
        std::string text;
        if (!parse_string(text)) {
          return false;
        }
        out = JsonValue::make_string(std::move(text));
        return true;
      }
      case 't':
        return parse_literal("true", JsonValue::make_bool(true), out);
      case 'f':
        return parse_literal("false", JsonValue::make_bool(false), out);
      case 'n':
        return parse_literal("null", JsonValue::make_null(), out);
      default:
        return parse_number(out);
    }
  }

  bool parse_literal(std::string_view literal, JsonValue value, JsonValue& out) {
    if (text_.size() - position_ < literal.size() ||
        text_.substr(position_, literal.size()) != literal) {
      return fail_bool("invalid literal");
    }
    position_ += literal.size();
    out = std::move(value);
    return true;
  }

  bool parse_array(std::size_t depth, JsonValue& out) {
    ++position_;  // consume '['
    JsonValue::array_type elements;
    skip_whitespace();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      out = JsonValue::make_array(std::move(elements));
      return true;
    }
    while (true) {
      if (elements.size() >= limits_.max_elements) {
        return fail_bool("array element count exceeds the configured limit");
      }
      JsonValue element;
      skip_whitespace();
      if (!parse_value(depth + 1, element)) {
        return false;
      }
      elements.push_back(std::move(element));
      skip_whitespace();
      if (position_ >= text_.size()) {
        return fail_bool("unterminated array");
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == ']') {
        ++position_;
        out = JsonValue::make_array(std::move(elements));
        return true;
      }
      return fail_bool("expected ',' or ']' in array");
    }
  }

  bool parse_object(std::size_t depth, JsonValue& out) {
    ++position_;  // consume '{'
    JsonValue::object_type members;
    skip_whitespace();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      out = JsonValue::make_object(std::move(members));
      return true;
    }
    while (true) {
      if (members.size() >= limits_.max_elements) {
        return fail_bool("object member count exceeds the configured limit");
      }
      skip_whitespace();
      if (position_ >= text_.size() || text_[position_] != '"') {
        return fail_bool("expected a string key in object");
      }
      std::string key;
      if (!parse_string(key)) {
        return false;
      }
      skip_whitespace();
      if (position_ >= text_.size() || text_[position_] != ':') {
        return fail_bool("expected ':' after object key");
      }
      ++position_;
      skip_whitespace();
      JsonValue value;
      if (!parse_value(depth + 1, value)) {
        return false;
      }
      members.emplace_back(std::move(key), std::move(value));
      skip_whitespace();
      if (position_ >= text_.size()) {
        return fail_bool("unterminated object");
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == '}') {
        ++position_;
        std::sort(members.begin(), members.end(),
                  [](const JsonValue::member_type& lhs, const JsonValue::member_type& rhs) {
                    return lhs.first < rhs.first;
                  });
        for (std::size_t i = 1; i < members.size(); ++i) {
          if (members[i - 1].first == members[i].first) {
            return fail_bool("duplicate object key: " + members[i].first);
          }
        }
        out = JsonValue::make_object(std::move(members));
        return true;
      }
      return fail_bool("expected ',' or '}' in object");
    }
  }

  bool parse_hex4(std::uint32_t& out) {
    if (position_ + 4 > text_.size()) {
      return fail_bool("truncated unicode escape");
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const int digit = hex_digit(text_[position_ + i]);
      if (digit < 0) {
        return fail_bool("invalid hex digit in unicode escape");
      }
      value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    position_ += 4;
    out = value;
    return true;
  }

  bool parse_string(std::string& out) {
    ++position_;  // consume opening quote
    out.clear();
    while (true) {
      if (position_ >= text_.size()) {
        return fail_bool("unterminated string");
      }
      const char character = text_[position_];
      if (character == '"') {
        ++position_;
        if (!is_valid_utf8(out)) {
          return fail_bool("string contains invalid UTF-8");
        }
        return true;
      }
      if (static_cast<unsigned char>(character) < 0x20u) {
        return fail_bool("unescaped control character in string");
      }
      if (character != '\\') {
        out.push_back(character);
        ++position_;
      } else {
        ++position_;
        if (position_ >= text_.size()) {
          return fail_bool("unterminated escape sequence");
        }
        const char escape = text_[position_++];
        switch (escape) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            std::uint32_t code_point = 0;
            if (!parse_hex4(code_point)) {
              return false;
            }
            if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
              if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                  text_[position_ + 1] != 'u') {
                return fail_bool("lone high surrogate in unicode escape");
              }
              position_ += 2;
              std::uint32_t low = 0;
              if (!parse_hex4(low)) {
                return false;
              }
              if (low < 0xDC00u || low > 0xDFFFu) {
                return fail_bool("invalid low surrogate in unicode escape");
              }
              code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
            } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
              return fail_bool("lone low surrogate in unicode escape");
            }
            if (!append_utf8(out, code_point)) {
              return fail_bool("invalid code point in unicode escape");
            }
            break;
          }
          default:
            return fail_bool("invalid escape sequence");
        }
      }
      if (out.size() > limits_.max_string_bytes) {
        return fail_bool("string exceeds the configured size limit");
      }
    }
  }

  bool parse_number(JsonValue& out) {
    const std::size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-') {
      ++position_;
    }
    if (position_ >= text_.size()) {
      return fail_bool("invalid number");
    }
    if (text_[position_] == '0') {
      ++position_;
    } else if (is_digit(text_[position_])) {
      while (position_ < text_.size() && is_digit(text_[position_])) {
        ++position_;
      }
    } else {
      return fail_bool("invalid number");
    }
    bool is_real = false;
    if (position_ < text_.size() && text_[position_] == '.') {
      is_real = true;
      ++position_;
      if (position_ >= text_.size() || !is_digit(text_[position_])) {
        return fail_bool("invalid fractional part in number");
      }
      while (position_ < text_.size() && is_digit(text_[position_])) {
        ++position_;
      }
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      is_real = true;
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= text_.size() || !is_digit(text_[position_])) {
        return fail_bool("invalid exponent in number");
      }
      while (position_ < text_.size() && is_digit(text_[position_])) {
        ++position_;
      }
    }
    const std::string_view token = text_.substr(start, position_ - start);
    const char* first = token.data();
    const char* last = token.data() + token.size();
    if (is_real) {
      double value = 0.0;
      const auto result = std::from_chars(first, last, value);
      if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(value)) {
        return fail_bool("real number out of range");
      }
      out = JsonValue::make_real(value);
      return true;
    }
    if (token.front() == '-') {
      std::int64_t value = 0;
      const auto result = std::from_chars(first, last, value);
      if (result.ec != std::errc{} || result.ptr != last) {
        return fail_bool("integer literal out of range for int64");
      }
      out = JsonValue::make_int(value);
    } else {
      std::uint64_t value = 0;
      const auto result = std::from_chars(first, last, value);
      if (result.ec != std::errc{} || result.ptr != last) {
        return fail_bool("integer literal out of range for uint64");
      }
      out = JsonValue::make_uint(value);
    }
    return true;
  }

  std::string_view text_;
  JsonLimits limits_;
  std::size_t position_{0};
  Status status_{};
};

void escape_string(std::string_view text, std::string& out) {
  out.push_back('"');
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20u) {
          static const char* kDigits = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kDigits[(byte >> 4) & 0x0Fu]);
          out.push_back(kDigits[byte & 0x0Fu]);
        } else {
          out.push_back(character);
        }
        break;
    }
  }
  out.push_back('"');
}

void write_number(const JsonValue& value, std::string& out) {
  char buffer[64];
  switch (value.kind()) {
    case JsonKind::integer_signed: {
      const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value.as_int64());
      out.append(buffer, result.ptr);
      break;
    }
    case JsonKind::integer_unsigned: {
      const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value.as_uint64());
      out.append(buffer, result.ptr);
      break;
    }
    case JsonKind::real: {
      const double real = value.as_double();
      if (!std::isfinite(real)) {
        out += "null";
        break;
      }
      const auto result = std::to_chars(buffer, buffer + sizeof(buffer), real);
      if (result.ec == std::errc{}) {
        out.append(buffer, result.ptr);
      } else {
        const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", real);
        if (written > 0) {
          out.append(buffer, static_cast<std::size_t>(written));
        }
      }
      break;
    }
    default:
      out += "null";
      break;
  }
}

void write_value(const JsonValue& value, JsonStyle style, std::size_t indent, std::string& out) {
  switch (value.kind()) {
    case JsonKind::null_value:
      out += "null";
      break;
    case JsonKind::boolean:
      out += value.as_bool() ? "true" : "false";
      break;
    case JsonKind::integer_signed:
    case JsonKind::integer_unsigned:
    case JsonKind::real:
      write_number(value, out);
      break;
    case JsonKind::string:
      escape_string(value.as_string(), out);
      break;
    case JsonKind::array: {
      const auto& elements = value.elements();
      if (elements.empty()) {
        out += "[]";
        break;
      }
      out.push_back('[');
      const std::size_t child_indent = indent + 2;
      for (std::size_t i = 0; i < elements.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        if (style == JsonStyle::pretty) {
          out.push_back('\n');
          out.append(child_indent, ' ');
        }
        write_value(elements[i], style, child_indent, out);
      }
      if (style == JsonStyle::pretty) {
        out.push_back('\n');
        out.append(indent, ' ');
      }
      out.push_back(']');
      break;
    }
    case JsonKind::object: {
      const auto& members = value.members();
      if (members.empty()) {
        out += "{}";
        break;
      }
      out.push_back('{');
      const std::size_t child_indent = indent + 2;
      for (std::size_t i = 0; i < members.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        if (style == JsonStyle::pretty) {
          out.push_back('\n');
          out.append(child_indent, ' ');
        }
        escape_string(members[i].first, out);
        out.push_back(':');
        if (style == JsonStyle::pretty) {
          out.push_back(' ');
        }
        write_value(members[i].second, style, child_indent, out);
      }
      if (style == JsonStyle::pretty) {
        out.push_back('\n');
        out.append(indent, ' ');
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

JsonValue JsonValue::make_bool(bool value) {
  JsonValue result;
  result.kind_ = JsonKind::boolean;
  result.boolean_ = value;
  return result;
}

JsonValue JsonValue::make_int(std::int64_t value) {
  JsonValue result;
  result.kind_ = JsonKind::integer_signed;
  result.integer_ = value;
  return result;
}

JsonValue JsonValue::make_uint(std::uint64_t value) {
  JsonValue result;
  result.kind_ = JsonKind::integer_unsigned;
  result.unsigned_integer_ = value;
  return result;
}

JsonValue JsonValue::make_real(double value) {
  JsonValue result;
  result.kind_ = JsonKind::real;
  result.real_ = value;
  return result;
}

JsonValue JsonValue::make_string(std::string value) {
  JsonValue result;
  result.kind_ = JsonKind::string;
  result.text_ = std::move(value);
  return result;
}

JsonValue JsonValue::make_array(array_type values) {
  JsonValue result;
  result.kind_ = JsonKind::array;
  result.array_ = std::move(values);
  return result;
}

JsonValue JsonValue::make_object(object_type members) {
  std::stable_sort(members.begin(), members.end(),
                   [](const member_type& lhs, const member_type& rhs) {
                     return lhs.first < rhs.first;
                   });
  object_type collapsed;
  collapsed.reserve(members.size());
  for (auto& member : members) {
    if (!collapsed.empty() && collapsed.back().first == member.first) {
      collapsed.back().second = std::move(member.second);
    } else {
      collapsed.push_back(std::move(member));
    }
  }
  JsonValue result;
  result.kind_ = JsonKind::object;
  result.object_ = std::move(collapsed);
  return result;
}

bool JsonValue::is_number() const noexcept {
  return kind_ == JsonKind::integer_signed || kind_ == JsonKind::integer_unsigned ||
         kind_ == JsonKind::real;
}

bool JsonValue::is_integer() const noexcept {
  return kind_ == JsonKind::integer_signed || kind_ == JsonKind::integer_unsigned;
}

bool JsonValue::as_bool(bool fallback) const noexcept {
  return kind_ == JsonKind::boolean ? boolean_ : fallback;
}

std::int64_t JsonValue::as_int64(std::int64_t fallback) const noexcept {
  switch (kind_) {
    case JsonKind::integer_signed:
      return integer_;
    case JsonKind::integer_unsigned:
      return unsigned_integer_ <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                 ? static_cast<std::int64_t>(unsigned_integer_)
                 : fallback;
    case JsonKind::real:
      return std::isfinite(real_) &&
                     real_ >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
                     real_ <= static_cast<double>(std::numeric_limits<std::int64_t>::max())
                 ? static_cast<std::int64_t>(real_)
                 : fallback;
    default:
      return fallback;
  }
}

std::uint64_t JsonValue::as_uint64(std::uint64_t fallback) const noexcept {
  switch (kind_) {
    case JsonKind::integer_signed:
      return integer_ >= 0 ? static_cast<std::uint64_t>(integer_) : fallback;
    case JsonKind::integer_unsigned:
      return unsigned_integer_;
    case JsonKind::real:
      return std::isfinite(real_) && real_ >= 0.0 &&
                     real_ <= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
                 ? static_cast<std::uint64_t>(real_)
                 : fallback;
    default:
      return fallback;
  }
}

double JsonValue::as_double(double fallback) const noexcept {
  switch (kind_) {
    case JsonKind::integer_signed:
      return static_cast<double>(integer_);
    case JsonKind::integer_unsigned:
      return static_cast<double>(unsigned_integer_);
    case JsonKind::real:
      return real_;
    default:
      return fallback;
  }
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  for (const auto& member : object_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

const JsonValue& JsonValue::at(std::size_t index) const noexcept {
  static const JsonValue kNull{};
  if (index >= array_.size()) {
    return kNull;
  }
  return array_[index];
}

std::size_t JsonValue::size() const noexcept {
  switch (kind_) {
    case JsonKind::array:
      return array_.size();
    case JsonKind::object:
      return object_.size();
    case JsonKind::string:
      return text_.size();
    default:
      return 0;
  }
}

Result<JsonValue> parse_json(std::string_view text, const JsonLimits& limits) {
  Parser parser(text, limits);
  return parser.parse_document();
}

std::string write_json(const JsonValue& value, JsonStyle style) {
  std::string out;
  write_value(value, style, 0, out);
  if (style == JsonStyle::pretty) {
    out.push_back('\n');
  }
  return out;
}

}  // namespace cplan
