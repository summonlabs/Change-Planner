#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "change_planner/plan/generate.hpp"

namespace cplan {

// Independent verification of a plan: every stage is replayed under every
// interleaving of its steps, checking preconditions before each application,
// postconditions afterwards and the full invariant set after every step.
struct VerificationReport {
  bool ok{false};
  std::uint64_t steps_applied{0};
  std::uint64_t permutations_verified{0};
  std::uint64_t invariant_evaluations{0};
  Digest trace_digest;
  std::vector<Violation> violations;
  std::vector<Condition> unsatisfied_conditions;
  StepId failing_step;
  StageIndex failing_stage;
  std::string message;

  [[nodiscard]] std::string explain() const;
};

// The plan supplies the planning instant it was scheduled for (which may differ
// from the request when a maintenance window shift was applied); state, target,
// constraints and evidence must match the request exactly.
//
// exhaustive_interleavings enumerates every permutation of each stage. When it
// is false only the canonical order is replayed (used for cheap re-validation of
// persisted plans).
[[nodiscard]] VerificationReport verify_plan(const Plan& plan, const PlanRequest& request,
                                             bool exhaustive_interleavings = true);

// Applies a prefix of the plan in the canonical order, checking the same
// conditions and invariants. Used by replanning and by the CLI.
[[nodiscard]] Result<CurrentStateSnapshot> replay_prefix(const Plan& plan,
                                                         const PlanRequest& request,
                                                         const std::vector<StepId>& steps,
                                                         std::vector<Violation>* violations);

}  // namespace cplan
