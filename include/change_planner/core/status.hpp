#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace cplan {

// Every expected failure in the runtime is reported through a typed code. The
// codes are part of the diagnostic surface and stay stable.
enum class ErrorCode : std::uint8_t {
  ok = 0,
  invalid_argument,
  malformed_input,
  unsupported_version,
  integrity_failure,
  truncated,
  size_limit,
  duplicate_identity,
  stale_generation,
  stale_authority,
  stale_incarnation,
  stale_revision,
  not_found,
  conflict,
  capacity_exceeded,
  unsafe_transition,
  no_plan,
  contradictory_constraints,
  cancelled,
  limit_exceeded,
  internal_invariant,
  unsupported_capability,
};

const char* to_string(ErrorCode code) noexcept;

class Status {
 public:
  constexpr Status() noexcept = default;

  static Status success() noexcept { return Status(); }

  static Status error(ErrorCode code, std::string message) {
    Status status;
    status.code_ = code;
    status.message_ = std::move(message);
    return status;
  }

  bool ok() const noexcept { return code_ == ErrorCode::ok; }
  ErrorCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

  // "code: message" rendered form, used by diagnostics and the CLI.
  std::string to_string() const;

 private:
  ErrorCode code_{ErrorCode::ok};
  std::string message_{};
};

// Result<T> carries either a value or a Status. It never carries both, and it
// never throws for expected failures.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Status status) : storage_(std::move(status)) {}

  bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  explicit operator bool() const noexcept { return ok(); }
  operator Status() const { return status(); }

  const T& value() const { return std::get<T>(storage_); }
  T& value() { return std::get<T>(storage_); }
  const T& operator*() const { return value(); }
  T& operator*() { return value(); }
  const T* operator->() const { return &value(); }
  T* operator->() { return &value(); }

  const Status& status() const {
    if (ok()) {
      static const Status kOkStatus{};
      return kOkStatus;
    }
    return std::get<Status>(storage_);
  }

  T value_or(T fallback) const { return ok() ? value() : std::move(fallback); }

 private:
  std::variant<T, Status> storage_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}

  bool ok() const noexcept { return status_.ok(); }
  explicit operator bool() const noexcept { return ok(); }
  operator Status() const { return status_; }

  const Status& status() const noexcept { return status_; }

 private:
  Status status_{};
};

// Propagates an expected failure out of the current function. Accepts both
// Status and Result<T> expressions.
#define CPLAN_TRY(expression)                         \
  do {                                                \
    ::cplan::Status cplan_try_status_ = (expression); \
    if (!cplan_try_status_.ok()) {                    \
      return cplan_try_status_;                       \
    }                                                 \
  } while (false)

// Verifies a runtime condition that must always hold. Failure is reported as
// Status::error(internal_invariant, ...) instead of terminating the process, so
// that callers and tests observe a diagnosable result.
#define CPLAN_VERIFY(condition, message)                                            \
  do {                                                                              \
    if (!(condition)) {                                                             \
      return ::cplan::Status::error(::cplan::ErrorCode::internal_invariant,         \
                                    std::string("invariant violated: ") + (message)); \
    }                                                                               \
  } while (false)

}  // namespace cplan
