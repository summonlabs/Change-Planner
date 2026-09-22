#include <algorithm>
#include <string>
#include <vector>

#include "change_planner/cli/io.hpp"
#include "change_planner/model/evaluate.hpp"
#include "change_planner/persist/artifact.hpp"
#include "change_planner/persist/repository.hpp"
#include "change_planner/plan/verify.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/test_framework.hpp"

using namespace cplan_test;

#ifndef CPLAN_EXAMPLE_DATA_DIR
#define CPLAN_EXAMPLE_DATA_DIR "examples/data"
#endif

namespace {

std::string request_path() {
  return std::string(CPLAN_EXAMPLE_DATA_DIR) + "/ladder-request.json";
}

cplan::PlanRequest loaded_request() {
  auto document = cplan::cli::read_json_file(request_path());
  auto request = cplan::cli::parse_plan_request(document.value());
  return request.value();
}

}  // namespace

CPLAN_TEST(adversarial, request_documents_reject_malformed_shapes) {
  const std::string valid = R"({"planId":"p","state":{},"target":{},"constraints":{"items":[]},"evidence":{}})";

  struct Case {
    const char* document;
    const char* what;
  };
  const Case cases[] = {
      {"[]", "top-level array instead of object"},
      {"{\"planId\":42}", "identity given as a number"},
      {"{\"planId\":\"\"}", "empty plan identity"},
      {"{\"planId\":\"has space\"}", "identity with a space"},
      {"{\"planId\":\"p\"}", "missing state"},
      {"{\"planId\":\"p\",\"state\":{},\"target\":{}}", "missing constraints"},
  };
  for (const Case& test_case : cases) {
    const auto parsed = cplan::parse_json(test_case.document);
    CPLAN_REQUIRE_OK(parsed);
    const auto request = cplan::cli::parse_plan_request(parsed.value());
    CPLAN_CHECK(!request.ok());
  }
  const auto valid_parsed = cplan::parse_json(valid);
  CPLAN_REQUIRE_OK(valid_parsed);
  const auto request = cplan::cli::parse_plan_request(valid_parsed.value());
  CPLAN_CHECK(!request.ok());  // constraints items exist but generation binding is absent

  // A well-formed but internally inconsistent state document is rejected.
  const auto state_document = cplan::parse_json(
      R"({"topologyGeneration":1,"authority":"a","devices":[{"id":"d1","domain":"x","terminationCapacity":10},{"id":"d1","domain":"x","terminationCapacity":10}]})");
  CPLAN_REQUIRE_OK(state_document);
  CPLAN_CHECK_ERROR(cplan::cli::parse_state_document(state_document.value()),
                    cplan::ErrorCode::duplicate_identity);

  const auto dangling = cplan::parse_json(
      R"({"topologyGeneration":1,"authority":"a","devices":[{"id":"d1","domain":"x"}],"links":[{"id":"l1","endpointA":"d1","endpointB":"ghost"}]})");
  CPLAN_REQUIRE_OK(dangling);
  CPLAN_CHECK_ERROR(cplan::cli::parse_state_document(dangling.value()),
                    cplan::ErrorCode::invalid_argument);

  const auto bad_enum = cplan::parse_json(
      R"({"topologyGeneration":1,"authority":"a","devices":[{"id":"d1","domain":"x","state":"teleported"}]})");
  CPLAN_REQUIRE_OK(bad_enum);
  CPLAN_CHECK_ERROR(cplan::cli::parse_state_document(bad_enum.value()),
                    cplan::ErrorCode::malformed_input);
}

CPLAN_TEST(adversarial, contradictory_constraints_are_reported_not_ignored) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 100);
  }
  // A headroom obligation of 1000 permille forbids any traffic at all.
  for (cplan::ConstraintItem& item : scenario.request.constraints.items) {
    if (item.kind == cplan::ConstraintKind::capacity_headroom) {
      item.headroom = cplan::Permille(1000);
    }
  }
  refresh_request(scenario);
  for (cplan::ConstraintItem& item : scenario.request.constraints.items) {
    if (item.kind == cplan::ConstraintKind::capacity_headroom) {
      item.headroom = cplan::Permille(1000);
    }
  }
  scenario.constraints = scenario.request.constraints;

  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(!result.ok());
  CPLAN_CHECK(result.refusal().code == cplan::RefusalCode::unsafe_initial_state ||
              result.refusal().code == cplan::RefusalCode::contradictory_constraints ||
              result.refusal().code == cplan::RefusalCode::no_safe_ordering);
  CPLAN_CHECK(!result.refusal().residual_violations.empty() ||
              !result.refusal().blocking.empty());
}

CPLAN_TEST(adversarial, impossible_transitions_are_typed_failures) {
  Scenario scenario = make_ladder(TopologyOptions{});
  const cplan::EvaluationContext context{scenario.request.planning_instant,
                                         &scenario.request.constraints,
                                         &scenario.request.evidence};

  {  // Route over a link that does not exist
    cplan::Operation operation;
    operation.kind = cplan::OperationKind::route_add;
    operation.route = cplan::RouteId::from_validated("r-missing-link");
    operation.route_spec.source = cplan::DeviceId::from_validated("ep-a");
    operation.route_spec.sink = cplan::DeviceId::from_validated("ep-b");
    operation.route_spec.path = {cplan::LinkId::from_validated("l-ghost")};
    CPLAN_CHECK_ERROR(cplan::apply_operation(scenario.state, operation, context),
                      cplan::ErrorCode::not_found);
  }
  {  // Removing something that is not there
    cplan::Operation operation;
    operation.kind = cplan::OperationKind::device_remove;
    operation.device = cplan::DeviceId::from_validated("mid-9");
    CPLAN_CHECK_ERROR(cplan::apply_operation(scenario.state, operation, context),
                      cplan::ErrorCode::not_found);
  }
  {  // Route removal while a workload still binds it
    cplan::Operation operation;
    operation.kind = cplan::OperationKind::route_remove;
    operation.route = cplan::RouteId::from_validated("r-1");
    CPLAN_CHECK_ERROR(cplan::apply_operation(scenario.state, operation, context),
                      cplan::ErrorCode::conflict);
  }
  {  // Link endpoints that are identical
    cplan::Operation operation;
    operation.kind = cplan::OperationKind::link_add;
    operation.link = cplan::LinkId::from_validated("l-self");
    operation.link_spec.endpoint_a = cplan::DeviceId::from_validated("ep-a");
    operation.link_spec.endpoint_b = cplan::DeviceId::from_validated("ep-a");
    CPLAN_CHECK_ERROR(cplan::apply_operation(scenario.state, operation, context),
                      cplan::ErrorCode::unsafe_transition);
  }
  {  // Capacity below the reserved floor
    cplan::Operation operation;
    operation.kind = cplan::OperationKind::link_set_capacity;
    operation.link = cplan::LinkId::from_validated("l-a1");
    operation.capacity = cplan::CapacityUnits(0);
    cplan::CurrentStateSnapshot reserved = scenario.state;
    for (cplan::Link& link : reserved.links) {
      if (link.id.str() == "l-a1") {
        link.spec.reserved = cplan::CapacityUnits(500);
      }
    }
    CPLAN_CHECK_ERROR(cplan::apply_operation(reserved, operation, context),
                      cplan::ErrorCode::capacity_exceeded);
  }
  {  // Target that asks for an entity the snapshot never had
    Scenario broken = make_ladder(TopologyOptions{});
    cplan::DesiredDevice ghost_device;
    ghost_device.action = cplan::DesiredAction::ensure;
    ghost_device.id = cplan::DeviceId::from_validated("ghost");
    ghost_device.spec.domain = cplan::FailureDomainId::from_validated("dom-x");
    ghost_device.spec.termination_capacity = cplan::CapacityUnits(100);
    ghost_device.spec.capabilities = all_capabilities();
    broken.target.devices.push_back(ghost_device);
    cplan::DesiredLink ghost_link;
    ghost_link.action = cplan::DesiredAction::ensure;
    ghost_link.id = cplan::LinkId::from_validated("l-ghost");
    ghost_link.spec.endpoint_a = cplan::DeviceId::from_validated("ghost");
    ghost_link.spec.endpoint_b = cplan::DeviceId::from_validated("ep-a");
    ghost_link.spec.capacity = cplan::CapacityUnits(10);
    broken.target.links.push_back(ghost_link);
    refresh_request(broken);
    const cplan::PlanResult result = run_plan(broken);
    if (!result.ok()) {
      // Diagnostics only fire on failure: this is the planner's own explanation.
      std::printf("    planner refused: %s\n", result.refusal().explain().c_str());
    }
    CPLAN_CHECK(result.ok());  // the ghost device and link are created by the intent

    Scenario dangling = make_ladder(TopologyOptions{});
    cplan::DesiredLink dangling_link;
    dangling_link.id = cplan::LinkId::from_validated("l-dangling");
    dangling_link.spec.endpoint_a = cplan::DeviceId::from_validated("ep-a");
    dangling_link.spec.endpoint_b = cplan::DeviceId::from_validated("never-declared");
    dangling_link.spec.capacity = cplan::CapacityUnits(10);
    dangling.target.links.push_back(dangling_link);
    refresh_request(dangling);
    const cplan::PlanResult refused = run_plan(dangling);
    CPLAN_REQUIRE(!refused.ok());
    CPLAN_CHECK(refused.refusal().code == cplan::RefusalCode::unsatisfiable_target);
    CPLAN_CHECK(refused.refusal().explain().find("never-declared") != std::string::npos);
  }
}

CPLAN_TEST(adversarial, resource_limits_refuse_instead_of_truncating) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 1);
  }
  refresh_request(scenario);
  scenario.request.limits.max_steps = 2;
  const cplan::PlanResult limited = run_plan(scenario);
  CPLAN_REQUIRE(!limited.ok());
  CPLAN_CHECK(limited.refusal().code == cplan::RefusalCode::search_limit_exceeded);
  CPLAN_CHECK(limited.refusal().explain().find("budget") != std::string::npos);

  refresh_request(scenario);
  scenario.request.limits.max_permutation_checks = 1;
  scenario.request.limits.max_states_visited = 4;
  const cplan::PlanResult starved = run_plan(scenario);
  CPLAN_REQUIRE(!starved.ok());
  CPLAN_CHECK(starved.refusal().code == cplan::RefusalCode::search_limit_exceeded);
}

CPLAN_TEST(adversarial, plan_artifacts_reject_hostile_rewrites) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 1);
  }
  refresh_request(scenario);
  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());
  const std::vector<std::byte> bytes = cplan::serialize_plan_artifact(result.plan());

  for (std::size_t index = 0; index < bytes.size(); index += 7) {
    std::vector<std::byte> mutated = bytes;
    mutated[index] ^= std::byte{0x5A};
    const auto parsed = cplan::parse_plan_artifact(mutated);
    if (parsed.ok()) {
      // A mutation may only be accepted when it is provably harmless: the
      // restored plan must be byte-identical to the original.
      CPLAN_CHECK_EQ(parsed.value().plan.content_digest(), result.plan().content_digest());
    }
  }
  // Truncations at every 16-byte boundary must be rejected, never partially read.
  for (std::size_t size = 0; size < bytes.size(); size += 16) {
    std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + size);
    CPLAN_CHECK(!cplan::parse_plan_artifact(truncated).ok());
  }
}

CPLAN_TEST(adversarial, repository_accounting_returns_to_baseline) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 1);
  }
  refresh_request(scenario);
  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());

  cplan::PlanRepository repository(4);
  for (std::uint32_t generation = 1; generation <= 50; ++generation) {
    cplan::Plan plan = result.plan();
    plan.generation = cplan::PlanGeneration(generation);
    CPLAN_CHECK_OK(repository.publish(plan));
  }
  CPLAN_CHECK_EQ(repository.size(), static_cast<std::size_t>(4));
  CPLAN_CHECK_EQ(repository.evictions(), static_cast<std::uint64_t>(46));
  CPLAN_CHECK_EQ(repository.retire_below(cplan::PlanGeneration(1000)),
                 static_cast<std::size_t>(4));
  CPLAN_CHECK_EQ(repository.size(), static_cast<std::size_t>(0));
  CPLAN_CHECK_EQ(repository.list().size(), static_cast<std::size_t>(0));
}
