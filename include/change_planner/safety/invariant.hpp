#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "change_planner/model/operation.hpp"

namespace cplan {

// The invariants the planner proves for every emitted step. Each one maps to a
// declared constraint kind, so a violation always names the obligation it
// breaks.
enum class InvariantId : std::uint8_t {
  connectivity = 0,
  failure_domain_redundancy,
  path_diversity,
  capacity_headroom,
  maintenance_exclusion,
  maintenance_window,
  change_serialization,
  workload_contract,
  capability_evidence,
  generation_binding,
  service_continuity,
};

const char* to_string(InvariantId id) noexcept;
Result<InvariantId> parse_invariant_id(std::string_view token);

enum class Severity : std::uint8_t { violation = 0, note };

const char* to_string(Severity severity) noexcept;

struct Violation {
  InvariantId invariant{InvariantId::connectivity};
  Severity severity{Severity::violation};
  std::vector<EntityRef> evidence;
  std::string detail_code;
  std::string message;

  friend bool operator==(const Violation&, const Violation&) = default;
  friend bool operator<(const Violation& lhs, const Violation& rhs) {
    if (lhs.invariant != rhs.invariant) return lhs.invariant < rhs.invariant;
    if (lhs.detail_code != rhs.detail_code) return lhs.detail_code < rhs.detail_code;
    if (lhs.evidence != rhs.evidence) return lhs.evidence < rhs.evidence;
    return lhs.message < rhs.message;
  }

  [[nodiscard]] std::string to_string() const;
};

}  // namespace cplan
