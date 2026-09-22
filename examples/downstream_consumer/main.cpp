// examples/downstream_consumer/main.cpp
//
// Minimal downstream consumer of the installed Change Planner package.
//
// This translation unit only ever includes the installed public headers
// (<change_planner/...>); it reaches nothing from the project's src/ or tests/
// directories, and it links the exported ChangePlanner::change_planner target.
// It builds a trivial scenario in code, asks the planner for a plan and reports
// whether a plan or a refusal was produced.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <change_planner/model/constraints.hpp>
#include <change_planner/model/evidence.hpp>
#include <change_planner/model/target.hpp>
#include <change_planner/plan/generate.hpp>

namespace {

// Logical planning instant: the planner never reads the wall clock.
constexpr std::int64_t kPlanningInstantNs = 1700000000000000000;

cplan::PlanRequest build_request() {
  cplan::PlanRequest request;
  request.plan_id = cplan::PlanId::from_validated("downstream-plan");
  request.generation = cplan::PlanGeneration(1);
  request.planning_instant = cplan::TimestampNs(kPlanningInstantNs);
  request.label = "downstream-consumer";
  request.objective = cplan::Objective::default_objective();
  request.constraints = cplan::default_constraints(cplan::Permille(0), 1, 1);

  cplan::CurrentStateSnapshot& state = request.state;
  state.topology_generation = cplan::TopologyGeneration(1);
  state.authority = cplan::AuthorityId::from_validated("downstream-authority");
  state.authority_generation = cplan::AuthorityGeneration(1);
  state.boot_incarnation = cplan::BootIncarnation(1);
  state.epoch = cplan::Epoch(1);
  state.observed_at = request.planning_instant;

  cplan::CapabilitySet capabilities;
  capabilities.add(cplan::ChangeCapability::link_capacity);
  capabilities.add(cplan::ChangeCapability::config_set);

  for (const char* name : {"node-a", "node-b"}) {
    cplan::Device device;
    device.id = cplan::DeviceId::from_validated(name);
    device.spec.domain = cplan::FailureDomainId::from_validated(std::string("dom-") + name);
    device.spec.state = cplan::EntityState::present;
    device.spec.termination_capacity = cplan::CapacityUnits(10000);
    device.spec.capabilities = capabilities;
    device.spec.max_concurrent_changes = 1;
    device.spec.profile_code = "generic-fabric-endpoint";
    state.devices.push_back(std::move(device));
  }

  cplan::Link link;
  link.id = cplan::LinkId::from_validated("link-ab");
  link.spec.endpoint_a = cplan::DeviceId::from_validated("node-a");
  link.spec.endpoint_b = cplan::DeviceId::from_validated("node-b");
  link.spec.capacity = cplan::CapacityUnits(1000);
  link.state = cplan::EntityState::present;
  state.links.push_back(std::move(link));
  state.canonicalize();

  cplan::TopologyEvidence& evidence = request.evidence;
  evidence.topology_generation = state.topology_generation;
  evidence.boot_incarnation = state.boot_incarnation;
  evidence.capability_version = cplan::CapabilityVersion(1);
  evidence.observed_at = state.observed_at;
  for (const cplan::Device& device : state.devices) {
    cplan::CapabilityEvidence entry;
    entry.device = device.id;
    entry.capabilities = device.spec.capabilities;
    entry.capability_version = evidence.capability_version;
    evidence.devices.push_back(std::move(entry));
  }
  evidence.canonicalize();

  request.target = cplan::target_from_state(state, cplan::TargetRevision(1), "downstream");
  for (cplan::DesiredLink& desired : request.target.links) {
    desired.spec.capacity = cplan::CapacityUnits(desired.spec.capacity.value() + 500);
  }
  cplan::DesiredConfig config;
  config.action = cplan::DesiredAction::ensure;
  config.device = cplan::DeviceId::from_validated("node-b");
  config.key = cplan::ConfigKey::from_validated("mtu");
  config.value_code = "9000";
  request.target.configs.push_back(std::move(config));
  request.target.canonicalize();

  request.target.based_on_generation = state.topology_generation;
  request.target.authority = state.authority;
  request.target.authority_generation = state.authority_generation;
  request.target.based_on_epoch = state.epoch;
  request.constraints.authority_generation = state.authority_generation;
  request.constraints.canonicalize();
  return request;
}

}  // namespace

int main() {
  const cplan::PlanRequest request = build_request();
  const cplan::PlanResult result = cplan::generate_plan(request);

  if (!result.ok()) {
    std::printf("refusal: %s\n", cplan::to_string(result.refusal().code));
    std::printf("%s\n", result.refusal().explain().c_str());
    return 1;
  }

  const cplan::Plan& plan = result.plan();
  std::printf("plan: id=%s generation=%llu stages=%zu steps=%zu\n", plan.id.str().c_str(),
              static_cast<unsigned long long>(plan.generation.value()), plan.stages.size(),
              plan.steps.size());
  return 0;
}
