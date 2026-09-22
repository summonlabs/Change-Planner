#include "change_planner/plan/replan.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "change_planner/core/checked.hpp"
#include "change_planner/model/evaluate.hpp"
#include "change_planner/plan/graph.hpp"
#include "change_planner/plan/invalidate.hpp"

namespace cplan {

const char* to_string(StepOutcome outcome) noexcept {
  switch (outcome) {
    case StepOutcome::succeeded:
      return "succeeded";
    case StepOutcome::failed:
      return "failed";
    case StepOutcome::skipped:
      return "skipped";
    case StepOutcome::unknown:
      return "unknown";
    case StepOutcome::partially_applied:
      return "partially-applied";
  }
  return "unknown";
}

Result<StepOutcome> parse_step_outcome(std::string_view token) {
  if (token == "succeeded") return StepOutcome::succeeded;
  if (token == "failed") return StepOutcome::failed;
  if (token == "skipped") return StepOutcome::skipped;
  if (token == "unknown") return StepOutcome::unknown;
  if (token == "partially-applied") return StepOutcome::partially_applied;
  return Status::error(ErrorCode::malformed_input,
                       "unknown step outcome token: " + std::string(token));
}

Result<void> ObservedExecution::validate() const {
  if (plan_id.empty()) {
    return Status::error(ErrorCode::invalid_argument, "observed execution has no plan identity");
  }
  if (plan_generation.value() == 0) {
    return Status::error(ErrorCode::invalid_argument,
                         "observed execution has no plan generation");
  }
  if (attempt.value() == 0) {
    return Status::error(ErrorCode::invalid_argument,
                         "observed execution attempt numbers start at one");
  }
  CPLAN_TRY(observed_state.validate());
  if (!(observed_state.authority_generation == authority_generation)) {
    return Status::error(ErrorCode::stale_authority,
                         "observed execution authority generation " +
                             std::to_string(authority_generation.value()) +
                             " disagrees with the observed state generation " +
                             std::to_string(observed_state.authority_generation.value()));
  }
  if (!(observed_state.topology_generation == observed_generation)) {
    return Status::error(ErrorCode::stale_generation,
                         "observed execution topology generation " +
                             std::to_string(observed_generation.value()) +
                             " disagrees with the observed state generation " +
                             std::to_string(observed_state.topology_generation.value()));
  }
  std::set<std::string> seen;
  for (const StepObservation& observation : observations) {
    if (observation.step.empty()) {
      return Status::error(ErrorCode::invalid_argument, "observation without a step identity");
    }
    if (!seen.insert(observation.step.str()).second) {
      return Status::error(ErrorCode::duplicate_identity,
                           "duplicate observation for step '" + observation.step.str() + "'");
    }
    if (observation.attempt.value() == 0) {
      return Status::error(ErrorCode::invalid_argument,
                           "observation attempt numbers start at one");
    }
  }
  return Status::success();
}

Digest ObservedExecution::digest() const {
  CanonicalEncoder encoder;
  encoder.put_tag("cplan.observed-execution.v1");
  encode_value(encoder, plan_id);
  encode_value(encoder, plan_generation);
  encode_value(encoder, attempt);
  encode_value(encoder, authority_generation);
  encode_value(encoder, observed_generation);
  encode_value(encoder, boot_incarnation);
  encode_value(encoder, observed_epoch);
  encode_value(encoder, observed_at);
  encode_value(encoder, observed_state.digest());
  std::vector<StepObservation> sorted = observations;
  std::sort(sorted.begin(), sorted.end());
  encoder.put_u32(static_cast<std::uint32_t>(sorted.size()));
  for (const StepObservation& observation : sorted) {
    encode_value(encoder, observation.step);
    encode_value(encoder, observation.attempt);
    encoder.put_u8(static_cast<std::uint8_t>(observation.outcome));
    encode_value(encoder, observation.detail_code);
  }
  return encoder.digest();
}

namespace {

Refusal make_refusal(const PlanRequest& request, RefusalCode code, std::string summary,
                     std::string message) {
  Refusal refusal;
  refusal.code = code;
  refusal.summary_code = std::move(summary);
  refusal.request_digest = request.request_digest();
  refusal.binding = request.binding();
  refusal.justification.governing_policy = request.constraints.governing_policy;
  refusal.justification.governing_policy_digest = request.constraints.governing_policy_digest;
  refusal.justification.authority_generation = request.state.authority_generation;
  refusal.justification.rationale_code = refusal.summary_code;
  if (!message.empty()) {
    refusal.justification.notes.push_back(std::move(message));
  }
  return refusal;
}

// Fences an observed execution against the plan it claims to describe. Every
// identity, generation, attempt and incarnation must line up before the
// observation is allowed to influence a new plan.
Result<void> fence_observation(const Plan& prior, const ObservedExecution& observed) {
  if (!(observed.plan_id == prior.id)) {
    return Status::error(ErrorCode::not_found,
                         "observed execution refers to plan '" + observed.plan_id.str() +
                             "' but the supplied plan is '" + prior.id.str() + "'");
  }
  if (!(observed.plan_generation == prior.generation)) {
    return Status::error(ErrorCode::stale_generation,
                         "observed execution describes plan generation " +
                             std::to_string(observed.plan_generation.value()) +
                             " but the supplied plan is generation " +
                             std::to_string(prior.generation.value()));
  }
  if (observed.authority_generation < prior.binding.authority_generation) {
    return Status::error(ErrorCode::stale_authority,
                         "observed execution was taken under authority generation " +
                             std::to_string(observed.authority_generation.value()) +
                             " which predates the plan authority generation " +
                             std::to_string(prior.binding.authority_generation.value()));
  }
  if (observed.observed_generation < prior.binding.topology_generation) {
    return Status::error(ErrorCode::stale_generation,
                         "observed execution topology generation " +
                             std::to_string(observed.observed_generation.value()) +
                             " predates the plan topology generation " +
                             std::to_string(prior.binding.topology_generation.value()));
  }
  if (!(observed.boot_incarnation == prior.binding.boot_incarnation)) {
    return Status::error(ErrorCode::stale_incarnation,
                         "observed execution belongs to boot incarnation " +
                             std::to_string(observed.boot_incarnation.value()) +
                             " but the plan belongs to incarnation " +
                             std::to_string(prior.binding.boot_incarnation.value()));
  }
  CPLAN_TRY(observed.validate());
  return Status::success();
}

// Steps that were observed partially applied and whose compensation is known are
// compensated before the continuation is attempted.
std::vector<const Step*> steps_to_compensate(const Plan& prior,
                                             const ObservedExecution& observed,
                                             bool compensate_partial) {
  std::vector<const Step*> result;
  if (!compensate_partial) {
    return result;
  }
  for (const StepObservation& observation : observed.observations) {
    if (observation.outcome != StepOutcome::partially_applied) {
      continue;
    }
    const Step* step = prior.find_step(observation.step);
    if (step == nullptr) {
      continue;
    }
    if (!step->compensation.available ||
        step->compensation.kind != CompensationKind::inverse) {
      continue;
    }
    result.push_back(step);
  }
  std::sort(result.begin(), result.end(),
            [](const Step* lhs, const Step* rhs) { return lhs->id < rhs->id; });
  return result;
}

struct PrependOutcome {
  bool ok{false};
  Plan plan;
  std::string message;
};

// Inserts a verified compensation stage in front of a generated continuation
// plan, re-indexing stages and re-verifying the result.
PrependOutcome prepend_compensations(const Plan& continuation, const PlanRequest& request,
                                     const std::vector<const Step*>& compensate) {
  PrependOutcome outcome;
  Plan plan = continuation;
  std::vector<Step> compensation_steps;
  std::vector<StepId> compensation_ids;
  for (const Step* prior_step : compensate) {
    Step step;
    step.operation = prior_step->compensation.inverse;
    step.operation.reason_code = "compensate:" + prior_step->id.str();
    step.risk = prior_step->risk;
    step.estimated_duration = prior_step->estimated_duration;
    step.id = step_id_for(step.operation);
    step.stage = StageIndex(0);
    step.compensation = derive_compensation(request.state, step.operation,
                                            EvaluationContext{request.planning_instant,
                                                              &request.constraints,
                                                              &request.evidence,
                                                              &request.state});
    step.justification = continuation.justification;
    step.justification.rationale_code = "compensate-partial-step";
    step.justification.notes.clear();
    step.justification.notes.push_back("compensates partially applied step '" +
                                       prior_step->id.str() + "'");
    compensation_steps.push_back(std::move(step));
  }
  std::sort(compensation_steps.begin(), compensation_steps.end(),
            [](const Step& lhs, const Step& rhs) { return lhs.id < rhs.id; });

  // Preconditions and postconditions are derived from the observed state for the
  // compensation steps, applied in canonical order.
  CurrentStateSnapshot state = request.state;
  const EvaluationContext evaluation{request.planning_instant, &request.constraints,
                                     &request.evidence, &request.state};
  for (Step& step : compensation_steps) {
    step.preconditions = derive_preconditions(state, step.operation, evaluation);
    auto applied = apply_operation(state, step.operation, evaluation);
    if (!applied.ok()) {
      outcome.message = "compensation step '" + step.id.str() +
                        "' cannot be applied to the observed state: " +
                        applied.status().to_string();
      return outcome;
    }
    state = applied.value().state;
    step.postconditions = derive_postconditions(request.state, step.operation, evaluation);
    for (const Violation& violation : check_invariants(state, evaluation)) {
      if (violation.severity == Severity::violation) {
        outcome.message = "compensating step '" + step.id.str() +
                          "' would violate a declared obligation: " + violation.to_string();
        return outcome;
      }
    }
    compensation_ids.push_back(step.id);
  }

  const StageIndex shift(1);
  for (Stage& stage : plan.stages) {
    stage.index = StageIndex(stage.index.value() + shift.value());
  }
  for (Step& step : plan.steps) {
    step.stage = StageIndex(step.stage.value() + shift.value());
  }
  Stage compensation_stage;
  compensation_stage.index = StageIndex(0);
  compensation_stage.steps = compensation_ids;
  compensation_stage.rationale_code = "compensate-partially-applied-steps";
  compensation_stage.verified_permutations = 1;
  compensation_stage.verified_applications = compensation_ids.size();
  plan.stages.insert(plan.stages.begin(), compensation_stage);
  for (Step& step : compensation_steps) {
    plan.steps.insert(plan.steps.begin(), step);
  }
  for (const Step& compensation : compensation_steps) {
    for (const Step& step : plan.steps) {
      if (step.stage.value() == 0) {
        continue;
      }
      const std::vector<EntityRef>& left = compensation.operation.write_set();
      const std::vector<EntityRef>& right = step.operation.write_set();
      bool intersects = false;
      for (const EntityRef& reference : left) {
        if (std::find(right.begin(), right.end(), reference) != right.end()) {
          intersects = true;
          break;
        }
      }
      if (intersects) {
        DependencyEdge edge;
        edge.from = compensation.id;
        edge.to = step.id;
        edge.reason = DependencyReason::invariant_ordering;
        edge.detail_code = "compensation-before-retry";
        plan.edges.push_back(edge);
      }
    }
  }
  std::sort(plan.edges.begin(), plan.edges.end());
  plan.edges.erase(std::unique(plan.edges.begin(), plan.edges.end()), plan.edges.end());
  plan.objective = evaluate_objective_vector(plan.stages, plan.steps, plan.edges);

  const Status structure = plan.verify_structure();
  if (!structure.ok()) {
    outcome.message = "compensated plan is structurally invalid: " + structure.to_string();
    return outcome;
  }
  const VerificationReport report = verify_plan(plan, request, true);
  if (!report.ok) {
    outcome.message = "compensated plan failed verification: " + report.explain();
    return outcome;
  }
  plan.verification_digest = report.trace_digest;
  plan.verified_permutations = report.permutations_verified;
  outcome.plan = std::move(plan);
  outcome.ok = true;
  return outcome;
}

}  // namespace

PlanRequest replan_request_for(const Plan& prior, const ObservedExecution& observed,
                               const PlanRequest& base_request,
                               const ReplanningOptions& options) {
  PlanRequest request = base_request;
  request.state = observed.observed_state;
  request.limits = options.limits;
  // The observed state is the authoritative starting point: the target intent and
  // the capability evidence are rebased onto the observed generation and epoch.
  // The rebase is explicit and recorded, never silent.
  request.target.based_on_generation = observed.observed_state.topology_generation;
  request.target.based_on_epoch = observed.observed_state.epoch;
  request.target.authority = observed.observed_state.authority;
  request.target.authority_generation = observed.observed_state.authority_generation;
  request.evidence.topology_generation = observed.observed_state.topology_generation;
  request.evidence.boot_incarnation = observed.observed_state.boot_incarnation;
  request.lineage.kind = LineageKind::replan;
  request.lineage.parent_plan = prior.id;
  request.lineage.parent_generation = prior.generation;
  request.lineage.observed_attempt = observed.attempt;
  request.lineage.observed_execution_digest = observed.digest();
  request.label = options.label.empty() ? base_request.label : options.label;
  request.generation = prior.generation;
  return request;
}

PlanRequest rollback_request_for(const Plan& prior, const ObservedExecution& observed,
                                 const PlanRequest& base_request,
                                 const ReplanningOptions& options) {
  PlanRequest request = replan_request_for(prior, observed, base_request, options);
  request.target = target_from_state(base_request.state,
                                     TargetRevision(base_request.target.revision.value() + 1),
                                     "rollback-to-plan-origin");
  request.target.based_on_generation = observed.observed_state.topology_generation;
  request.target.based_on_epoch = observed.observed_state.epoch;
  request.target.authority = observed.observed_state.authority;
  request.target.authority_generation = observed.observed_state.authority_generation;
  request.lineage.kind = LineageKind::rollback;
  request.label = options.label.empty() ? "rollback" : options.label;
  return request;
}

PlanResult replan(const Plan& prior, const ObservedExecution& observed,
                  const PlanRequest& base_request, const ReplanningOptions& options) {
  PlanDiagnostics diagnostics;
  const Status fence = fence_observation(prior, observed);
  if (!fence.ok()) {
    return PlanResult::from_refusal(
        make_refusal(base_request, RefusalCode::stale_input, "observation-fenced",
                     fence.to_string()),
        diagnostics);
  }

  auto built = replan_request_for(prior, observed, base_request, options);
  PlanRequest request = std::move(built);
  const auto next_generation = checked_increment(prior.generation);
  if (!next_generation.ok()) {
    return PlanResult::from_refusal(
        make_refusal(request, RefusalCode::search_limit_exceeded, "generation-exhausted",
                     next_generation.status().to_string()),
        diagnostics);
  }
  request.generation = next_generation.value();

  // The observation is bound to the plan generation it describes: a caller
  // cannot claim to have executed a different attempt of the same plan.
  for (const StepObservation& observation : observed.observations) {
    if (prior.find_step(observation.step) == nullptr) {
      // Unknown steps are ignored rather than trusted; they are reported.
      continue;
    }
  }

  PlanResult result = generate_plan(request, StopToken{});
  diagnostics = result.diagnostics;

  if (result.ok()) {
    result.outcome = [&]() {
      Plan plan = result.plan();
      plan.lineage = request.lineage;
      plan.label = request.label;
      std::size_t succeeded = 0;
      std::size_t failed = 0;
      std::size_t unknown = 0;
      std::size_t partial = 0;
      for (const StepObservation& observation : observed.observations) {
        switch (observation.outcome) {
          case StepOutcome::succeeded:
            ++succeeded;
            break;
          case StepOutcome::failed:
            ++failed;
            break;
          case StepOutcome::unknown:
            ++unknown;
            break;
          case StepOutcome::partially_applied:
            ++partial;
            break;
          case StepOutcome::skipped:
            break;
        }
      }
      plan.justification.notes.push_back(
          "replanned from observed attempt " + std::to_string(observed.attempt.value()) +
          ": " + std::to_string(succeeded) + " step(s) reported succeeded, " +
          std::to_string(failed) + " failed, " + std::to_string(unknown) + " unknown, " +
          std::to_string(partial) + " partially applied");
      plan.justification.notes.push_back(
          "no prior step effect is assumed: the continuation was derived from the observed "
          "authoritative state");
      plan.justification.notes.push_back(
          "target intent and capability evidence rebased onto observed topology generation " +
          std::to_string(observed.observed_state.topology_generation.value()) +
          " and epoch " + std::to_string(observed.observed_state.epoch.value()) +
          " (revision " + std::to_string(request.target.revision.value()) + " unchanged)");
      return plan;
    }();
  }

  if (result.ok()) {
    const std::vector<const Step*> compensate =
        steps_to_compensate(prior, observed, options.compensate_partial_steps);
    if (!compensate.empty()) {
      PrependOutcome prepended = prepend_compensations(result.plan(), request, compensate);
      if (!prepended.ok) {
        return PlanResult::from_refusal(
            make_refusal(request, RefusalCode::no_safe_ordering, "compensation-unsafe",
                         prepended.message),
            diagnostics);
      }
      prepended.plan.lineage = request.lineage;
      prepended.plan.label = request.label;
      prepended.plan.justification.notes = result.plan().justification.notes;
      prepended.plan.justification.notes.push_back(
          "compensated " + std::to_string(compensate.size()) +
          " partially applied step(s) before continuing");
      PlanResult replanned;
      replanned.outcome = std::move(prepended.plan);
      replanned.diagnostics = diagnostics;
      return replanned;
    }
  }
  return result;
}

PlanResult build_rollback_plan(const Plan& prior, const ObservedExecution& observed,
                               const PlanRequest& base_request,
                               const ReplanningOptions& options) {
  PlanDiagnostics diagnostics;
  const Status fence = fence_observation(prior, observed);
  if (!fence.ok()) {
    return PlanResult::from_refusal(
        make_refusal(base_request, RefusalCode::stale_input, "observation-fenced",
                     fence.to_string()),
        diagnostics);
  }
  if (!(prior.binding == base_request.binding())) {
    return PlanResult::from_refusal(
        make_refusal(base_request, RefusalCode::stale_input, "rollback-origin-mismatch",
                     "the supplied request does not describe the state the prior plan started "
                     "from, so a rollback target cannot be derived"),
        diagnostics);
  }

  auto built = rollback_request_for(prior, observed, base_request, options);
  PlanRequest request = std::move(built);
  const auto next_generation = checked_increment(prior.generation);
  if (!next_generation.ok()) {
    return PlanResult::from_refusal(
        make_refusal(request, RefusalCode::search_limit_exceeded, "generation-exhausted",
                     next_generation.status().to_string()),
        diagnostics);
  }
  request.generation = next_generation.value();

  PlanResult result = generate_plan(request, StopToken{});
  if (result.ok()) {
    Plan plan = result.plan();
    plan.lineage = request.lineage;
    plan.label = request.label;
    plan.justification.notes.push_back(
        "rollback plan: returns the fabric to the state prior plan '" + prior.id.str() +
        "' started from, using the compensation metadata of its steps");
    result.outcome = std::move(plan);
  }
  return result;
}

}  // namespace cplan
