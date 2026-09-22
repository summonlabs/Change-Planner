#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

#include "change_planner/core/status.hpp"

namespace cplan {

// Domain identities are never interchangeable: each entity kind has its own tag
// type, so a LinkId cannot be passed where a DeviceId is expected.
inline constexpr std::size_t kMaxIdentityLength = 128;

// Identity text is restricted to printable, unquoted ASCII so that identities
// survive JSON, canonical binary encoding, CLI arguments and log lines
// unchanged.
[[nodiscard]] bool is_valid_identity_text(std::string_view text) noexcept;

template <class Tag>
class NameId {
 public:
  NameId() = default;

  static Result<NameId> parse(std::string_view text);

  // Construction from text already proven valid by a parser/decoder path.
  static NameId from_validated(std::string text);

  const std::string& str() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }
  std::size_t size() const noexcept { return value_.size(); }

  std::string to_string() const { return value_; }
  std::size_t hash() const noexcept { return std::hash<std::string>{}(value_); }

  friend bool operator==(const NameId&, const NameId&) = default;
  friend std::strong_ordering operator<=>(const NameId& lhs, const NameId& rhs) {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  std::string value_{};
};

template <class Tag>
Result<NameId<Tag>> NameId<Tag>::parse(std::string_view text) {
  if (!is_valid_identity_text(text)) {
    return Status::error(ErrorCode::invalid_argument,
                         "identity text rejected for " + std::string(Tag::kind_name()) + ": '" +
                             std::string(text.substr(0, 160)) + "'");
  }
  return NameId<Tag>::from_validated(std::string(text));
}

template <class Tag>
NameId<Tag> NameId<Tag>::from_validated(std::string text) {
  NameId<Tag> id;
  id.value_ = std::move(text);
  return id;
}

// Fixed-width scalar identity/quantity types (generations, epochs, attempts,
// revisions, capacities). Arithmetic is explicit and overflow checked at call
// sites through core/checked.hpp.
template <class Tag, class Rep>
class Scalar {
 public:
  using rep_type = Rep;

  constexpr Scalar() noexcept = default;
  constexpr explicit Scalar(Rep value) noexcept : value_(value) {}

  constexpr Rep value() const noexcept { return value_; }

  friend constexpr bool operator==(Scalar lhs, Scalar rhs) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Scalar lhs, Scalar rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  Rep value_{};
};

// Increment helper that refuses to wrap. Generations must never silently roll
// over: an exhausted counter is a hard error.
template <class Tag, class Rep>
[[nodiscard]] inline Result<Scalar<Tag, Rep>> checked_increment(Scalar<Tag, Rep> value) {
  static_assert(std::numeric_limits<Rep>::is_integer, "Scalar requires an integer representation");
  if (value.value() == std::numeric_limits<Rep>::max()) {
    return Status::error(ErrorCode::limit_exceeded,
                         std::string("counter exhausted: ") + Tag::kind_name());
  }
  return Scalar<Tag, Rep>(static_cast<Rep>(value.value() + 1));
}

}  // namespace cplan

namespace std {

template <class Tag>
struct hash<::cplan::NameId<Tag>> {
  std::size_t operator()(const ::cplan::NameId<Tag>& id) const noexcept { return id.hash(); }
};

template <class Tag, class Rep>
struct hash<::cplan::Scalar<Tag, Rep>> {
  std::size_t operator()(const ::cplan::Scalar<Tag, Rep>& value) const noexcept {
    return std::hash<Rep>{}(value.value());
  }
};

}  // namespace std
