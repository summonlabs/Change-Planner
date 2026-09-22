#pragma once

#include <string>
#include <vector>

#include "change_planner/model/constraints.hpp"
#include "change_planner/model/evidence.hpp"
#include "change_planner/model/target.hpp"
#include "change_planner/plan/generate.hpp"
#include "change_planner/safety/checker.hpp"

namespace cplan {

// Operations derived from the delta between current state and target intent,
// together with the notes explaining what was skipped and why.
struct DerivedOperations {
  std::vector<Operation> operations;
  std::vector<std::string> notes;
};

// Declarative target minus current state, expressed as typed operations.
// Deterministic: the same inputs always produce the same operation list in the
// same order.
[[nodiscard]] Result<DerivedOperations> derive_operations(const CurrentStateSnapshot& state,
                                                          const TargetIntent& target,
                                                          const EvaluationContext& context);

// Static dependency derivation. Two steps conflict when one writes what the
// other writes or reads, when a structural prerequisite exists (drain before
// removal, device before link, link before route, route before binding), or when
// the declared change-serialization obligations forbid parallel execution.
struct ConflictAnalysis {
  std::vector<DependencyEdge> edges;
  std::vector<RejectedAlternative> rejected;
};

[[nodiscard]] Result<ConflictAnalysis> derive_dependencies(const CurrentStateSnapshot& state,
                                                           const std::vector<Operation>& operations,
                                                           const EvaluationContext& context);

// Deterministic step identity derived from the operation content.
[[nodiscard]] StepId step_id_for(const Operation& operation);

// Compensation metadata for an operation, derived from the state it is applied
// to.
[[nodiscard]] Compensation derive_compensation(const CurrentStateSnapshot& state,
                                               const Operation& operation,
                                               const EvaluationContext& context);

// Risk score and duration estimate for an operation. Vendor-neutral defaults
// derived from the operation class and the declared limits.
[[nodiscard]] RiskScore derive_risk(const Operation& operation);
[[nodiscard]] DurationNs derive_duration(const Operation& operation);

}  // namespace cplan
