#include "change_planner/plan/verify.hpp"

#include <algorithm>
#include <set>

#include "change_planner/core/checked.hpp"
#include "change_planner/model/evaluate.hpp"

namespace cplan {

std::string VerificationReport::explain() const {
  if (ok) {
    return "verified " + std::to_string(steps_applied) + " step application(s) across " +
           std::to_string(permutations_verified) + " interleaving(s) and " +
           std::to_string(invariant_evaluations) + " invariant evaluation(s)";
  }
  std::string out = "verification failed";
  if (!failing_step.empty()) {
    out += " at step '" + failing_step.str() + "' (stage " +
           std::to_string(failing_stage.value()) + ")";
  }
  if (!message.empty()) {
    out += ": " + message;
  }
  for (const Condition& condition : unsatisfied_conditions) {
    out += "\n  unsatisfied condition: " + condition.to_string();
  }
  for (const Violation& violation : violations) {
    out += "\n  " + violation.to_string();
  }
  return out;
}

namespace {

std::vector<std::size_t> identity_permutation(std::size_t size) {
  std::vector<std::size_t> order(size);
  for (std::size_t i = 0; i < size; ++i) {
    order[i] = i;
  }
  return order;
}

}  // namespace

VerificationReport verify_plan(const Plan& plan, const PlanRequest& request,
                               bool exhaustive_interleavings) {
  VerificationReport report;

  const Status structure = plan.verify_structure();
  if (!structure.ok()) {
    report.message = "plan structure invalid: " + structure.to_string();
    return report;
  }
  if (!(plan.binding == request.binding())) {
    report.message =
        "plan validity binding does not describe the supplied request: the plan was computed "
        "against different inputs";
    return report;
  }

  // The plan is authoritative about when it is scheduled: a plan generated with
  // a shifted maintenance instant verifies against its own instant. Everything
  // else (state, target, constraints, evidence) must match the request exactly.
  const EvaluationContext evaluation{plan.planning_instant, &request.constraints,
                                     &request.evidence, &request.state};
  std::vector<Violation> initial = check_invariants(request.state, evaluation);
  ++report.invariant_evaluations;
  for (const Violation& violation : initial) {
    if (violation.severity == Severity::violation) {
      report.violations.push_back(violation);
    }
  }
  if (!report.violations.empty()) {
    report.message = "the plan start state already violates declared obligations";
    return report;
  }

  CanonicalEncoder trace;
  trace.put_tag("cplan.verification-trace.v1");
  encode_value(trace, plan.content_digest());

  CurrentStateSnapshot state = request.state;

  for (const Stage& stage : plan.stages) {
    std::vector<const Step*> steps;
    steps.reserve(stage.steps.size());
    for (const StepId& step_id : stage.steps) {
      const Step* step = plan.find_step(step_id);
      if (step == nullptr) {
        report.message = "stage references unknown step '" + step_id.str() + "'";
        report.failing_stage = stage.index;
        return report;
      }
      steps.push_back(step);
    }
    std::vector<std::size_t> order = identity_permutation(steps.size());
    std::sort(order.begin(), order.end(), [&steps](std::size_t lhs, std::size_t rhs) {
      return steps[lhs]->id < steps[rhs]->id;
    });

    auto limit = checked_factorial(static_cast<std::uint32_t>(steps.size()));
    if (!limit.ok()) {
      report.message = limit.status().to_string();
      report.failing_stage = stage.index;
      return report;
    }

    Digest expected;
    bool have_expected = false;
    std::vector<std::size_t> permutation = order;
    std::uint64_t permutation_index = 0;
    while (true) {
      ++permutation_index;
      CurrentStateSnapshot working = state;
      for (const std::size_t slot : permutation) {
        const Step& step = *steps[slot];
        for (const Condition& condition : step.preconditions) {
          auto satisfied = evaluate_condition(working, condition, evaluation);
          if (!satisfied.ok()) {
            report.message = "precondition could not be evaluated: " +
                             satisfied.status().to_string();
            report.failing_step = step.id;
            report.failing_stage = stage.index;
            return report;
          }
          if (!satisfied.value()) {
            report.unsatisfied_conditions.push_back(condition);
            report.failing_step = step.id;
            report.failing_stage = stage.index;
            report.message = "precondition does not hold before applying the step";
            return report;
          }
        }
        auto applied = apply_operation(working, step.operation, evaluation);
        if (!applied.ok()) {
          report.failing_step = step.id;
          report.failing_stage = stage.index;
          report.message = "operation rejected: " + applied.status().to_string();
          return report;
        }
        working = applied.value().state;
        ++report.steps_applied;
        for (const Condition& condition : step.postconditions) {
          auto satisfied = evaluate_condition(working, condition, evaluation);
          if (!satisfied.ok() || !satisfied.value()) {
            report.unsatisfied_conditions.push_back(condition);
            report.failing_step = step.id;
            report.failing_stage = stage.index;
            report.message = "postcondition does not hold after applying the step";
            return report;
          }
        }
        const std::vector<Violation> violations = check_invariants(working, evaluation);
        ++report.invariant_evaluations;
        for (const Violation& violation : violations) {
          if (violation.severity != Severity::violation) {
            continue;
          }
          report.violations.push_back(violation);
          report.failing_step = step.id;
          report.failing_stage = stage.index;
          report.message = "invariant violated after applying the step";
          return report;
        }
      }
      ++report.permutations_verified;
      trace.put_u32(stage.index.value());
      trace.put_u64(permutation_index);
      encode_value(trace, working.digest());
      const Digest digest = working.digest();
      if (!have_expected) {
        expected = digest;
        have_expected = true;
      } else if (!(digest == expected)) {
        report.failing_stage = stage.index;
        report.message =
            "steps in stage " + std::to_string(stage.index.value()) +
            " are not order independent: interleavings converge to different states";
        return report;
      }
      if (!exhaustive_interleavings) {
        break;
      }
      if (!std::next_permutation(permutation.begin(), permutation.end())) {
        break;
      }
    }

    std::vector<const Step*> canonical = steps;
    std::sort(canonical.begin(), canonical.end(),
              [](const Step* lhs, const Step* rhs) { return lhs->id < rhs->id; });
    for (const Step* step : canonical) {
      auto applied = apply_operation(state, step->operation, evaluation);
      if (!applied.ok()) {
        report.failing_step = step->id;
        report.failing_stage = stage.index;
        report.message = "canonical replay rejected the operation: " +
                         applied.status().to_string();
        return report;
      }
      state = applied.value().state;
    }
  }

  report.trace_digest = trace.digest();
  report.ok = true;
  return report;
}

Result<CurrentStateSnapshot> replay_prefix(const Plan& plan, const PlanRequest& request,
                                           const std::vector<StepId>& steps,
                                           std::vector<Violation>* violations) {
  if (!(plan.binding == request.binding())) {
    return Status::error(ErrorCode::stale_generation,
                         "plan validity binding does not match the supplied request");
  }
  const EvaluationContext evaluation{plan.planning_instant, &request.constraints,
                                     &request.evidence, &request.state};
  CurrentStateSnapshot state = request.state;
  for (const StepId& step_id : steps) {
    const Step* step = plan.find_step(step_id);
    if (step == nullptr) {
      return Status::error(ErrorCode::not_found, "step '" + step_id.str() + "' is not in the plan");
    }
    for (const Condition& condition : step->preconditions) {
      auto satisfied = evaluate_condition(state, condition, evaluation);
      if (!satisfied.ok()) {
        return satisfied.status();
      }
      if (!satisfied.value()) {
        return Status::error(ErrorCode::unsafe_transition,
                             "precondition does not hold for step '" + step_id.str() +
                                 "': " + condition.to_string());
      }
    }
    auto applied = apply_operation(state, step->operation, evaluation);
    if (!applied.ok()) {
      return applied.status();
    }
    state = applied.value().state;
    for (const Violation& violation : check_invariants(state, evaluation)) {
      if (violation.severity == Severity::violation) {
        if (violations != nullptr) {
          violations->push_back(violation);
        } else {
          return Status::error(ErrorCode::unsafe_transition,
                               "invariant violated after step '" + step_id.str() +
                                   "': " + violation.to_string());
        }
      }
    }
  }
  return state;
}

}  // namespace cplan
