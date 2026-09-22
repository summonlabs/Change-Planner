#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "change_planner/core/status.hpp"

namespace cplan {
namespace detail_checked {

template <class T>
[[nodiscard]] inline Result<T> overflow(const char* operation) {
  return Status::error(ErrorCode::size_limit,
                       std::string("checked arithmetic overflow in ") + operation);
}

}  // namespace detail_checked

// All externally derived sizes and counts flow through these helpers so that
// hostile or corrupt inputs cannot wrap a counter into a small value.
[[nodiscard]] inline Result<std::uint64_t> checked_add_u64(std::uint64_t lhs, std::uint64_t rhs) {
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
    return detail_checked::overflow<std::uint64_t>("add");
  }
  return lhs + rhs;
}

[[nodiscard]] inline Result<std::uint64_t> checked_sub_u64(std::uint64_t lhs, std::uint64_t rhs) {
  if (lhs < rhs) {
    return detail_checked::overflow<std::uint64_t>("sub");
  }
  return lhs - rhs;
}

[[nodiscard]] inline Result<std::uint64_t> checked_mul_u64(std::uint64_t lhs, std::uint64_t rhs) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    return detail_checked::overflow<std::uint64_t>("mul");
  }
  return lhs * rhs;
}

[[nodiscard]] inline Result<std::size_t> checked_add_size(std::size_t lhs, std::size_t rhs) {
  if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
    return detail_checked::overflow<std::size_t>("add");
  }
  return lhs + rhs;
}

[[nodiscard]] inline Result<std::size_t> checked_mul_size(std::size_t lhs, std::size_t rhs) {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return detail_checked::overflow<std::size_t>("mul");
  }
  return lhs * rhs;
}

[[nodiscard]] inline Result<std::size_t> checked_size_from_u64(std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return detail_checked::overflow<std::size_t>("narrow");
  }
  return static_cast<std::size_t>(value);
}

// n! computed with overflow detection; used to bound exhaustive interleaving
// proofs over a stage of n concurrently applied steps.
[[nodiscard]] inline Result<std::uint64_t> checked_factorial(std::uint32_t n) {
  std::uint64_t result = 1;
  for (std::uint32_t i = 2; i <= n; ++i) {
    auto next = checked_mul_u64(result, i);
    if (!next.ok()) {
      return next.status();
    }
    result = next.value();
  }
  return result;
}

}  // namespace cplan
