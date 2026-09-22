#include <algorithm>
#include <string>
#include <vector>

#include "change_planner/model/evaluate.hpp"
#include "change_planner/plan/compare.hpp"
#include "change_planner/plan/graph.hpp"
#include "change_planner/plan/invalidate.hpp"
#include "change_planner/plan/replan.hpp"
#include "change_planner/plan/verify.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/test_framework.hpp"

using namespace cplan_test;

namespace {

cplan::CapabilitySet without(cplan::CapabilitySet capabilities, cplan::ChangeCapability capability) {
  return cplan::CapabilitySet(capabilities.mask() & ~static_cast<std::uint32_t>(capability));
}

cplan::EvaluationContext context_of(const Scenario& scenario) {
  return cplan::EvaluationContext{scenario.request.planning_instant, &scenario.request.constraints,
                                  &scenario.request.evidence};
}

bool has_operation_kind(const cplan::Plan& plan, cplan::OperationKind kind) {
  for (const cplan::Step& step : plan.steps) {
    if (step.operation.kind == kind) {
      return true;
    }
  }
  return false;
}

std::size_t stage_of(const cplan::Plan& plan, cplan::OperationKind kind) {
  for (const cplan::Step& step : plan.steps) {
    if (step.operation.kind == kind) {
      return step.stage.value();
    }
  }
  return static_cast<std::size_t>(-1);
}

}  // namespace

CPLAN_TEST(planner, generates_capacity_change_plan) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 5000);
  }
  refresh_request(scenario);

  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());
  const cplan::Plan& plan = result.plan();
  CPLAN_CHECK_EQ(plan.steps.size(), static_cast<std::size_t>(4));
  CPLAN_CHECK_EQ(plan.stages.size(), static_cast<std::size_t>(1));
  for (const cplan::Step& step : plan.steps) {
    CPLAN_CHECK(step.operation.kind == cplan::OperationKind::link_set_capacity);
    CPLAN_CHECK(!step.preconditions.empty());
    CPLAN_CHECK(!step.postconditions.empty());
    CPLAN_CHECK(step.compensation.available);
  }
  CPLAN_CHECK(plan.verify_structure().ok());
  CPLAN_CHECK_EQ(plan.verified_permutations, static_cast<std::uint64_t>(24));

  const cplan::VerificationReport report = cplan::verify_plan(plan, scenario.request, true);
  CPLAN_CHECK(report.ok);

  auto final_state = apply_plan(plan, scenario.request);
  CPLAN_REQUIRE_OK(final_state);
  for (const cplan::Link& link : final_state.value().links) {
    CPLAN_CHECK_EQ(link.spec.capacity.value(), 15000u);
  }
  CPLAN_CHECK_EQ(hard_violation_count(cplan::check_invariants(final_state.value(), context_of(scenario))),
                 static_cast<std::size_t>(0));
}

CPLAN_TEST(planner, identical_inputs_produce_identical_plans) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 2500);
  }
  scenario.target.policies.push_back(cplan::DesiredPolicy{
      cplan::DesiredAction::ensure, cplan::PolicyId::from_validated("p-fabric"),
      cplan::PolicyTarget{cplan::PolicyScope::global, std::string()}, "value-a", true});
  scenario.target.configs.push_back(cplan::DesiredConfig{
      cplan::DesiredAction::ensure, cplan::DeviceId::from_validated("ep-a"),
      cplan::ConfigKey::from_validated("mtu"), "9000"});
  refresh_request(scenario);

  const cplan::PlanResult first = run_plan(scenario);
  const cplan::PlanResult second = run_plan(scenario);
  CPLAN_REQUIRE(first.ok());
  CPLAN_REQUIRE(second.ok());
  CPLAN_CHECK_EQ(first.plan().content_digest(), second.plan().content_digest());
  CPLAN_CHECK(first.plan().semantically_identical(second.plan()));

  // Deterministic objective metrics too.
  CPLAN_CHECK_EQ(first.plan().objective.stages.value(), second.plan().objective.stages.value());
  CPLAN_CHECK_EQ(first.plan().objective.risk_exposure.value(),
                 second.plan().objective.risk_exposure.value());
  CPLAN_CHECK_EQ(first.plan().objective.churn, second.plan().objective.churn);

  const cplan::PlanComparison comparison = cplan::compare_plans(first.plan(), second.plan());
  CPLAN_CHECK(comparison.identical);
  CPLAN_CHECK(comparison.differences.empty());
}

CPLAN_TEST(planner, scheduled_order_respects_dependencies) {
  Scenario scenario = make_ladder(TopologyOptions{});
  // Tear down the second path and lower the contract to a single path.
  for (cplan::DesiredRoute& route : scenario.target.routes) {
    if (route.id.str() == "r-2") {
      route.action = cplan::DesiredAction::remove;
    }
  }
  for (cplan::DesiredLink& link : scenario.target.links) {
    if (link.id.str() == "l-a2" || link.id.str() == "l-2b") {
      link.action = cplan::DesiredAction::remove;
    }
  }
  for (cplan::DesiredDevice& device : scenario.target.devices) {
    if (device.id.str() == "mid-2") {
      device.action = cplan::DesiredAction::remove;
    }
  }
  for (cplan::DesiredWorkload& workload : scenario.target.workloads) {
    workload.bindings.erase(
        std::remove_if(workload.bindings.begin(), workload.bindings.end(),
                       [](const cplan::RouteBinding& binding) {
                         return binding.route.str() == "r-2";
                       }),
        workload.bindings.end());
    workload.requirements.min_link_disjoint_paths = 1;
  }
  refresh_request(scenario);

  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());
  const cplan::Plan& plan = result.plan();

  CPLAN_CHECK(has_operation_kind(plan, cplan::OperationKind::workload_set_bindings));
  CPLAN_CHECK(has_operation_kind(plan, cplan::OperationKind::route_remove));
  CPLAN_CHECK(has_operation_kind(plan, cplan::OperationKind::drain_resource));
  CPLAN_CHECK(has_operation_kind(plan, cplan::OperationKind::device_remove));
  CPLAN_CHECK(has_operation_kind(plan, cplan::OperationKind::link_remove));

  CPLAN_CHECK_LT(stage_of(plan, cplan::OperationKind::workload_set_bindings),
                 stage_of(plan, cplan::OperationKind::route_remove));
  CPLAN_CHECK_LT(stage_of(plan, cplan::OperationKind::drain_resource),
                 stage_of(plan, cplan::OperationKind::device_remove));

  const cplan::VerificationReport report = cplan::verify_plan(plan, scenario.request, true);
  CPLAN_CHECK(report.ok);
}

CPLAN_TEST(planner, refuses_when_no_safe_ordering_exists) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredDevice& device : scenario.target.devices) {
    if (device.id.str() == "mid-1" || device.id.str() == "mid-2") {
      device.action = cplan::DesiredAction::remove;
    }
  }
  refresh_request(scenario);

  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(!result.ok());
  const cplan::Refusal& refusal = result.refusal();
  CPLAN_CHECK(refusal.code == cplan::RefusalCode::no_safe_ordering ||
              refusal.code == cplan::RefusalCode::contradictory_constraints ||
              refusal.code == cplan::RefusalCode::unsafe_initial_state);
  CPLAN_CHECK(!refusal.explain().empty());
  CPLAN_CHECK(!refusal.justification.notes.empty());
}

CPLAN_TEST(planner, converged_target_produces_empty_plan) {
  Scenario scenario = make_ladder(TopologyOptions{});
  refresh_request(scenario);
  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());
  CPLAN_CHECK(result.plan().steps.empty());
  CPLAN_CHECK(result.plan().stages.empty());
  CPLAN_CHECK(result.plan().verify_structure().ok());
  CPLAN_CHECK_EQ(result.plan().justification.rationale_code, std::string("already-converged"));
}

CPLAN_TEST(planner, fences_stale_generation_inputs) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 1000);
  }
  refresh_request(scenario);
  scenario.request.target.based_on_generation = cplan::TopologyGeneration(999);

  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(!result.ok());
  CPLAN_CHECK(result.refusal().code == cplan::RefusalCode::stale_input);
  CPLAN_CHECK(result.refusal().summary_code.find("topology-generation") != std::string::npos);
}

CPLAN_TEST(planner, refuses_when_capability_evidence_is_insufficient) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredDevice& device : scenario.target.devices) {
    if (device.id.str() == "mid-1" || device.id.str() == "mid-2") {
      device.action = cplan::DesiredAction::remove;
    }
  }
  refresh_request(scenario);
  for (cplan::CapabilityEvidence& entry : scenario.request.evidence.devices) {
    entry.capabilities = without(entry.capabilities, cplan::ChangeCapability::device_remove);
  }

  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(!result.ok());
  CPLAN_CHECK(result.refusal().code == cplan::RefusalCode::capability_missing);
  CPLAN_CHECK(result.refusal().explain().find("device-remove") != std::string::npos);
}

CPLAN_TEST(planner, maintenance_window_gating_refuses_outside_window) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredDevice& device : scenario.target.devices) {
    if (device.id.str() == "mid-1" || device.id.str() == "mid-2") {
      device.action = cplan::DesiredAction::remove;
    }
  }
  // Push the planning instant outside every maintenance window.
  scenario.state.observed_at = cplan::TimestampNs(5000000000);
  refresh_request(scenario);
  scenario.request.planning_instant = cplan::TimestampNs(5000000000);

  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(!result.ok());
  CPLAN_CHECK(result.refusal().explain().find("maintenance") != std::string::npos);
}

CPLAN_TEST(planner, window_shift_finds_a_permitted_instant) {
  TopologyOptions options;
  options.with_exclusions = true;  // the requested instant is inside an exclusion window
  Scenario scenario = make_ladder(options);
  for (cplan::DesiredDevice& device : scenario.target.devices) {
    if (device.id.str() == "mid-2") {
      device.action = cplan::DesiredAction::remove;
    }
  }
  for (cplan::DesiredRoute& route : scenario.target.routes) {
    if (route.id.str() == "r-2") {
      route.action = cplan::DesiredAction::remove;
    }
  }
  for (cplan::DesiredLink& link : scenario.target.links) {
    if (link.id.str() == "l-a2" || link.id.str() == "l-2b") {
      link.action = cplan::DesiredAction::remove;
    }
  }
  for (cplan::DesiredWorkload& workload : scenario.target.workloads) {
    workload.bindings.erase(
        std::remove_if(workload.bindings.begin(), workload.bindings.end(),
                       [](const cplan::RouteBinding& binding) {
                         return binding.route.str() == "r-2";
                       }),
        workload.bindings.end());
    workload.requirements.min_link_disjoint_paths = 1;
  }
  refresh_request(scenario);
  const cplan::TimestampNs requested = scenario.request.planning_instant;

  // Without a shift the planner refuses: the instant sits inside an exclusion.
  {
    const cplan::PlanResult refused = run_plan(scenario);
    CPLAN_REQUIRE(!refused.ok());
    CPLAN_CHECK(refused.refusal().explain().find("exclusion") != std::string::npos);
  }

  cplan::PlanningLimits limits;
  limits.allow_window_shift = true;
  scenario.request.limits = limits;
  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());
  CPLAN_CHECK(result.diagnostics.window_shift_applied);
  CPLAN_CHECK(result.plan().planning_instant > requested);
  CPLAN_CHECK(cplan::verify_plan(result.plan(), scenario.request, true).ok);
}

CPLAN_TEST(planner, window_shift_refuses_when_no_window_opens_later) {
  TopologyOptions options;
  options.with_exclusions = true;
  Scenario scenario = make_ladder(options);
  for (cplan::DesiredDevice& device : scenario.target.devices) {
    if (device.id.str() == "mid-2") {
      device.action = cplan::DesiredAction::remove;
    }
  }
  refresh_request(scenario);
  // Every maintenance window is in the past, and the requested instant is inside
  // an exclusion: no instant can satisfy both obligations.
  scenario.request.planning_instant = cplan::TimestampNs(9000000000);
  cplan::PlanningLimits limits;
  limits.allow_window_shift = true;
  scenario.request.limits = limits;

  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(!result.ok());
  CPLAN_CHECK(result.refusal().code == cplan::RefusalCode::contradictory_constraints);
  CPLAN_CHECK(result.refusal().summary_code == std::string("no-permitted-maintenance-instant"));
  CPLAN_CHECK(!result.refusal().justification.notes.empty());
}

CPLAN_TEST(planner, parallel_stages_are_independent_and_bounded) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 1);
  }
  refresh_request(scenario);
  scenario.request.limits.max_stage_width = 2;

  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());
  for (const cplan::Stage& stage : result.plan().stages) {
    CPLAN_CHECK(stage.steps.size() <= 2);
    CPLAN_CHECK(stage.verified_permutations >= 1);
  }
  CPLAN_CHECK(cplan::verify_plan(result.plan(), scenario.request, true).ok);
}

CPLAN_TEST(planner, objective_priority_changes_selected_plan) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 100);
  }
  refresh_request(scenario);

  const auto minimum_stages = cplan::Objective::parse_code("min-stages");
  CPLAN_REQUIRE_OK(minimum_stages);
  refresh_request(scenario, minimum_stages.value());
  const cplan::PlanResult staged = run_plan(scenario);
  CPLAN_REQUIRE(staged.ok());

  const auto minimum_risk = cplan::Objective::parse_code("min-risk");
  CPLAN_REQUIRE_OK(minimum_risk);
  refresh_request(scenario, minimum_risk.value());
  const cplan::PlanResult risk_first = run_plan(scenario);
  CPLAN_REQUIRE(risk_first.ok());

  CPLAN_CHECK(!(staged.plan().objective.stages > risk_first.plan().objective.stages));
  CPLAN_CHECK(!(risk_first.plan().objective.risk_exposure > staged.plan().objective.risk_exposure));
}

CPLAN_TEST(planner, reports_rejected_alternatives_with_reasons) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 100);
  }
  refresh_request(scenario);
  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());
  CPLAN_CHECK_GT(result.diagnostics.candidates_evaluated, 1u);
  for (const cplan::RejectedAlternative& alternative : result.plan().rejected) {
    CPLAN_CHECK(!alternative.code.empty());
    CPLAN_CHECK(!alternative.detail.empty());
  }
}
