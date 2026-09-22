#include "change_planner/plan/generate.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "change_planner/core/checked.hpp"
#include "change_planner/model/evaluate.hpp"
#include "change_planner/plan/graph.hpp"
#include "change_planner/plan/verify.hpp"

namespace cplan {

// ---------------------------------------------------------------------------
// Request and limits
// ---------------------------------------------------------------------------

Result<void> PlanningLimits::validate() const {
  if (max_steps == 0) {
    return Status::error(ErrorCode::invalid_argument, "max_steps must be positive");
  }
  if (max_candidates == 0 || max_candidates > 16) {
    return Status::error(ErrorCode::invalid_argument, "max_candidates must be within 1..16");
  }
  if (max_stage_width == 0 || max_stage_width > 5) {
    return Status::error(ErrorCode::invalid_argument,
                         "max_stage_width must be within 1..5 so that exhaustively verified "
                         "interleavings stay bounded");
  }
  if (max_scheduling_attempts == 0) {
    return Status::error(ErrorCode::invalid_argument, "max_scheduling_attempts must be positive");
  }
  if (max_permutation_checks == 0 || max_states_visited == 0) {
    return Status::error(ErrorCode::invalid_argument,
                         "permutation and state budgets must be positive");
  }
  CPLAN_TRY(checked_factorial(max_stage_width));
  return Status::success();
}

Digest PlanningLimits::digest() const {
  CanonicalEncoder encoder;
  encoder.put_tag("planning-limits");
  encoder.put_u32(max_steps);
  encoder.put_u32(max_candidates);
  encoder.put_u32(max_stage_width);
  encoder.put_u32(max_scheduling_attempts);
  encoder.put_u32(max_core_minimisation_attempts);
  encoder.put_u64(max_permutation_checks);
  encoder.put_u64(max_states_visited);
  encoder.put_bool(allow_window_shift);
  return encoder.digest();
}

Result<void> PlanRequest::validate() const {
  if (plan_id.empty()) {
    return Status::error(ErrorCode::invalid_argument, "plan request has an empty plan identity");
  }
  CPLAN_TRY(state.validate());
  CPLAN_TRY(target.validate());
  CPLAN_TRY(constraints.validate());
  CPLAN_TRY(evidence.validate());
  CPLAN_TRY(limits.validate());
  if (!objective.valid()) {
    return Status::error(ErrorCode::invalid_argument, "plan request objective is invalid");
  }
  return Status::success();
}

ValidityBinding PlanRequest::binding() const {
  ValidityBinding result;
  result.topology_generation = state.topology_generation;
  result.authority = state.authority;
  result.authority_generation = state.authority_generation;
  result.boot_incarnation = state.boot_incarnation;
  result.epoch = state.epoch;
  result.capability_version = evidence.capability_version;
  result.target_revision = target.revision;
  result.topology_digest = state.digest();
  result.target_digest = target.digest();
  result.constraint_digest = constraints.digest();
  result.evidence_digest = evidence.digest();
  result.topology = summarize(state);
  return result;
}

Digest PlanRequest::request_digest() const {
  CanonicalEncoder encoder;
  encoder.put_tag("cplan.request.v1");
  encode_value(encoder, plan_id);
  encode_value(encoder, generation);
  encode_value(encoder, planning_instant);
  encode_value(encoder, state.digest());
  encode_value(encoder, target.digest());
  encode_value(encoder, constraints.digest());
  encode_value(encoder, evidence.digest());
  encode_value(encoder, objective.digest());
  encode_value(encoder, limits.digest());
  lineage.encode(encoder);
  encode_value(encoder, label);
  return encoder.digest();
}

namespace {

// ---------------------------------------------------------------------------
// Scheduling internals
// ---------------------------------------------------------------------------

enum class Strategy : std::uint8_t {
  safety_first = 0,
  unlock_first,
  maintenance_first,
  reverse_canonical,
};

const char* strategy_code(Strategy strategy) {
  switch (strategy) {
    case Strategy::safety_first:
      return "safety-first";
    case Strategy::unlock_first:
      return "unlock-first";
    case Strategy::maintenance_first:
      return "maintenance-first";
    case Strategy::reverse_canonical:
      return "reverse-canonical";
  }
  return "unknown";
}

struct Node {
  Operation operation;
  StepId id;
  std::vector<std::size_t> predecessors;
  std::size_t successors{0};
};

struct FailureInfo {
  bool has_failure{false};
  std::size_t depth{0};
  StepId step;
  Operation operation;
  std::string code;
  std::vector<Violation> violations;
  std::vector<Condition> unsatisfied;

  void record(std::size_t new_depth, const StepId& new_step, const Operation& new_operation,
              std::string new_code, std::vector<Violation> new_violations,
              std::vector<Condition> new_unsatisfied) {
    if (has_failure && depth > new_depth) {
      return;
    }
    if (has_failure && depth == new_depth && !(new_step < step)) {
      return;
    }
    has_failure = true;
    depth = new_depth;
    step = new_step;
    operation = new_operation;
    code = std::move(new_code);
    violations = std::move(new_violations);
    unsatisfied = std::move(new_unsatisfied);
  }
};

struct SearchContext {
  const PlanRequest* request{nullptr};
  EvaluationContext evaluation;
  PlanDiagnostics* diagnostics{nullptr};
  StopToken stop;
  std::vector<Node> nodes;
  Strategy strategy{Strategy::safety_first};
  std::vector<char> applied;
  std::size_t applied_count{0};
  CurrentStateSnapshot state;
  std::vector<std::size_t> order;
  FailureInfo failure;
  bool cancelled{false};
  bool budget_exhausted{false};
  bool limit_exceeded{false};
};

bool stop_requested(const SearchContext& context) { return context.stop.stop_requested(); }

std::vector<std::size_t> ready_candidates(const SearchContext& context) {
  std::vector<std::size_t> ready;
  for (std::size_t index = 0; index < context.nodes.size(); ++index) {
    if (context.applied[index] != 0) {
      continue;
    }
    bool satisfied = true;
    for (const std::size_t predecessor : context.nodes[index].predecessors) {
      if (context.applied[predecessor] == 0) {
        satisfied = false;
        break;
      }
    }
    if (satisfied) {
      ready.push_back(index);
    }
  }
  return ready;
}

void sort_ready(const SearchContext& context, std::vector<std::size_t>& ready) {
  const Strategy strategy = context.strategy;
  std::stable_sort(ready.begin(), ready.end(),
                   [&context, strategy](std::size_t lhs, std::size_t rhs) {
                     const Operation& left = context.nodes[lhs].operation;
                     const Operation& right = context.nodes[rhs].operation;
                     const RiskScore left_risk = derive_risk(left);
                     const RiskScore right_risk = derive_risk(right);
                     switch (strategy) {
                       case Strategy::safety_first: {
                         const int left_class =
                             left.requires_maintenance_window() ? 1 : 0;
                         const int right_class =
                             right.requires_maintenance_window() ? 1 : 0;
                         if (left_class != right_class) return left_class < right_class;
                         if (!(left_risk == right_risk)) return left_risk < right_risk;
                         break;
                       }
                       case Strategy::unlock_first: {
                         const std::size_t left_unlock = context.nodes[lhs].successors;
                         const std::size_t right_unlock = context.nodes[rhs].successors;
                         if (left_unlock != right_unlock) return left_unlock > right_unlock;
                         if (!(left_risk == right_risk)) return left_risk < right_risk;
                         break;
                       }
                       case Strategy::maintenance_first: {
                         const int left_class =
                             left.requires_maintenance_window() ? 0 : 1;
                         const int right_class =
                             right.requires_maintenance_window() ? 0 : 1;
                         if (left_class != right_class) return left_class < right_class;
                         if (!(left_risk == right_risk)) return right_risk < left_risk;
                         break;
                       }
                       case Strategy::reverse_canonical:
                         break;
                     }
                     return left < right;
                   });
  if (strategy == Strategy::reverse_canonical) {
    std::reverse(ready.begin(), ready.end());
  }
}

bool independent(const SearchContext& context, std::size_t lhs, std::size_t rhs) {
  for (const std::size_t predecessor : context.nodes[rhs].predecessors) {
    if (predecessor == lhs) {
      return false;
    }
  }
  for (const std::size_t predecessor : context.nodes[lhs].predecessors) {
    if (predecessor == rhs) {
      return false;
    }
  }
  return true;
}

// Bounded, deterministic depth-first search over step orderings. Symmetry
// reduction skips ready steps that are provably independent of an already-tried
// candidate: if a solution starts with one of them, swapping the two yields
// another solution because they commute and neither reads what the other writes.
bool search_order(SearchContext& context) {
  struct Frame {
    std::vector<std::size_t> ready;
    std::vector<char> skipped;
    std::size_t next{0};
    bool committed{false};
    std::size_t chosen{std::numeric_limits<std::size_t>::max()};
  };

  std::vector<Frame> stack;
  std::vector<CurrentStateSnapshot> state_stack;

  auto push_frame = [&context, &stack, &state_stack]() {
    Frame frame;
    frame.ready = ready_candidates(context);
    sort_ready(context, frame.ready);
    frame.skipped.assign(frame.ready.size(), 0);
    stack.push_back(std::move(frame));
    state_stack.push_back(context.state);
  };

  push_frame();

  while (!stack.empty()) {
    if (stop_requested(context)) {
      context.cancelled = true;
      return false;
    }
    if (context.applied_count == context.nodes.size()) {
      return true;
    }
    Frame& frame = stack.back();
    bool advanced = false;
    while (frame.next < frame.ready.size()) {
      const std::size_t candidate = frame.ready[frame.next];
      const std::size_t slot = frame.next;
      ++frame.next;
      if (frame.skipped[slot] != 0 || context.applied[candidate] != 0) {
        continue;
      }
      ++context.diagnostics->states_visited;
      if (context.diagnostics->states_visited > context.request->limits.max_states_visited) {
        context.limit_exceeded = true;
        return false;
      }
      const Operation& operation = context.nodes[candidate].operation;
      const std::vector<Condition> preconditions =
          derive_preconditions(context.state, operation, context.evaluation);
      std::vector<Condition> unsatisfied;
      for (const Condition& condition : preconditions) {
        auto satisfied = evaluate_condition(context.state, condition, context.evaluation);
        if (!satisfied.ok()) {
          context.failure.record(context.applied_count, context.nodes[candidate].id, operation,
                                 satisfied.status().message(), {}, {condition});
          unsatisfied.push_back(condition);
          break;
        }
        if (!satisfied.value()) {
          unsatisfied.push_back(condition);
        }
      }
      if (!unsatisfied.empty()) {
        context.failure.record(context.applied_count, context.nodes[candidate].id, operation,
                               "unsatisfied-precondition", {}, unsatisfied);
        continue;
      }
      auto applied = apply_operation(context.state, operation, context.evaluation);
      if (!applied.ok()) {
        context.failure.record(context.applied_count, context.nodes[candidate].id, operation,
                               applied.status().message(), {}, {});
        continue;
      }
      const std::vector<Violation> violations =
          check_invariants(applied.value().state, context.evaluation);
      ++context.diagnostics->invariant_evaluations;
      std::vector<Violation> hard_violations;
      for (const Violation& violation : violations) {
        if (violation.severity == Severity::violation) {
          hard_violations.push_back(violation);
        }
      }
      if (!hard_violations.empty()) {
        context.failure.record(context.applied_count, context.nodes[candidate].id, operation,
                               "invariant-violation", hard_violations, {});
        continue;
      }

      context.state = applied.value().state;
      context.applied[candidate] = 1;
      ++context.applied_count;
      context.order.push_back(candidate);
      frame.committed = true;
      frame.chosen = candidate;

      for (std::size_t j = frame.next; j < frame.ready.size(); ++j) {
        if (independent(context, candidate, frame.ready[j])) {
          frame.skipped[j] = 1;
        }
      }
      push_frame();
      advanced = true;
      break;
    }
    if (advanced) {
      continue;
    }
    // Backtrack.
    context.state = state_stack.back();
    state_stack.pop_back();
    Frame& closing = stack.back();
    if (closing.committed) {
      context.applied[closing.chosen] = 0;
      --context.applied_count;
      context.order.pop_back();
      closing.committed = false;
      closing.chosen = std::numeric_limits<std::size_t>::max();
    }
    stack.pop_back();
    if (context.limit_exceeded) {
      return false;
    }
  }
  return false;
}

struct StageCheck {
  bool ok{false};
  std::string message;
  std::vector<Violation> violations;
  std::vector<Condition> unsatisfied;
  StepId failing_step;
  std::uint64_t permutations{0};
  std::uint64_t applications{0};
  bool limit_exceeded{false};
};

// Exhaustively verifies that a set of steps, applied in any interleaving from
// the given entry state, keeps every invariant, satisfies every precondition and
// postcondition, and always converges to the same state.
StageCheck verify_stage(const PlanRequest& request, const EvaluationContext& evaluation,
                        const CurrentStateSnapshot& entry_state,
                        const std::vector<const Node*>& steps, PlanDiagnostics& diagnostics) {
  StageCheck check;
  std::vector<std::size_t> order(steps.size());
  for (std::size_t i = 0; i < steps.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&steps](std::size_t lhs, std::size_t rhs) {
    return steps[lhs]->id < steps[rhs]->id;
  });

  const auto limit_status = checked_factorial(static_cast<std::uint32_t>(steps.size()));
  if (!limit_status.ok()) {
    check.limit_exceeded = true;
    check.message = limit_status.status().message();
    return check;
  }

  Digest expected;
  bool have_expected = false;
  auto permutation = order;
  std::size_t permutation_index = 0;
  while (true) {
    ++permutation_index;
    ++check.permutations;
    ++diagnostics.permutation_checks;
    if (diagnostics.permutation_checks > request.limits.max_permutation_checks) {
      check.limit_exceeded = true;
      check.message = "permutation verification budget exhausted";
      return check;
    }
    CurrentStateSnapshot state = entry_state;
    for (const std::size_t slot : permutation) {
      const Node& node = *steps[slot];
      const std::vector<Condition> preconditions =
          derive_preconditions(entry_state, node.operation, evaluation);
      for (const Condition& condition : preconditions) {
        auto satisfied = evaluate_condition(state, condition, evaluation);
        if (!satisfied.ok() || !satisfied.value()) {
          check.ok = false;
          check.failing_step = node.id;
          check.unsatisfied.push_back(condition);
          check.message = "precondition does not hold in interleaving " +
                          std::to_string(permutation_index) + ": " + condition.to_string();
          return check;
        }
      }
      auto applied = apply_operation(state, node.operation, evaluation);
      if (!applied.ok()) {
        check.ok = false;
        check.failing_step = node.id;
        check.message = "operation rejected during interleaving " +
                        std::to_string(permutation_index) + ": " + applied.status().to_string();
        return check;
      }
      state = applied.value().state;
      ++check.applications;
      ++diagnostics.states_visited;
      if (diagnostics.states_visited > request.limits.max_states_visited) {
        check.limit_exceeded = true;
        check.message = "state budget exhausted during interleaving verification";
        return check;
      }
      const std::vector<Condition> postconditions =
          derive_postconditions(entry_state, node.operation, evaluation);
      for (const Condition& condition : postconditions) {
        auto satisfied = evaluate_condition(state, condition, evaluation);
        if (!satisfied.ok() || !satisfied.value()) {
          check.ok = false;
          check.failing_step = node.id;
          check.unsatisfied.push_back(condition);
          check.message = "postcondition does not hold in interleaving " +
                          std::to_string(permutation_index) + ": " + condition.to_string();
          return check;
        }
      }
      const std::vector<Violation> violations = check_invariants(state, evaluation);
      ++diagnostics.invariant_evaluations;
      for (const Violation& violation : violations) {
        if (violation.severity == Severity::violation) {
          check.ok = false;
          check.failing_step = node.id;
          check.violations.push_back(violation);
          check.message = "invariant violated in interleaving " +
                          std::to_string(permutation_index) + ": " + violation.to_string();
          return check;
        }
      }
    }
    const Digest digest = state.digest();
    if (!have_expected) {
      expected = digest;
      have_expected = true;
    } else if (!(digest == expected)) {
      check.ok = false;
      check.message =
          "steps are not order independent: interleaving " + std::to_string(permutation_index) +
          " converges to a different state than the canonical order";
      return check;
    }
    if (!std::next_permutation(permutation.begin(), permutation.end())) {
      break;
    }
  }
  check.ok = true;
  return check;
}

struct SchedulerResult {
  bool ok{false};
  bool cancelled{false};
  bool limit_exceeded{false};
  std::vector<Stage> stages;
  std::vector<Step> steps;
  std::vector<std::size_t> linear_order;
  CurrentStateSnapshot final_state;
  FailureInfo failure;
  std::string message;
};

SchedulerResult schedule(const PlanRequest& request, const EvaluationContext& evaluation,
                         const std::vector<Node>& nodes, const std::vector<DependencyEdge>& edges,
                         Strategy strategy, PlanDiagnostics& diagnostics, const StopToken& stop) {
  SchedulerResult result;

  // Steps joined by a dependency edge must never share a stage: the edge is the
  // recorded reason why they cannot run in parallel.
  std::set<std::pair<std::string, std::string>> linked;
  for (const DependencyEdge& edge : edges) {
    linked.insert({edge.from.str(), edge.to.str()});
    linked.insert({edge.to.str(), edge.from.str()});
  }
  auto batch_conflicts = [&linked](const std::vector<const Node*>& batch, const StepId& candidate) {
    for (const Node* node : batch) {
      if (linked.count({node->id.str(), candidate.str()}) != 0) {
        return true;
      }
    }
    return false;
  };

  SearchContext context;
  context.request = &request;
  context.evaluation = evaluation;
  context.diagnostics = &diagnostics;
  context.stop = stop;
  context.nodes = nodes;
  context.strategy = strategy;
  context.applied.assign(nodes.size(), 0);
  context.state = request.state;

  ++diagnostics.scheduling_attempts;
  if (!search_order(context)) {
    result.cancelled = context.cancelled;
    result.limit_exceeded = context.limit_exceeded;
    result.failure = context.failure;
    result.message = context.cancelled ? "planning cancelled"
                                       : (context.limit_exceeded ? "scheduling budget exhausted"
                                                                 : "no safe ordering exists");
    return result;
  }

  // Compress the verified linear order into maximal verified stages.
  CurrentStateSnapshot state = request.state;
  std::vector<const Node*> batch;
  StageIndex stage_index(0);
  std::vector<Stage> stages;
  std::vector<Step> steps;
  std::vector<std::size_t> linear_order = context.order;

  auto flush = [&](std::vector<const Node*>& current, const CurrentStateSnapshot& entry,
                   StageCheck check, const char* rationale) {
    std::vector<StepId> ids;
    ids.reserve(current.size());
    for (const Node* node : current) {
      ids.push_back(node->id);
    }
    std::sort(ids.begin(), ids.end());
    Stage stage;
    stage.index = stage_index;
    stage.steps = ids;
    stage.rationale_code = rationale;
    stage.verified_permutations = check.permutations;
    stage.verified_applications = check.applications;
    for (const Node* node : current) {
      Step step;
      step.id = node->id;
      step.stage = stage_index;
      step.operation = node->operation;
      step.preconditions = derive_preconditions(entry, node->operation, evaluation);
      step.postconditions = derive_postconditions(entry, node->operation, evaluation);
      step.compensation = derive_compensation(entry, node->operation, evaluation);
      step.risk = derive_risk(node->operation);
      step.estimated_duration = derive_duration(node->operation);
      const Device* device = node->operation.device.empty()
                                 ? nullptr
                                 : entry.find_device(node->operation.device);
      if (device != nullptr && node->operation.requires_maintenance_window()) {
        for (const MaintenanceWindow& window : device->maintenance_windows) {
          if (window.contains(request.planning_instant)) {
            step.windows.push_back(window.id);
          }
        }
      }
      step.justification.governing_policy = request.constraints.governing_policy;
      step.justification.governing_policy_digest = request.constraints.governing_policy_digest;
      step.justification.authority_generation = request.state.authority_generation;
      step.justification.rationale_code = step.preconditions.empty() ? "no-precondition"
                                                                    : "statically-verified";
      step.justification.notes.push_back("operation class " +
                                         std::string(to_string(node->operation.operation_class())));
      steps.push_back(std::move(step));
    }
    stages.push_back(std::move(stage));
  };

  // Build the ordered application for the current batch in canonical step order.
  auto apply_batch = [&](std::vector<const Node*>& current, CurrentStateSnapshot& target) {
    std::vector<const Node*> ordered = current;
    std::sort(ordered.begin(), ordered.end(),
              [](const Node* lhs, const Node* rhs) { return lhs->id < rhs->id; });
    for (const Node* node : ordered) {
      auto applied = apply_operation(target, node->operation, evaluation);
      if (!applied.ok()) {
        return applied.status();
      }
      target = applied.value().state;
    }
    return Status::success();
  };

  for (const std::size_t index : linear_order) {
    if (batch.size() >= request.limits.max_stage_width) {
      const StageCheck batch_check = verify_stage(request, evaluation, state, batch, diagnostics);
      if (!batch_check.ok) {
        result.message = "batch verification failed: " + batch_check.message;
        return result;
      }
      const CurrentStateSnapshot entry = state;
      const Status apply_status = apply_batch(batch, state);
      if (!apply_status.ok()) {
        result.message = "batch application failed: " + apply_status.to_string();
        return result;
      }
      flush(batch, entry, batch_check,
            batch.size() > 1 ? "stage-width-bound" : "single-step-change");
      stage_index = StageIndex(stage_index.value() + 1);
      batch.clear();
    }
    if (!batch.empty() && batch_conflicts(batch, nodes[index].id)) {
      const StageCheck batch_check = verify_stage(request, evaluation, state, batch, diagnostics);
      if (!batch_check.ok) {
        result.message = "batch verification failed: " + batch_check.message;
        return result;
      }
      const CurrentStateSnapshot entry = state;
      const Status apply_status = apply_batch(batch, state);
      if (!apply_status.ok()) {
        result.message = "batch application failed: " + apply_status.to_string();
        return result;
      }
      flush(batch, entry, batch_check,
            batch.size() > 1 ? "dependency-ordered" : "single-step-change");
      stage_index = StageIndex(stage_index.value() + 1);
      batch.clear();
    }
    std::vector<const Node*> trial = batch;
    trial.push_back(&nodes[index]);
    const StageCheck check = verify_stage(request, evaluation, state, trial, diagnostics);
    if (check.limit_exceeded) {
      result.limit_exceeded = true;
      result.message = check.message;
      return result;
    }
    if (check.ok) {
      batch = std::move(trial);
      continue;
    }
    if (batch.empty()) {
      result.message = "a step that was safe alone failed stage verification: " + check.message;
      return result;
    }
    const StageCheck batch_check = verify_stage(request, evaluation, state, batch, diagnostics);
    if (!batch_check.ok) {
      result.message = "batch verification failed: " + batch_check.message;
      return result;
    }
    {
      const CurrentStateSnapshot entry = state;
      const Status apply_status = apply_batch(batch, state);
      if (!apply_status.ok()) {
        result.message = "batch application failed: " + apply_status.to_string();
        return result;
      }
      flush(batch, entry, batch_check,
            batch.size() > 1 ? "safety-ordered" : "single-step-change");
    }
    stage_index = StageIndex(stage_index.value() + 1);
    batch.clear();
    batch.push_back(&nodes[index]);
    const StageCheck single = verify_stage(request, evaluation, state, batch, diagnostics);
    if (!single.ok) {
      result.message = "single-step stage failed verification: " + single.message;
      return result;
    }
  }
  if (!batch.empty()) {
    const CurrentStateSnapshot entry = state;
    const StageCheck batch_check = verify_stage(request, evaluation, state, batch, diagnostics);
    if (!batch_check.ok) {
      result.message = "final batch verification failed: " + batch_check.message;
      return result;
    }
    {
      const Status apply_status = apply_batch(batch, state);
      if (!apply_status.ok()) {
        result.message = "final batch application failed: " + apply_status.to_string();
        return result;
      }
    }
    flush(batch, entry, batch_check,
          batch.size() > 1 ? "independent-parallel-group" : "single-step-change");
    stage_index = StageIndex(stage_index.value() + 1);
    batch.clear();
  }

  result.stages = std::move(stages);
  result.steps = std::move(steps);
  result.linear_order = std::move(linear_order);
  result.final_state = std::move(state);
  result.ok = true;
  return result;
}

std::vector<InvariantId> verified_invariants_for(const SafetyConstraints& constraints) {
  std::set<InvariantId> invariants;
  for (const ConstraintItem& item : constraints.items) {
    if (!item.enabled) {
      continue;
    }
    switch (item.kind) {
      case ConstraintKind::connectivity:
        invariants.insert(InvariantId::connectivity);
        break;
      case ConstraintKind::failure_domain_redundancy:
        invariants.insert(InvariantId::failure_domain_redundancy);
        break;
      case ConstraintKind::path_diversity:
        invariants.insert(InvariantId::path_diversity);
        break;
      case ConstraintKind::capacity_headroom:
        invariants.insert(InvariantId::capacity_headroom);
        break;
      case ConstraintKind::maintenance_exclusion:
        invariants.insert(InvariantId::maintenance_exclusion);
        break;
      case ConstraintKind::maintenance_window:
        invariants.insert(InvariantId::maintenance_window);
        break;
      case ConstraintKind::drain_before_maintenance:
        invariants.insert(InvariantId::maintenance_window);
        break;
      case ConstraintKind::change_serialization:
        invariants.insert(InvariantId::change_serialization);
        invariants.insert(InvariantId::service_continuity);
        break;
      case ConstraintKind::workload_contract:
        invariants.insert(InvariantId::workload_contract);
        break;
      case ConstraintKind::capability_evidence:
        invariants.insert(InvariantId::capability_evidence);
        break;
      case ConstraintKind::generation_binding:
        invariants.insert(InvariantId::generation_binding);
        break;
      case ConstraintKind::rollback_metadata:
        break;
    }
  }
  invariants.insert(InvariantId::connectivity);
  return std::vector<InvariantId>(invariants.begin(), invariants.end());
}

CapabilitySet effective_capabilities(const PlanRequest& request, const Device& device) {
  CapabilitySet supported = device.spec.capabilities;
  if (request.constraints.enforced(ConstraintKind::capability_evidence)) {
    const CapabilityEvidence* evidence = request.evidence.find(device.id);
    if (evidence != nullptr) {
      supported = supported.intersect(evidence->capabilities);
    } else {
      supported = CapabilitySet();
    }
  }
  return supported;
}

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
  refusal.justification.checked_invariants = verified_invariants_for(request.constraints);
  if (!message.empty()) {
    refusal.justification.notes.push_back(std::move(message));
  }
  return refusal;
}

RefusalCode map_status(const Status& status) {
  switch (status.code()) {
    case ErrorCode::invalid_argument:
    case ErrorCode::malformed_input:
      return RefusalCode::unsatisfiable_target;
    case ErrorCode::unsupported_capability:
      return RefusalCode::impossible_transition;
    case ErrorCode::unsafe_transition:
    case ErrorCode::conflict:
    case ErrorCode::not_found:
    case ErrorCode::duplicate_identity:
      return RefusalCode::impossible_transition;
    case ErrorCode::capacity_exceeded:
    case ErrorCode::contradictory_constraints:
      return RefusalCode::contradictory_constraints;
    case ErrorCode::cancelled:
      return RefusalCode::cancelled;
    case ErrorCode::limit_exceeded:
    case ErrorCode::size_limit:
      return RefusalCode::search_limit_exceeded;
    case ErrorCode::stale_generation:
    case ErrorCode::stale_authority:
    case ErrorCode::stale_incarnation:
    case ErrorCode::stale_revision:
      return RefusalCode::stale_input;
    default:
      return RefusalCode::internal_verification_failed;
  }
}

struct BindingCheck {
  bool ok{true};
  std::string dimension;
  std::string message;
};

BindingCheck check_generation_binding(const PlanRequest& request) {
  BindingCheck check;
  if (request.target.authority != request.state.authority) {
    check.ok = false;
    check.dimension = "authority";
    check.message = "target intent authority '" + request.target.authority.str() +
                    "' does not match snapshot authority '" + request.state.authority.str() + "'";
    return check;
  }
  if (!(request.target.authority_generation == request.state.authority_generation)) {
    check.ok = false;
    check.dimension = "authority-generation";
    check.message = "target intent authority generation " +
                    std::to_string(request.target.authority_generation.value()) +
                    " does not match snapshot authority generation " +
                    std::to_string(request.state.authority_generation.value());
    return check;
  }
  if (!(request.target.based_on_generation == request.state.topology_generation)) {
    check.ok = false;
    check.dimension = "topology-generation";
    check.message = "target intent is based on topology generation " +
                    std::to_string(request.target.based_on_generation.value()) +
                    " but the snapshot generation is " +
                    std::to_string(request.state.topology_generation.value());
    return check;
  }
  if (!(request.target.based_on_epoch == request.state.epoch)) {
    check.ok = false;
    check.dimension = "epoch";
    check.message = "target intent is based on epoch " +
                    std::to_string(request.target.based_on_epoch.value()) +
                    " but the snapshot epoch is " + std::to_string(request.state.epoch.value());
    return check;
  }
  if (!(request.evidence.topology_generation == request.state.topology_generation)) {
    check.ok = false;
    check.dimension = "capability-evidence-generation";
    check.message = "capability evidence was observed at topology generation " +
                    std::to_string(request.evidence.topology_generation.value()) +
                    " but the snapshot generation is " +
                    std::to_string(request.state.topology_generation.value());
    return check;
  }
  if (!(request.evidence.boot_incarnation == request.state.boot_incarnation)) {
    check.ok = false;
    check.dimension = "boot-incarnation";
    check.message = "capability evidence belongs to boot incarnation " +
                    std::to_string(request.evidence.boot_incarnation.value()) +
                    " but the snapshot belongs to incarnation " +
                    std::to_string(request.state.boot_incarnation.value());
    return check;
  }
  if (request.constraints.enforced(ConstraintKind::generation_binding) &&
      !(request.constraints.authority_generation == request.state.authority_generation)) {
    check.ok = false;
    check.dimension = "constraint-authority-generation";
    check.message = "governing constraints were issued under authority generation " +
                    std::to_string(request.constraints.authority_generation.value()) +
                    " but the snapshot authority generation is " +
                    std::to_string(request.state.authority_generation.value());
    return check;
  }
  return check;
}

// ---------------------------------------------------------------------------
// Attempt: one complete planning pass under the current enforcement settings
// ---------------------------------------------------------------------------

struct Attempt {
  bool success{false};
  Plan plan;
  RefusalCode code{RefusalCode::internal_verification_failed};
  std::string summary;
  std::string message;
  std::vector<Violation> residual;
  std::vector<BlockedStep> blocked;
  std::vector<RejectedAlternative> rejected;
  bool cancelled{false};
  bool limit_exceeded{false};
};

Attempt run_attempt(const PlanRequest& request, PlanDiagnostics& diagnostics, const StopToken& stop,
                    std::uint32_t candidate_count) {
  Attempt attempt;
  const EvaluationContext evaluation{request.planning_instant, &request.constraints,
                                     &request.evidence, &request.state};

  auto derived = derive_operations(request.state, request.target, evaluation);
  if (!derived.ok()) {
    attempt.code = map_status(derived.status());
    attempt.summary = "target-derivation-failed";
    attempt.message = derived.status().to_string();
    return attempt;
  }
  const std::vector<Operation>& operations = derived.value().operations;
  if (operations.size() > request.limits.max_steps) {
    attempt.code = RefusalCode::search_limit_exceeded;
    attempt.summary = "step-budget-exceeded";
    attempt.message = "target requires " + std::to_string(operations.size()) +
                      " operations which exceeds the configured step budget of " +
                      std::to_string(request.limits.max_steps);
    return attempt;
  }
  for (const std::string& note : derived.value().notes) {
    attempt.rejected.push_back(RejectedAlternative{"target-noop", note, InvariantId::connectivity,
                                                   {EntityRef::global()}});
  }

  // Capability gating: an operation whose capability is absent is impossible,
  // not merely unsafe.
  for (const Operation& operation : operations) {
    const ChangeCapability capability = operation.required_capability();
    if (capability == ChangeCapability::none) {
      continue;
    }
    for (const DeviceId& subject : operation.capability_subjects(request.state)) {
      const Device* device = request.state.find_device(subject);
      if (device == nullptr) {
        // The device is created by this plan: its declared target capabilities
        // gate the operation, because no evidence can exist for it yet.
        CapabilitySet declared;
        for (const DesiredDevice& desired : request.target.devices) {
          if (desired.id == subject && desired.action == DesiredAction::ensure) {
            declared = desired.spec.capabilities;
            break;
          }
        }
        if (declared.has(capability)) {
          continue;
        }
        attempt.code = RefusalCode::capability_missing;
        attempt.summary = "capability-unsupported";
        attempt.message = "device '" + subject.str() +
                          "' is created by this plan without declaring capability '" +
                          std::string(to_string(capability)) + "' required by operation '" +
                          operation.to_string() + "'";
        attempt.rejected.push_back(RejectedAlternative{"capability", attempt.message,
                                                       InvariantId::capability_evidence,
                                                       {EntityRef::of(subject)}});
        return attempt;
      }
      const CapabilitySet supported = effective_capabilities(request, *device);
      if (!supported.has(capability)) {
        attempt.code = RefusalCode::capability_missing;
        attempt.summary = "capability-unsupported";
        attempt.message = "device '" + subject.str() + "' does not support capability '" +
                          std::string(to_string(capability)) + "' required by operation '" +
                          operation.to_string() + "'";
        attempt.rejected.push_back(RejectedAlternative{"capability", attempt.message,
                                                       InvariantId::capability_evidence,
                                                       {EntityRef::of(subject)}});
        return attempt;
      }
    }
  }

  const std::vector<Violation> initial = check_invariants(request.state, evaluation);
  ++diagnostics.invariant_evaluations;
  std::vector<Violation> hard_initial;
  for (const Violation& violation : initial) {
    if (violation.severity == Severity::violation) {
      hard_initial.push_back(violation);
    }
  }
  if (!hard_initial.empty()) {
    attempt.code = RefusalCode::unsafe_initial_state;
    attempt.summary = "current-state-unsafe";
    attempt.message =
        "the current authoritative state already violates " +
        std::to_string(hard_initial.size()) +
        " declared obligation(s); the planner never plans from an unsafe state";
    attempt.residual = hard_initial;
    return attempt;
  }

  if (operations.empty()) {
    attempt.success = true;
    attempt.plan.id = request.plan_id;
    attempt.plan.generation = request.generation;
    attempt.plan.lineage = request.lineage;
    attempt.plan.binding = request.binding();
    attempt.plan.planning_instant = request.planning_instant;
    attempt.plan.request_digest = request.request_digest();
    attempt.plan.objective = ObjectiveVector{};
    attempt.plan.verified_invariants = verified_invariants_for(request.constraints);
    attempt.plan.justification.governing_policy = request.constraints.governing_policy;
    attempt.plan.justification.governing_policy_digest = request.constraints.governing_policy_digest;
    attempt.plan.justification.authority_generation = request.state.authority_generation;
    attempt.plan.justification.checked_invariants = attempt.plan.verified_invariants;
    attempt.plan.justification.rationale_code = "already-converged";
    attempt.plan.justification.notes.push_back(
        "the authoritative state already satisfies the target intent; no steps are required");
    attempt.plan.label = request.label;
    attempt.summary = "already-converged";
    attempt.message = "no operations required";
    return attempt;
  }

  auto conflicts = derive_dependencies(request.state, operations, evaluation);
  if (!conflicts.ok()) {
    attempt.code = map_status(conflicts.status());
    attempt.summary = "dependency-derivation-failed";
    attempt.message = conflicts.status().to_string();
    return attempt;
  }
  const std::vector<DependencyEdge>& edges = conflicts.value().edges;

  std::vector<Node> nodes;
  nodes.reserve(operations.size());
  std::map<std::string, std::size_t> index_by_step;
  for (std::size_t i = 0; i < operations.size(); ++i) {
    Node node;
    node.operation = operations[i];
    node.id = step_id_for(operations[i]);
    if (!index_by_step.emplace(node.id.str(), i).second) {
      attempt.code = RefusalCode::impossible_transition;
      attempt.summary = "duplicate-operation-identity";
      attempt.message = "two derived operations share the step identity '" + node.id.str() + "'";
      return attempt;
    }
    nodes.push_back(std::move(node));
  }
  for (const DependencyEdge& edge : edges) {
    const auto from = index_by_step.find(edge.from.str());
    const auto to = index_by_step.find(edge.to.str());
    if (from == index_by_step.end() || to == index_by_step.end()) {
      continue;
    }
    nodes[to->second].predecessors.push_back(from->second);
    ++nodes[from->second].successors;
  }

  const std::vector<InvariantId> invariants = verified_invariants_for(request.constraints);
  static constexpr Strategy kStrategies[] = {Strategy::safety_first, Strategy::unlock_first,
                                             Strategy::maintenance_first,
                                             Strategy::reverse_canonical};

  bool have_best = false;
  Plan best_plan;
  ObjectiveVector best_vector;
  FailureInfo best_failure;
  std::string best_failure_message;
  bool saw_limit_exceeded = false;
  std::string limit_message;
  std::vector<RejectedAlternative> rejected;

  for (std::uint32_t candidate = 0; candidate < candidate_count && candidate < 4; ++candidate) {
    if (stop.stop_requested()) {
      attempt.cancelled = true;
      attempt.code = RefusalCode::cancelled;
      attempt.summary = "cancelled";
      attempt.message = "planning was cancelled before a plan was published";
      return attempt;
    }
    if (diagnostics.scheduling_attempts >= request.limits.max_scheduling_attempts) {
      attempt.code = RefusalCode::search_limit_exceeded;
      attempt.summary = "scheduling-attempt-budget-exhausted";
      attempt.message =
          "the scheduling attempt budget (" +
          std::to_string(request.limits.max_scheduling_attempts) +
          ") was exhausted before a candidate ordering could be completed";
      attempt.limit_exceeded = true;
      attempt.rejected = std::move(rejected);
      return attempt;
    }
    ++diagnostics.candidates_evaluated;
    const Strategy strategy = kStrategies[candidate];
    SchedulerResult scheduled =
        schedule(request, evaluation, nodes, edges, strategy, diagnostics, stop);
    if (scheduled.cancelled) {
      attempt.cancelled = true;
      attempt.code = RefusalCode::cancelled;
      attempt.summary = "cancelled";
      attempt.message = "planning was cancelled before a plan was published";
      return attempt;
    }
    if (scheduled.limit_exceeded) {
      saw_limit_exceeded = true;
      limit_message = scheduled.message;
      rejected.push_back(RejectedAlternative{"candidate-limit",
                                             std::string(strategy_code(strategy)) + ": " +
                                                 scheduled.message,
                                             InvariantId::connectivity, {EntityRef::global()}});
      continue;
    }
    if (!scheduled.ok) {
      if (scheduled.failure.has_failure && !best_failure.has_failure) {
        best_failure = scheduled.failure;
        best_failure_message = scheduled.message;
      }
      rejected.push_back(RejectedAlternative{
          "candidate-unsafe", std::string(strategy_code(strategy)) + ": " + scheduled.message,
          InvariantId::connectivity, {EntityRef::global()}});
      continue;
    }

    Plan candidate_plan;
    candidate_plan.id = request.plan_id;
    candidate_plan.generation = request.generation;
    candidate_plan.lineage = request.lineage;
    candidate_plan.binding = request.binding();
    candidate_plan.planning_instant = request.planning_instant;
    candidate_plan.request_digest = request.request_digest();
    candidate_plan.stages = scheduled.stages;
    candidate_plan.steps = scheduled.steps;
    candidate_plan.edges = edges;
    candidate_plan.verified_invariants = invariants;
    candidate_plan.objective =
        evaluate_objective_vector(candidate_plan.stages, candidate_plan.steps, edges);
    candidate_plan.justification.governing_policy = request.constraints.governing_policy;
    candidate_plan.justification.governing_policy_digest =
        request.constraints.governing_policy_digest;
    candidate_plan.justification.authority_generation = request.state.authority_generation;
    candidate_plan.justification.checked_invariants = invariants;
    candidate_plan.justification.rationale_code = strategy_code(strategy);
    candidate_plan.justification.notes.push_back(
        std::string("candidate ordering produced by strategy ") + strategy_code(strategy));
    candidate_plan.justification.notes.push_back(
        "verified " + std::to_string(scheduled.stages.size()) + " stage(s) over " +
        std::to_string(candidate_plan.steps.size()) + " step(s)");
    candidate_plan.label = request.label;
    for (Step& step : candidate_plan.steps) {
      step.justification.checked_invariants = invariants;
      step.justification.evidence.push_back(EntityRef::global());
    }

    const VerificationReport report = verify_plan(candidate_plan, request, true);
    if (!report.ok) {
      rejected.push_back(RejectedAlternative{"candidate-verification-failed",
                                             std::string(strategy_code(strategy)) + ": " +
                                                 report.explain(),
                                             InvariantId::connectivity,
                                             {EntityRef::global()}});
      continue;
    }
    candidate_plan.verification_digest = report.trace_digest;
    candidate_plan.verified_permutations = report.permutations_verified;

    if (!have_best) {
      best_plan = std::move(candidate_plan);
      best_vector = best_plan.objective;
      have_best = true;
      continue;
    }
    const Objective& objective = request.objective;
    if (objective_prefers(objective, candidate_plan.objective, best_vector)) {
      rejected.push_back(RejectedAlternative{
          "objective-loss",
          std::string("previous candidate lost: ") +
              explain_objective_loss(objective, best_vector, candidate_plan.objective),
          InvariantId::connectivity, {EntityRef::global()}});
      best_plan = std::move(candidate_plan);
      best_vector = best_plan.objective;
    } else {
      rejected.push_back(RejectedAlternative{
          "objective-loss",
          std::string(strategy_code(strategy)) + " candidate rejected: " +
              explain_objective_loss(objective, candidate_plan.objective, best_vector),
          InvariantId::connectivity, {EntityRef::global()}});
    }
  }

  if (!have_best) {
    attempt.limit_exceeded = saw_limit_exceeded;
    attempt.code = saw_limit_exceeded ? RefusalCode::search_limit_exceeded
                                      : RefusalCode::no_safe_ordering;
    attempt.summary = saw_limit_exceeded ? "scheduling-budget-exhausted" : "no-safe-ordering";
    attempt.message = saw_limit_exceeded
                          ? limit_message
                          : "no ordering of the derived operations keeps every declared "
                            "obligation satisfied; the blocked steps are reported with the "
                            "obligation that blocks them";
    if (best_failure.has_failure) {
      BlockedStep blocked;
      blocked.id = best_failure.step;
      blocked.operation = best_failure.operation;
      blocked.violations = best_failure.violations;
      blocked.unsatisfied_preconditions = best_failure.unsatisfied;
      blocked.blocking_code = best_failure.code;
      attempt.blocked.push_back(std::move(blocked));
      attempt.residual = best_failure.violations;
    }
    attempt.rejected = std::move(rejected);
    return attempt;
  }

  for (const RejectedAlternative& alternative : rejected) {
    best_plan.rejected.push_back(alternative);
  }
  attempt.success = true;
  attempt.plan = std::move(best_plan);
  attempt.summary = "plan-generated";
  attempt.message = "plan generated under strategy " + best_plan.justification.rationale_code;
  attempt.rejected = rejected;
  return attempt;
}

// Locates an instant at which every device touched by a maintenance-class
// operation has a maintenance window open and no touched device is inside an
// exclusion window.
Result<TimestampNs> shift_instant(const PlanRequest& request,
                                  const std::vector<Operation>& operations) {
  std::vector<TimestampNs> candidates;
  candidates.push_back(request.planning_instant);
  std::vector<DeviceId> subjects;
  for (const Operation& operation : operations) {
    if (!operation.requires_maintenance_window()) {
      continue;
    }
    for (const DeviceId& subject : operation.capability_subjects(request.state)) {
      subjects.push_back(subject);
    }
    if (!operation.device.empty()) {
      subjects.push_back(operation.device);
    }
  }
  std::sort(subjects.begin(), subjects.end());
  subjects.erase(std::unique(subjects.begin(), subjects.end()), subjects.end());
  for (const DeviceId& subject : subjects) {
    const Device* device = request.state.find_device(subject);
    if (device == nullptr) {
      continue;
    }
    for (const MaintenanceWindow& window : device->maintenance_windows) {
      if (window.begin >= request.planning_instant) {
        candidates.push_back(window.begin);
      }
    }
    for (const ExclusionWindow& window : device->exclusions) {
      if (window.end >= request.planning_instant) {
        candidates.push_back(window.end);
      }
    }
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
  for (const TimestampNs candidate : candidates) {
    bool usable = true;
    for (const DeviceId& subject : subjects) {
      const Device* device = request.state.find_device(subject);
      if (device == nullptr) {
        continue;
      }
      std::string reason;
      if (!change_permitted_at(*device, candidate, &reason)) {
        usable = false;
        break;
      }
      bool needs_window = false;
      for (const Operation& operation : operations) {
        if (operation.requires_maintenance_window() && operation.device == subject) {
          needs_window = true;
          break;
        }
      }
      if (needs_window && !maintenance_permitted_at(*device, candidate, &reason)) {
        usable = false;
        break;
      }
    }
    if (usable) {
      return candidate;
    }
  }
  return Status::error(ErrorCode::unsafe_transition,
                       "no instant satisfies the maintenance windows and exclusion windows of "
                       "every device touched by a maintenance operation");
}

std::vector<BlockingConstraint> minimise_blocking_core(const PlanRequest& request,
                                                       const Refusal& base,
                                                       PlanDiagnostics& diagnostics,
                                                       const StopToken& stop) {
  std::vector<BlockingConstraint> blocking;
  std::vector<std::size_t> enabled_slots;
  for (std::size_t i = 0; i < request.constraints.items.size(); ++i) {
    if (request.constraints.items[i].enabled) {
      enabled_slots.push_back(i);
    }
  }
  if (enabled_slots.empty()) {
    return blocking;
  }

  auto attempt_with = [&request, &diagnostics, &stop](const std::vector<std::size_t>& disabled,
                                                      bool& cancelled) {
    PlanRequest trial = request;
    for (const std::size_t slot : disabled) {
      trial.constraints.items[slot].enabled = false;
    }
    ++diagnostics.core_minimisation_attempts;
    Attempt attempt = run_attempt(trial, diagnostics, stop, 1);
    cancelled = attempt.cancelled;
    return attempt.success;
  };

  std::vector<std::size_t> disabled;
  bool progress = true;
  while (progress &&
         diagnostics.core_minimisation_attempts < request.limits.max_core_minimisation_attempts) {
    progress = false;
    for (const std::size_t slot : enabled_slots) {
      if (std::find(disabled.begin(), disabled.end(), slot) != disabled.end()) {
        continue;
      }
      std::vector<std::size_t> trial = disabled;
      trial.push_back(slot);
      bool cancelled = false;
      if (attempt_with(trial, cancelled)) {
        disabled = std::move(trial);
        progress = true;
        break;
      }
      if (cancelled) {
        return blocking;
      }
      if (diagnostics.core_minimisation_attempts >=
          request.limits.max_core_minimisation_attempts) {
        break;
      }
    }
  }
  if (disabled.empty()) {
    return blocking;
  }

  // Irreducibility pass: a disabled item that is not needed to unblock planning
  // is re-enabled, so the reported core is minimal.
  for (std::size_t index = 0; index < disabled.size();) {
    std::vector<std::size_t> trial;
    for (std::size_t k = 0; k < disabled.size(); ++k) {
      if (k != index) {
        trial.push_back(disabled[k]);
      }
    }
    bool cancelled = false;
    if (attempt_with(trial, cancelled)) {
      disabled = std::move(trial);
      continue;
    }
    if (cancelled) {
      break;
    }
    ++index;
  }

  for (const std::size_t slot : disabled) {
    const ConstraintItem& item = request.constraints.items[slot];
    BlockingConstraint entry;
    entry.kind = item.kind;
    entry.id = item.id;
    entry.justification_code = item.justification_code;
    switch (item.kind) {
      case ConstraintKind::connectivity:
        entry.invariant = InvariantId::connectivity;
        break;
      case ConstraintKind::failure_domain_redundancy:
        entry.invariant = InvariantId::failure_domain_redundancy;
        break;
      case ConstraintKind::path_diversity:
        entry.invariant = InvariantId::path_diversity;
        break;
      case ConstraintKind::capacity_headroom:
        entry.invariant = InvariantId::capacity_headroom;
        break;
      case ConstraintKind::maintenance_exclusion:
        entry.invariant = InvariantId::maintenance_exclusion;
        break;
      case ConstraintKind::maintenance_window:
      case ConstraintKind::drain_before_maintenance:
        entry.invariant = InvariantId::maintenance_window;
        break;
      case ConstraintKind::change_serialization:
        entry.invariant = InvariantId::change_serialization;
        break;
      case ConstraintKind::workload_contract:
        entry.invariant = InvariantId::workload_contract;
        break;
      case ConstraintKind::capability_evidence:
        entry.invariant = InvariantId::capability_evidence;
        break;
      case ConstraintKind::generation_binding:
        entry.invariant = InvariantId::generation_binding;
        break;
      case ConstraintKind::rollback_metadata:
        entry.invariant = InvariantId::service_continuity;
        break;
    }
    entry.message = "no safe ordering exists while obligation '" + item.id.str() + "' (" +
                    std::string(to_string(item.kind)) + ") is enforced";
    for (const Violation& violation : base.residual_violations) {
      if (violation.invariant == entry.invariant) {
        for (const EntityRef& reference : violation.evidence) {
          entry.evidence.push_back(reference);
        }
      }
    }
    std::sort(entry.evidence.begin(), entry.evidence.end());
    entry.evidence.erase(std::unique(entry.evidence.begin(), entry.evidence.end()),
                         entry.evidence.end());
    blocking.push_back(std::move(entry));
  }
  std::sort(blocking.begin(), blocking.end());
  return blocking;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

PlanResult generate_plan(const PlanRequest& request, const StopToken& stop) {
  PlanDiagnostics diagnostics;

  {
    const Status status = request.validate();
    if (!status.ok()) {
      return PlanResult::from_refusal(
          make_refusal(request, RefusalCode::input_invalid, "input-invalid", status.to_string()),
          diagnostics);
    }
  }

  const BindingCheck binding = check_generation_binding(request);
  if (!binding.ok) {
    return PlanResult::from_refusal(
        make_refusal(request, RefusalCode::stale_input, "stale-" + binding.dimension,
                     binding.message),
        diagnostics);
  }

  PlanRequest effective = request;
  if (request.limits.allow_window_shift) {
    auto derived = derive_operations(
        request.state, request.target,
        EvaluationContext{request.planning_instant, &request.constraints, &request.evidence,
                          &request.state});
    if (derived.ok()) {
      auto shifted = shift_instant(request, derived.value().operations);
      if (!shifted.ok()) {
        return PlanResult::from_refusal(
            make_refusal(effective, RefusalCode::contradictory_constraints,
                         "no-permitted-maintenance-instant", shifted.status().message()),
            diagnostics);
      }
      if (!(shifted.value() == request.planning_instant)) {
        diagnostics.window_shift_applied = true;
      }
      effective.planning_instant = shifted.value();
    }
  }

  Attempt attempt = run_attempt(effective, diagnostics, stop, request.limits.max_candidates);
  if (attempt.success) {
    return PlanResult::from_plan(std::move(attempt.plan), diagnostics);
  }
  if (attempt.cancelled) {
    return PlanResult::from_refusal(
        make_refusal(effective, RefusalCode::cancelled, "cancelled", attempt.message),
        diagnostics);
  }

  Refusal refusal = make_refusal(effective, attempt.code, attempt.summary, attempt.message);
  refusal.blocked_steps = attempt.blocked;
  refusal.residual_violations = attempt.residual;
  refusal.rejected = attempt.rejected;
  if (attempt.code == RefusalCode::no_safe_ordering ||
      attempt.code == RefusalCode::unsafe_initial_state ||
      attempt.code == RefusalCode::contradictory_constraints) {
    refusal.blocking = minimise_blocking_core(effective, refusal, diagnostics, stop);
  }
  std::sort(refusal.residual_violations.begin(), refusal.residual_violations.end());
  std::sort(refusal.blocked_steps.begin(), refusal.blocked_steps.end(),
            [](const BlockedStep& lhs, const BlockedStep& rhs) { return lhs.id < rhs.id; });
  std::sort(refusal.rejected.begin(), refusal.rejected.end());
  if (refusal.blocking.empty() && !refusal.residual_violations.empty()) {
    refusal.justification.notes.push_back(
        "no single declared obligation could be relaxed to obtain a plan; the block is "
        "structural");
  }
  return PlanResult::from_refusal(std::move(refusal), diagnostics);
}

}  // namespace cplan
