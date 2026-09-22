#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "change_planner/plan/generate.hpp"
#include "change_planner/plan/verify.hpp"

namespace cplan {

// What the executing runtime observed for a step. Nothing is assumed: a step
// reported as succeeded but absent from the observed state is planned again, and
// an unknown or partially applied step is compensated rather than trusted.
enum class StepOutcome : std::uint8_t {
  succeeded = 0,
  failed,
  skipped,
  unknown,
  partially_applied,
};

const char* to_string(StepOutcome outcome) noexcept;
Result<StepOutcome> parse_step_outcome(std::string_view token);

struct StepObservation {
  StepId step;
  AttemptNumber attempt;
  StepOutcome outcome{StepOutcome::unknown};
  std::string detail_code;

  friend bool operator==(const StepObservation&, const StepObservation&) = default;
  friend bool operator<(const StepObservation& lhs, const StepObservation& rhs) {
    return lhs.step < rhs.step;
  }
};

struct ObservedExecution {
  PlanId plan_id;
  PlanGeneration plan_generation;
  AttemptNumber attempt;
  AuthorityGeneration authority_generation;
  TopologyGeneration observed_generation;
  BootIncarnation boot_incarnation;
  Epoch observed_epoch;
  TimestampNs observed_at;
  CurrentStateSnapshot observed_state;
  std::vector<StepObservation> observations;

  friend bool operator==(const ObservedExecution&, const ObservedExecution&) = default;

  [[nodiscard]] Result<void> validate() const;
  [[nodiscard]] Digest digest() const;
};

struct ReplanningOptions {
  // Compensate steps that were observed partially applied before re-attempting
  // their effect.
  bool compensate_partial_steps{true};
  PlanningLimits limits;
  std::string label;
};

// The exact request a continuation is generated from: observed state, rebased
// target intent and evidence, replan lineage and the next plan generation. It is
// exposed so callers can verify or re-validate a persisted continuation with
// precisely the inputs it was bound to.
[[nodiscard]] PlanRequest replan_request_for(const Plan& prior, const ObservedExecution& observed,
                                             const PlanRequest& base_request,
                                             const ReplanningOptions& options = {});

// The exact request a rollback plan is generated from: the observed state as the
// start state and the pre-plan state as the target.
[[nodiscard]] PlanRequest rollback_request_for(const Plan& prior, const ObservedExecution& observed,
                                               const PlanRequest& base_request,
                                               const ReplanningOptions& options = {});

// Replans from an observed partial execution state. The prior plan is fenced by
// identity, generation, attempt and authority; stale observations are refused
// rather than reinterpreted.
[[nodiscard]] PlanResult replan(const Plan& prior, const ObservedExecution& observed,
                                const PlanRequest& base_request,
                                const ReplanningOptions& options = {});

// Builds a compensation plan that returns the fabric to the state the prior plan
// started from, using the rollback metadata carried by its steps.
[[nodiscard]] PlanResult build_rollback_plan(const Plan& prior, const ObservedExecution& observed,
                                             const PlanRequest& base_request,
                                             const ReplanningOptions& options = {});

}  // namespace cplan
