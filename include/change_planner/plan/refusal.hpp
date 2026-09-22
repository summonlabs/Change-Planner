#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "change_planner/model/constraints.hpp"
#include "change_planner/plan/plan.hpp"

namespace cplan {

// Why no safe plan exists. A refusal always names the obligations or structural
// facts that block every ordering; it never fabricates a path.
enum class RefusalCode : std::uint8_t {
  accepted = 0,
  input_invalid,
  stale_input,
  unsatisfiable_target,
  impossible_transition,
  capability_missing,
  unsafe_initial_state,
  no_safe_ordering,
  contradictory_constraints,
  search_limit_exceeded,
  cancelled,
  internal_verification_failed,
};

const char* to_string(RefusalCode code) noexcept;
Result<RefusalCode> parse_refusal_code(std::string_view token);

// A single obligation that blocks every candidate ordering.
struct BlockingConstraint {
  ConstraintKind kind{ConstraintKind::connectivity};
  ConstraintId id;
  InvariantId invariant{InvariantId::connectivity};
  std::vector<EntityRef> evidence;
  std::string message;
  std::string justification_code;

  friend bool operator==(const BlockingConstraint&, const BlockingConstraint&) = default;
  friend bool operator<(const BlockingConstraint& lhs, const BlockingConstraint& rhs) {
    if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
    return lhs.id < rhs.id;
  }

  void encode(CanonicalEncoder& out) const;
  static BlockingConstraint decode(CanonicalDecoder& in);
};

// A step that could not be scheduled, with the reasons observed in the planner
// model.
struct BlockedStep {
  StepId id;
  Operation operation;
  std::vector<Violation> violations;
  std::vector<Condition> unsatisfied_preconditions;
  std::string blocking_code;

  friend bool operator==(const BlockedStep&, const BlockedStep&) = default;

  void encode(CanonicalEncoder& out) const;
  static BlockedStep decode(CanonicalDecoder& in);
};

struct Refusal {
  RefusalCode code{RefusalCode::accepted};
  std::string summary_code;
  Digest request_digest;
  ValidityBinding binding;
  std::vector<BlockingConstraint> blocking;
  std::vector<Violation> residual_violations;
  std::vector<BlockedStep> blocked_steps;
  std::vector<RejectedAlternative> rejected;
  Justification justification;

  friend bool operator==(const Refusal&, const Refusal&) = default;

  // Deterministic, human-facing explanation of the blocking obligations.
  [[nodiscard]] std::string explain() const;
  [[nodiscard]] bool minimal_core_computed() const { return !blocking.empty(); }

  void encode(CanonicalEncoder& out) const;
  static Refusal decode(CanonicalDecoder& in);
};

struct PlanDiagnostics {
  std::uint64_t candidates_evaluated{0};
  std::uint64_t scheduling_attempts{0};
  std::uint64_t states_visited{0};
  std::uint64_t permutation_checks{0};
  std::uint64_t invariant_evaluations{0};
  std::uint64_t core_minimisation_attempts{0};
  bool window_shift_applied{false};
};

// The outcome of a planning run: exactly one of a plan or a refusal.
struct PlanResult {
  std::variant<Plan, Refusal> outcome;
  PlanDiagnostics diagnostics;

  [[nodiscard]] bool ok() const { return std::holds_alternative<Plan>(outcome); }
  [[nodiscard]] const Plan& plan() const { return std::get<Plan>(outcome); }
  [[nodiscard]] const Refusal& refusal() const { return std::get<Refusal>(outcome); }

  static PlanResult from_plan(Plan plan, PlanDiagnostics diagnostics = {});
  static PlanResult from_refusal(Refusal refusal, PlanDiagnostics diagnostics = {});
};

}  // namespace cplan
