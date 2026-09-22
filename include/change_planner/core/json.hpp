#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "change_planner/core/status.hpp"
#include "change_planner/core/utf8.hpp"

namespace cplan {

// Minimal, strict, dependency-free JSON model used for CLI documents and
// fixtures. Objects always store members sorted by key, so writing a value twice
// produces byte-identical output (canonical form).
enum class JsonKind : std::uint8_t {
  null_value = 0,
  boolean,
  integer_signed,
  integer_unsigned,
  real,
  string,
  array,
  object,
};

enum class JsonStyle : std::uint8_t { compact = 0, pretty };

struct JsonLimits {
  std::size_t max_bytes = 8u * 1024u * 1024u;
  std::size_t max_depth = 64;
  std::size_t max_string_bytes = 1u * 1024u * 1024u;
  std::size_t max_elements = 1u * 1024u * 1024u;
};

class JsonValue {
 public:
  using array_type = std::vector<JsonValue>;
  using member_type = std::pair<std::string, JsonValue>;
  using object_type = std::vector<member_type>;

  JsonValue() = default;

  static JsonValue make_null() { return JsonValue{}; }
  static JsonValue make_bool(bool value);
  static JsonValue make_int(std::int64_t value);
  static JsonValue make_uint(std::uint64_t value);
  static JsonValue make_real(double value);
  static JsonValue make_string(std::string value);
  static JsonValue make_array(array_type values);
  // Sorts members by key; a repeated key keeps the last value supplied.
  static JsonValue make_object(object_type members);

  JsonKind kind() const noexcept { return kind_; }
  bool is_null() const noexcept { return kind_ == JsonKind::null_value; }
  bool is_bool() const noexcept { return kind_ == JsonKind::boolean; }
  bool is_number() const noexcept;
  bool is_integer() const noexcept;
  bool is_string() const noexcept { return kind_ == JsonKind::string; }
  bool is_array() const noexcept { return kind_ == JsonKind::array; }
  bool is_object() const noexcept { return kind_ == JsonKind::object; }

  bool as_bool(bool fallback = false) const noexcept;
  std::int64_t as_int64(std::int64_t fallback = 0) const noexcept;
  std::uint64_t as_uint64(std::uint64_t fallback = 0) const noexcept;
  double as_double(double fallback = 0.0) const noexcept;
  const std::string& as_string() const noexcept { return text_; }

  const array_type& elements() const noexcept { return array_; }
  const object_type& members() const noexcept { return object_; }
  const JsonValue* find(std::string_view key) const noexcept;
  const JsonValue& at(std::size_t index) const noexcept;

  std::size_t size() const noexcept;

  friend bool operator==(const JsonValue&, const JsonValue&) = default;

 private:
  JsonKind kind_{JsonKind::null_value};
  bool boolean_{false};
  std::int64_t integer_{0};
  std::uint64_t unsigned_integer_{0};
  double real_{0.0};
  std::string text_{};
  array_type array_{};
  object_type object_{};
};

// Strict RFC 8259 parsing with bounded depth, bounded sizes, rejection of
// duplicate object keys, rejection of invalid UTF-8, and rejection of trailing
// content.
[[nodiscard]] Result<JsonValue> parse_json(std::string_view text, const JsonLimits& limits = {});

[[nodiscard]] std::string write_json(const JsonValue& value, JsonStyle style = JsonStyle::compact);

}  // namespace cplan
