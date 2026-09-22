#include "change_planner/plan/compare.hpp"

#include <algorithm>
#include <map>

namespace cplan {

const char* to_string(StepChangeKind kind) noexcept {
  switch (kind) {
    case StepChangeKind::added:
      return "added";
    case StepChangeKind::removed:
      return "removed";
    case StepChangeKind::moved:
      return "moved";
    case StepChangeKind::redefined:
      return "redefined";
  }
  return "unknown";
}

std::string PlanComparison::explain() const {
  std::string out = identical ? "plans are identical"
                              : (semantically_identical ? "plans are semantically identical"
                                                         : "plans differ");
  out += " (left: " + std::to_string(left_steps) + " steps in " +
         std::to_string(left_stages) + " stages; right: " + std::to_string(right_steps) +
         " steps in " + std::to_string(right_stages) + " stages)";
  out += "\nleft content:  " + left_content.hex();
  out += "\nright content: " + right_content.hex();
  if (differences.empty()) {
    out += "\nno step-level differences";
    return out;
  }
  out += "\ndifferences:";
  for (const StepDifference& difference : differences) {
    out += "\n  - " + difference.id.str() + " [" +
           std::string(to_string(difference.kind)) + "]";
    if (difference.present_left) {
      out += " left stage " + std::to_string(difference.left_stage.value());
    }
    if (difference.present_right) {
      out += " right stage " + std::to_string(difference.right_stage.value());
    }
    if (!difference.detail.empty()) {
      out += ": " + difference.detail;
    }
  }
  return out;
}

PlanComparison compare_plans(const Plan& left, const Plan& right) {
  PlanComparison comparison;
  comparison.left_content = left.content_digest();
  comparison.right_content = right.content_digest();
  comparison.identical = left == right;
  comparison.semantically_identical = comparison.left_content == comparison.right_content;
  comparison.left_stages = left.stages.size();
  comparison.right_stages = right.stages.size();
  comparison.left_steps = left.steps.size();
  comparison.right_steps = right.steps.size();

  std::map<std::string, const Step*> left_steps;
  std::map<std::string, const Step*> right_steps;
  for (const Step& step : left.steps) {
    left_steps.emplace(step.id.str(), &step);
  }
  for (const Step& step : right.steps) {
    right_steps.emplace(step.id.str(), &step);
  }

  for (const auto& entry : left_steps) {
    const auto other = right_steps.find(entry.first);
    if (other == right_steps.end()) {
      StepDifference difference;
      difference.id = entry.second->id;
      difference.kind = StepChangeKind::removed;
      difference.present_left = true;
      difference.left_stage = entry.second->stage;
      difference.detail = "only present in the left plan";
      comparison.differences.push_back(std::move(difference));
      continue;
    }
    comparison.common_steps.push_back(entry.second->id);
    if (!(entry.second->operation == other->second->operation)) {
      StepDifference difference;
      difference.id = entry.second->id;
      difference.kind = StepChangeKind::redefined;
      difference.present_left = true;
      difference.present_right = true;
      difference.left_stage = entry.second->stage;
      difference.right_stage = other->second->stage;
      difference.detail = "the same step identity carries a different operation payload";
      comparison.differences.push_back(std::move(difference));
      continue;
    }
    if (!(entry.second->stage == other->second->stage)) {
      StepDifference difference;
      difference.id = entry.second->id;
      difference.kind = StepChangeKind::moved;
      difference.present_left = true;
      difference.present_right = true;
      difference.left_stage = entry.second->stage;
      difference.right_stage = other->second->stage;
      difference.detail = "scheduled in a different stage";
      comparison.differences.push_back(std::move(difference));
    }
  }
  for (const auto& entry : right_steps) {
    if (left_steps.find(entry.first) == left_steps.end()) {
      StepDifference difference;
      difference.id = entry.second->id;
      difference.kind = StepChangeKind::added;
      difference.present_right = true;
      difference.right_stage = entry.second->stage;
      difference.detail = "only present in the right plan";
      comparison.differences.push_back(std::move(difference));
    }
  }
  std::sort(comparison.differences.begin(), comparison.differences.end());
  std::sort(comparison.common_steps.begin(), comparison.common_steps.end());

  comparison.summary = comparison.explain();
  return comparison;
}

std::string StepExplanation::explain() const {
  std::string out = "step " + id.str() + " (stage " + std::to_string(stage.value()) + "): ";
  out += operation;
  out += "\n  class: " + std::string(to_string(operation_class));
  out += "\n  risk: " + std::to_string(risk.value()) + ", estimated duration " +
         std::to_string(estimated_duration.value()) + "ns";
  out += "\n  reason: " + justification.rationale_code;
  if (!justification.notes.empty()) {
    for (const std::string& note : justification.notes) {
      out += "\n    note: " + note;
    }
  }
  out += "\n  invariants checked:";
  for (const InvariantId invariant : justification.checked_invariants) {
    out += " " + std::string(cplan::to_string(invariant));
  }
  out += "\n  preconditions:";
  if (preconditions.empty()) {
    out += " none";
  }
  for (const Condition& condition : preconditions) {
    out += "\n    - " + condition.to_string();
  }
  out += "\n  postconditions:";
  if (postconditions.empty()) {
    out += " none";
  }
  for (const Condition& condition : postconditions) {
    out += "\n    - " + condition.to_string();
  }
  out += "\n  depends on:";
  if (incoming.empty()) {
    out += " nothing";
  }
  for (const DependencyEdge& edge : incoming) {
    out += "\n    - " + edge.from.str() + " (" + std::string(to_string(edge.reason)) + ", " +
           edge.detail_code + ")";
  }
  out += "\n  unlocks:";
  if (outgoing.empty()) {
    out += " nothing";
  }
  for (const DependencyEdge& edge : outgoing) {
    out += "\n    - " + edge.to.str() + " (" + std::string(to_string(edge.reason)) + ", " +
           edge.detail_code + ")";
  }
  out += "\n  concurrent with:";
  if (concurrent_steps.empty()) {
    out += " nothing";
  }
  for (const StepId& step_id : concurrent_steps) {
    out += " " + step_id.str();
  }
  out += "\n  compensation: " + std::string(to_string(compensation.kind));
  if (compensation.available) {
    out += " (" + compensation.inverse.to_string() + ")";
  } else {
    out += " unavailable: " + compensation.note_code;
  }
  if (!windows.empty()) {
    out += "\n  windows:";
    for (const WindowId& window : windows) {
      out += " " + window.str();
    }
  }
  return out;
}

Result<StepExplanation> explain_step(const Plan& plan, const StepId& id) {
  const Step* step = plan.find_step(id);
  if (step == nullptr) {
    return Status::error(ErrorCode::not_found, "step '" + id.str() + "' is not in the plan");
  }
  StepExplanation explanation;
  explanation.id = step->id;
  explanation.stage = step->stage;
  explanation.operation = step->operation.to_string();
  explanation.operation_kind = step->operation.kind;
  explanation.operation_class = step->operation.operation_class();
  explanation.preconditions = step->preconditions;
  explanation.postconditions = step->postconditions;
  explanation.incoming = plan.incoming_edges(step->id);
  explanation.outgoing = plan.outgoing_edges(step->id);
  explanation.compensation = step->compensation;
  explanation.risk = step->risk;
  explanation.estimated_duration = step->estimated_duration;
  explanation.windows = step->windows;
  explanation.justification = step->justification;
  const Stage* stage = plan.find_stage(step->stage);
  if (stage != nullptr) {
    for (const StepId& other : stage->steps) {
      if (other != step->id) {
        explanation.concurrent_steps.push_back(other);
      }
    }
  }
  return explanation;
}

std::vector<StepExplanation> explain_steps(const Plan& plan) {
  std::vector<StepExplanation> explanations;
  explanations.reserve(plan.steps.size());
  for (const Step& step : plan.steps) {
    auto explanation = explain_step(plan, step.id);
    if (explanation.ok()) {
      explanations.push_back(std::move(explanation.value()));
    }
  }
  return explanations;
}

}  // namespace cplan
