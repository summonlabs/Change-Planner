#include <algorithm>
#include <string>
#include <vector>

#include "change_planner/model/evaluate.hpp"
#include "change_planner/plan/graph.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/test_framework.hpp"

using namespace cplan_test;

namespace {

cplan::EvaluationContext context_of(const Scenario& scenario) {
  return cplan::EvaluationContext{scenario.request.planning_instant, &scenario.request.constraints,
                                  &scenario.request.evidence};
}

cplan::Operation device_operation(cplan::OperationKind kind, const std::string& device) {
  cplan::Operation operation;
  operation.kind = kind;
  operation.device = cplan::DeviceId::from_validated(device);
  return operation;
}

}  // namespace

CPLAN_TEST(model, snapshot_validation_rejects_dangling_and_duplicate_entities) {
  Scenario scenario = make_ladder(TopologyOptions{});
  CPLAN_CHECK_OK(scenario.state.validate());
  CPLAN_CHECK(!scenario.state.digest().is_zero());

  {
    cplan::CurrentStateSnapshot broken = scenario.state;
    broken.links.push_back(broken.links.front());
    CPLAN_CHECK_ERROR(broken.validate(), cplan::ErrorCode::duplicate_identity);
  }
  {
    cplan::CurrentStateSnapshot broken = scenario.state;
    broken.routes.front().spec.path.push_back(cplan::LinkId::from_validated("l-missing"));
    CPLAN_CHECK_ERROR(broken.validate(), cplan::ErrorCode::invalid_argument);
  }
  {
    cplan::CurrentStateSnapshot broken = scenario.state;
    broken.workloads.front().bindings.push_back(
        cplan::RouteBinding{cplan::RouteId::from_validated("r-missing"), true,
                            cplan::DemandUnits(10)});
    CPLAN_CHECK_ERROR(broken.validate(), cplan::ErrorCode::invalid_argument);
  }
  {
    cplan::CurrentStateSnapshot broken = scenario.state;
    broken.devices.front().exclusions.push_back(
        cplan::ExclusionWindow{cplan::WindowId::from_validated("ex-bad"), cplan::TimestampNs(10),
                               cplan::TimestampNs(5), "inverted"});
    CPLAN_CHECK_ERROR(broken.validate(), cplan::ErrorCode::invalid_argument);
  }
  {
    cplan::CurrentStateSnapshot broken = scenario.state;
    broken.authority = cplan::AuthorityId{};
    CPLAN_CHECK_ERROR(broken.validate(), cplan::ErrorCode::invalid_argument);
  }
}

CPLAN_TEST(model, canonicalization_is_order_independent) {
  Scenario scenario = make_ladder(TopologyOptions{});
  cplan::CurrentStateSnapshot shuffled = scenario.state;
  std::reverse(shuffled.devices.begin(), shuffled.devices.end());
  std::reverse(shuffled.links.begin(), shuffled.links.end());
  std::reverse(shuffled.routes.begin(), shuffled.routes.end());
  shuffled.canonicalize();
  CPLAN_CHECK_EQ(shuffled.digest(), scenario.state.digest());
}

CPLAN_TEST(model, operation_application_is_structural_and_typed) {
  Scenario scenario = make_ladder(TopologyOptions{});
  const cplan::EvaluationContext context = context_of(scenario);

  // Capacity change applies and is visible in the resulting state.
  cplan::Operation capacity = device_operation(cplan::OperationKind::device_remove, "mid-1");
  capacity.kind = cplan::OperationKind::link_set_capacity;
  capacity.link = cplan::LinkId::from_validated("l-a1");
  capacity.capacity = cplan::CapacityUnits(12345);
  auto applied = cplan::apply_operation(scenario.state, capacity, context);
  CPLAN_REQUIRE_OK(applied);
  CPLAN_CHECK_EQ(applied.value().state.find_link(cplan::LinkId::from_validated("l-a1"))
                     ->spec.capacity.value(),
                 12345u);

  // Removing the device without draining is impossible under the default policy.
  auto premature = cplan::apply_operation(
      scenario.state, device_operation(cplan::OperationKind::device_remove, "mid-1"), context);
  CPLAN_CHECK_ERROR(premature, cplan::ErrorCode::unsafe_transition);

  // Drain then remove succeeds.
  auto drained = cplan::apply_operation(
      scenario.state, device_operation(cplan::OperationKind::drain_resource, "mid-1"), context);
  CPLAN_REQUIRE_OK(drained);
  auto removed = cplan::apply_operation(
      drained.value().state, device_operation(cplan::OperationKind::device_remove, "mid-1"),
      context);
  CPLAN_REQUIRE_OK(removed);
  CPLAN_CHECK(removed.value().state.find_device(cplan::DeviceId::from_validated("mid-1"))
                  ->spec.state == cplan::EntityState::removed);

  // Adding an already present device is a conflict, not a silent overwrite.
  cplan::Operation add = device_operation(cplan::OperationKind::device_add, "ep-a");
  add.device_spec = scenario.state.find_device(cplan::DeviceId::from_validated("ep-a"))->spec;
  CPLAN_CHECK_ERROR(cplan::apply_operation(scenario.state, add, context),
                    cplan::ErrorCode::conflict);

  // A route with a disconnected path is an impossible transition.
  cplan::Operation route_add;
  route_add.kind = cplan::OperationKind::route_add;
  route_add.route = cplan::RouteId::from_validated("r-bad");
  route_add.route_spec.source = cplan::DeviceId::from_validated("ep-a");
  route_add.route_spec.sink = cplan::DeviceId::from_validated("ep-b");
  route_add.route_spec.path = {cplan::LinkId::from_validated("l-a1"),
                               cplan::LinkId::from_validated("l-2b")};
  CPLAN_CHECK_ERROR(cplan::apply_operation(scenario.state, route_add, context),
                    cplan::ErrorCode::unsafe_transition);
}

CPLAN_TEST(model, conditions_are_evaluated_not_assumed) {
  Scenario scenario = make_ladder(TopologyOptions{});
  const cplan::EvaluationContext context = context_of(scenario);

  cplan::Condition present;
  present.kind = cplan::ConditionKind::entity_present;
  present.subject = cplan::EntityRef::of(cplan::DeviceId::from_validated("ep-a"));
  auto present_value = cplan::evaluate_condition(scenario.state, present, context);
  CPLAN_REQUIRE_OK(present_value);
  CPLAN_CHECK(present_value.value());

  cplan::Condition walkable;
  walkable.kind = cplan::ConditionKind::route_walkable;
  walkable.subject = cplan::EntityRef::of(cplan::RouteId::from_validated("r-1"));
  auto walkable_value = cplan::evaluate_condition(scenario.state, walkable, context);
  CPLAN_REQUIRE_OK(walkable_value);
  CPLAN_CHECK(walkable_value.value());

  // A condition whose parameter cannot be interpreted fails instead of defaulting.
  cplan::Condition unknown;
  unknown.kind = cplan::ConditionKind::invariant_satisfied;
  unknown.parameter = "not-an-invariant";
  CPLAN_CHECK_ERROR(cplan::evaluate_condition(scenario.state, unknown, context),
                    cplan::ErrorCode::malformed_input);

  // Window conditions depend on the planning instant.
  cplan::Condition window;
  window.kind = cplan::ConditionKind::maintenance_window_open;
  window.subject = cplan::EntityRef::of(cplan::DeviceId::from_validated("mid-1"));
  auto open = cplan::evaluate_condition(scenario.state, window, context);
  CPLAN_REQUIRE_OK(open);
  CPLAN_CHECK(open.value());
  cplan::EvaluationContext later = context;
  later.instant = cplan::TimestampNs(9000000000);
  auto closed = cplan::evaluate_condition(scenario.state, window, later);
  CPLAN_REQUIRE_OK(closed);
  CPLAN_CHECK(!closed.value());
}

CPLAN_TEST(model, derived_preconditions_and_postconditions_hold) {
  Scenario scenario = make_ladder(TopologyOptions{});
  const cplan::EvaluationContext context = context_of(scenario);

  cplan::Operation drain = device_operation(cplan::OperationKind::drain_resource, "mid-1");
  const std::vector<cplan::Condition> preconditions =
      cplan::derive_preconditions(scenario.state, drain, context);
  CPLAN_CHECK(!preconditions.empty());
  for (const cplan::Condition& condition : preconditions) {
    auto satisfied = cplan::evaluate_condition(scenario.state, condition, context);
    CPLAN_REQUIRE_OK(satisfied);
    CPLAN_CHECK(satisfied.value());
  }
  auto applied = cplan::apply_operation(scenario.state, drain, context);
  CPLAN_REQUIRE_OK(applied);
  const std::vector<cplan::Condition> postconditions =
      cplan::derive_postconditions(scenario.state, drain, context);
  CPLAN_CHECK(!postconditions.empty());
  for (const cplan::Condition& condition : postconditions) {
    auto satisfied = cplan::evaluate_condition(applied.value().state, condition, context);
    CPLAN_REQUIRE_OK(satisfied);
    CPLAN_CHECK(satisfied.value());
  }
}

CPLAN_TEST(model, dependency_derivation_orders_prerequisites) {
  Scenario scenario = make_ladder(TopologyOptions{});
  const cplan::EvaluationContext context = context_of(scenario);

  std::vector<cplan::Operation> operations;
  operations.push_back(device_operation(cplan::OperationKind::device_remove, "mid-0"));
  operations.push_back(device_operation(cplan::OperationKind::drain_resource, "mid-0"));
  CPLAN_CHECK_OK(scenario.state.validate());

  auto conflicts = cplan::derive_dependencies(scenario.state, operations, context);
  CPLAN_REQUIRE_OK(conflicts);
  CPLAN_CHECK_EQ(conflicts.value().edges.size(), static_cast<std::size_t>(1));
  const cplan::DependencyEdge& edge = conflicts.value().edges.front();
  CPLAN_CHECK_EQ(edge.from, cplan::step_id_for(operations[1]));
  CPLAN_CHECK_EQ(edge.to, cplan::step_id_for(operations[0]));
  CPLAN_CHECK(edge.reason == cplan::DependencyReason::structural_prerequisite);
}

CPLAN_TEST(model, step_identity_is_content_derived_and_stable) {
  Scenario scenario = make_ladder(TopologyOptions{});
  cplan::Operation first = device_operation(cplan::OperationKind::drain_resource, "mid-1");
  cplan::Operation second = first;
  CPLAN_CHECK_EQ(cplan::step_id_for(first), cplan::step_id_for(second));
  second.risk = cplan::RiskScore(5);
  CPLAN_CHECK(!(cplan::step_id_for(first) == cplan::step_id_for(second)));
  CPLAN_CHECK(!(scenario.state.digest() == cplan::Digest{}));
}

CPLAN_TEST(model, compensation_metadata_is_derived_from_state) {
  Scenario scenario = make_ladder(TopologyOptions{});
  const cplan::EvaluationContext context = context_of(scenario);
  cplan::Operation capacity;
  capacity.kind = cplan::OperationKind::link_set_capacity;
  capacity.link = cplan::LinkId::from_validated("l-a1");
  capacity.capacity = cplan::CapacityUnits(20000);
  const cplan::Compensation compensation =
      cplan::derive_compensation(scenario.state, capacity, context);
  CPLAN_CHECK(compensation.available);
  CPLAN_CHECK(compensation.kind == cplan::CompensationKind::inverse);
  CPLAN_CHECK_EQ(compensation.inverse.capacity.value(), 10000u);
}

CPLAN_TEST(model, operations_are_totally_ordered_deterministically) {
  Scenario scenario = make_ladder(TopologyOptions{});
  std::vector<cplan::Operation> operations;
  for (const std::string& id : {"mid-2", "ep-a", "mid-1"}) {
    operations.push_back(device_operation(cplan::OperationKind::drain_resource, id));
  }
  std::vector<cplan::Operation> sorted = operations;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    CPLAN_CHECK(sorted[i - 1] < sorted[i]);
  }
  // Sorting a permutation yields the identical order.
  std::reverse(operations.begin(), operations.end());
  std::sort(operations.begin(), operations.end());
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    CPLAN_CHECK(sorted[i] == operations[i]);
  }
}
