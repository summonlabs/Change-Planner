#include "change_planner/core/status.hpp"

namespace cplan {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::ok:
      return "ok";
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::malformed_input:
      return "malformed_input";
    case ErrorCode::unsupported_version:
      return "unsupported_version";
    case ErrorCode::integrity_failure:
      return "integrity_failure";
    case ErrorCode::truncated:
      return "truncated";
    case ErrorCode::size_limit:
      return "size_limit";
    case ErrorCode::duplicate_identity:
      return "duplicate_identity";
    case ErrorCode::stale_generation:
      return "stale_generation";
    case ErrorCode::stale_authority:
      return "stale_authority";
    case ErrorCode::stale_incarnation:
      return "stale_incarnation";
    case ErrorCode::stale_revision:
      return "stale_revision";
    case ErrorCode::not_found:
      return "not_found";
    case ErrorCode::conflict:
      return "conflict";
    case ErrorCode::capacity_exceeded:
      return "capacity_exceeded";
    case ErrorCode::unsafe_transition:
      return "unsafe_transition";
    case ErrorCode::no_plan:
      return "no_plan";
    case ErrorCode::contradictory_constraints:
      return "contradictory_constraints";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::limit_exceeded:
      return "limit_exceeded";
    case ErrorCode::internal_invariant:
      return "internal_invariant";
    case ErrorCode::unsupported_capability:
      return "unsupported_capability";
  }
  return "unknown";
}

std::string Status::to_string() const {
  if (ok()) {
    return "ok";
  }
  return std::string(cplan::to_string(code_)) + ": " + message_;
}

}  // namespace cplan
