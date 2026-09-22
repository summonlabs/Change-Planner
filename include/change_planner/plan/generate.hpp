#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "change_planner/core/cancel.hpp"
#include "change_planner/model/constraints.hpp"
#include "change_planner/model/evidence.hpp"
#include "change_planner/model/target.hpp"
#include "change_planner/plan/objective.hpp"
#include "change_planner/plan/refusal.hpp"

namespace cplan {

// Bounded resource surfaces of the planner. Every limit is enforced with checked
// arithmetic; exceeding a limit produces a refusal, never an unchecked plan.
struct PlanningLimits {
  std::uint32_t max_steps{4096};
  std::uint32_t max_candidates{4};
  std::uint32_t max_stage_width{4};
  std::uint32_t max_scheduling_attempts{64};
  std::uint32_t max_core_minimisation_attempts{64};
  std::uint64_t max_permutation_checks{2000000};
  std::uint64_t max_states_visited{2000000};
  bool allow_window_shift{false};

  [[nodiscard]] Result<void> validate() const;
  [[nodiscard]] Digest digest() const;
};

// A complete, generation-bound planning request. The planner never reads the
// wall clock: the planning instant is supplied by the caller.
struct PlanRequest {
  PlanId plan_id;
  PlanGeneration generation;
  TimestampNs planning_instant;
  CurrentStateSnapshot state;
  TargetIntent target;
  SafetyConstraints constraints;
  TopologyEvidence evidence;
  Objective objective;
  PlanningLimits limits;
  PlanLineage lineage;
  std::string label;

  [[nodiscard]] Result<void> validate() const;
  [[nodiscard]] ValidityBinding binding() const;
  [[nodiscard]] Digest request_digest() const;
};

// Generates a safe ordered transition plan, or refuses with a minimal
// explanation. Cancellation is observed at bounded intervals; a cancelled run
// never publishes a plan.
[[nodiscard]] PlanResult generate_plan(const PlanRequest& request, const StopToken& stop = {});

}  // namespace cplan
