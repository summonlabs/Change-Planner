#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "change_planner/model/state.hpp"
#include "change_planner/model/target.hpp"

namespace cplan {

// Every safety obligation the planner enforces is an explicit, individually
// addressable constraint item. Refusals name the items that block a plan, so an
// operator can see exactly which declared obligation has no safe schedule.
enum class ConstraintKind : std::uint8_t {
  connectivity = 0,
  failure_domain_redundancy,
  path_diversity,
  capacity_headroom,
  maintenance_exclusion,
  maintenance_window,
  drain_before_maintenance,
  change_serialization,
  workload_contract,
  capability_evidence,
  rollback_metadata,
  generation_binding,
};

const char* to_string(ConstraintKind kind) noexcept;
Result<ConstraintKind> parse_constraint_kind(std::string_view token);

struct ConstraintItem {
  ConstraintId id;
  ConstraintKind kind{ConstraintKind::connectivity};
  bool enabled{true};
  // capacity_headroom: required free headroom as parts-per-thousand.
  Permille headroom;
  // change_serialization: maximum devices simultaneously out of service in one
  // failure domain.
  std::uint32_t max_concurrent_changes{1};
  // maintenance_exclusion: maximum failure domains allowed in maintenance at once.
  std::uint32_t max_domains_in_maintenance{1};
  // Optional scope filters / parameters (contract identity, domain identity).
  std::string parameter_code;
  std::string justification_code;

  friend bool operator==(const ConstraintItem&, const ConstraintItem&) = default;
  friend bool operator<(const ConstraintItem& lhs, const ConstraintItem& rhs) {
    if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
    return lhs.id < rhs.id;
  }
};

// Workload/network contracts supplied as planning inputs. A contract states the
// service obligations that must hold for a set of workloads at every point of
// the transition.
struct NetworkContract {
  ContractId id;
  std::vector<WorkloadId> workloads;
  CapacityUnits required_units;
  std::uint32_t min_disjoint_domains{0};
  std::uint32_t max_simultaneous_domain_maintenance{1};
  bool forbid_concurrent_changes_in_same_domain{true};
  std::string sla_code;

  friend bool operator==(const NetworkContract&, const NetworkContract&) = default;
};

// Declared tolerances used by plan invalidation. Movement beyond these bounds
// invalidates a plan instead of silently reusing it.
struct ChangeTolerance {
  Permille topology_drift{0};
  Permille capacity_drift{0};
  Permille demand_drift{0};
  std::uint32_t max_added_entities{0};
  std::uint32_t max_removed_entities{0};
  bool allow_capability_growth{false};
  bool allow_capability_shrink{false};
  std::vector<std::string> exempt_identities;

  friend bool operator==(const ChangeTolerance&, const ChangeTolerance&) = default;
};

struct SafetyConstraints {
  ConstraintId id;
  AuthorityGeneration authority_generation;
  PolicyId governing_policy;
  Digest governing_policy_digest;
  std::vector<ConstraintItem> items;
  std::vector<NetworkContract> contracts;
  ChangeTolerance tolerance;

  friend bool operator==(const SafetyConstraints&, const SafetyConstraints&) = default;

  [[nodiscard]] Result<void> validate() const;
  [[nodiscard]] Digest digest() const;

  [[nodiscard]] bool enforced(ConstraintKind kind) const;
  [[nodiscard]] const ConstraintItem* find(ConstraintKind kind) const;
  [[nodiscard]] std::vector<const ConstraintItem*> all(ConstraintKind kind) const;

  void canonicalize();
  void encode(CanonicalEncoder& out) const;
  static SafetyConstraints decode(CanonicalDecoder& in);
};

// Convenience constructor used by fixtures, examples and the CLI: a constraint
// set with every obligation enabled at the supplied limits.
[[nodiscard]] SafetyConstraints default_constraints(Permille headroom,
                                                    std::uint32_t max_concurrent_changes,
                                                    std::uint32_t max_domains_in_maintenance);

}  // namespace cplan
