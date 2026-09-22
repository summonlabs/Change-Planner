#pragma once

#include <string>
#include <vector>

#include "change_planner/model/operation.hpp"
#include "change_planner/model/state.hpp"
#include "change_planner/safety/checker.hpp"

namespace cplan {

// Result of structurally applying an operation. Structural feasibility only:
// safety obligations are evaluated separately by the safety checker so that the
// planner can distinguish "impossible" from "unsafe right now".
struct AppliedOperation {
  CurrentStateSnapshot state;
  // Effects that the operation produced, in canonical order.
  std::vector<EntityRef> effects;
};

// Applies an operation to a snapshot. Fails with a typed, human-readable status
// when the transition is impossible (missing entity, dangling reference,
// duplicate identity, unwalkable route, capability absent).
[[nodiscard]] Result<AppliedOperation> apply_operation(const CurrentStateSnapshot& state,
                                                       const Operation& operation,
                                                       const EvaluationContext& context);

// Preconditions and postconditions the planner attaches to a step. They are
// derived from the operation and the state it is applied to, and are re-checked
// during plan verification.
[[nodiscard]] std::vector<Condition> derive_preconditions(const CurrentStateSnapshot& state,
                                                          const Operation& operation,
                                                          const EvaluationContext& context);
[[nodiscard]] std::vector<Condition> derive_postconditions(const CurrentStateSnapshot& state,
                                                           const Operation& operation,
                                                           const EvaluationContext& context);

// Evaluates a single condition. A condition that cannot be evaluated against a
// state (for example an unknown parameter token) fails rather than defaulting to
// true.
[[nodiscard]] Result<bool> evaluate_condition(const CurrentStateSnapshot& state,
                                              const Condition& condition,
                                              const EvaluationContext& context);

}  // namespace cplan
