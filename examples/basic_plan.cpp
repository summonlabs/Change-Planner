// examples/basic_plan.cpp
//
// End-to-end walkthrough of the Change Planner public API: describe a tiny
// three-device topology, declare the target the operator wants, ask the planner
// for an ordered transition (generate_plan), independently re-verify it
// (verify_plan), then print the plan, its steps, the objective vector and the
// verification summary -- or, when no safe ordering exists, the refusal.
//
// No vendor-specific assumptions: every identity, capability and constraint is
// generic, and the planning instant is a logical timestamp supplied by the
// caller rather than the wall clock.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "change_planner/model/constraints.hpp"
#include "change_planner/model/evidence.hpp"
#include "change_planner/model/target.hpp"
#include "change_planner/plan/generate.hpp"
#include "change_planner/plan/verify.hpp"

namespace {

// The logical instant the plan is authored for. The planner never reads a clock.
constexpr std::int64_t kPlanningInstantNs = 1700000000000000000;

cplan::CapabilitySet generic_capabilities() {
  cplan::CapabilitySet capabilities;
  capabilities.add(cplan::ChangeCapability::device_state);
  capabilities.add(cplan::ChangeCapability::link_capacity);
  capabilities.add(cplan::ChangeCapability::route_state);
  capabilities.add(cplan::ChangeCapability::config_set);
  capabilities.add(cplan::ChangeCapability::workload_binding);
  return capabilities;
}

cplan::Device make_device(const std::string& id, const std::string& domain) {
  cplan::Device device;
  device.id = cplan::DeviceId::from_validated(id);
  device.spec.domain = cplan::FailureDomainId::from_validated(domain);
  device.spec.state = cplan::EntityState::present;
  device.spec.termination_capacity = cplan::CapacityUnits(20000);
  device.spec.capabilities = generic_capabilities();
  device.spec.max_concurrent_changes = 1;
  device.spec.profile_code = "generic-fabric-endpoint";
  return device;
}

cplan::Link make_link(const std::string& id, const std::string& a, const std::string& b) {
  cplan::Link link;
  link.id = cplan::LinkId::from_validated(id);
  link.spec.endpoint_a = cplan::DeviceId::from_validated(a);
  link.spec.endpoint_b = cplan::DeviceId::from_validated(b);
  link.spec.capacity = cplan::CapacityUnits(10000);
  link.spec.latency_class = 1;
  link.state = cplan::EntityState::present;
  return link;
}

// Builds the complete planning request: current state, target intent, safety
// constraints, observed capability evidence and planning limits.
cplan::Result<cplan::PlanRequest> build_request() {
  cplan::PlanRequest request;
  request.plan_id = cplan::PlanId::from_validated("basic-plan");
  request.generation = cplan::PlanGeneration(1);
  request.planning_instant = cplan::TimestampNs(kPlanningInstantNs);
  request.label = "basic-example";
  request.objective = cplan::Objective::default_objective();
  request.constraints = cplan::default_constraints(cplan::Permille(0), 1, 1);

  cplan::CurrentStateSnapshot& state = request.state;
  state.topology_generation = cplan::TopologyGeneration(1);
  state.authority = cplan::AuthorityId::from_validated("example-authority");
  state.authority_generation = cplan::AuthorityGeneration(1);
  state.boot_incarnation = cplan::BootIncarnation(1);
  state.epoch = cplan::Epoch(1);
  state.observed_at = request.planning_instant;

  state.devices.push_back(make_device("edge-a", "dom-a"));
  state.devices.push_back(make_device("core-1", "dom-core"));
  state.devices.push_back(make_device("edge-b", "dom-b"));
  state.links.push_back(make_link("link-a", "edge-a", "core-1"));
  state.links.push_back(make_link("link-b", "core-1", "edge-b"));

  cplan::Route route;
  route.id = cplan::RouteId::from_validated("route-1");
  route.spec.source = cplan::DeviceId::from_validated("edge-a");
  route.spec.sink = cplan::DeviceId::from_validated("edge-b");
  route.spec.path.push_back(cplan::LinkId::from_validated("link-a"));
  route.spec.path.push_back(cplan::LinkId::from_validated("link-b"));
  route.state = cplan::RouteState::active;
  state.routes.push_back(std::move(route));

  cplan::Workload workload;
  workload.id = cplan::WorkloadId::from_validated("workload-1");
  cplan::RouteBinding binding;
  binding.route = cplan::RouteId::from_validated("route-1");
  binding.active = true;
  binding.demand = cplan::DemandUnits(250);
  workload.bindings.push_back(std::move(binding));
  workload.requirements.require_connectivity = true;
  workload.requirements.min_link_disjoint_paths = 1;
  workload.contract_code = "generic-connectivity-contract";
  state.workloads.push_back(std::move(workload));

  state.canonicalize();

  // Capability evidence observed for exactly this snapshot.
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

  // Target intent: bump both links and set one configuration entry.
  request.target = cplan::target_from_state(state, cplan::TargetRevision(1), "basic-example");
  for (cplan::DesiredLink& link : request.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 2000);
  }
  cplan::DesiredConfig config;
  config.action = cplan::DesiredAction::ensure;
  config.device = cplan::DeviceId::from_validated("core-1");
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

  CPLAN_TRY(request.validate());
  return request;
}

}  // namespace

int main() {
  const cplan::Result<cplan::PlanRequest> built = build_request();
  if (!built.ok()) {
    std::fprintf(stderr, "request rejected: %s\n", built.status().to_string().c_str());
    return 2;
  }
  const cplan::PlanRequest& request = built.value();

  std::printf("scenario: devices=%zu links=%zu routes=%zu workloads=%zu\n",
              request.state.devices.size(), request.state.links.size(),
              request.state.routes.size(), request.state.workloads.size());

  const cplan::PlanResult result = cplan::generate_plan(request);
  if (!result.ok()) {
    std::printf("refusal: %s\n", cplan::to_string(result.refusal().code));
    std::printf("%s\n", result.refusal().explain().c_str());
    return 1;
  }

  const cplan::Plan& plan = result.plan();
  std::printf("plan: id=%s generation=%llu label=%s\n", plan.id.str().c_str(),
              static_cast<unsigned long long>(plan.generation.value()), plan.label.c_str());
  std::printf("objective: stages=%u risk=%u churn=%llu maintenance-ns=%llu tie-break=%s\n",
              plan.objective.stages.value(), plan.objective.risk_exposure.value(),
              static_cast<unsigned long long>(plan.objective.churn),
              static_cast<unsigned long long>(plan.objective.maintenance_duration.value()),
              plan.objective.tie_break.short_hex().c_str());
  std::printf("stages: %zu (steps: %zu)\n", plan.stages.size(), plan.steps.size());

  for (const cplan::Step& step : plan.steps) {
    std::printf("  stage %u step %s: %s\n", step.stage.value(), step.id.str().c_str(),
                step.operation.to_string().c_str());
  }

  const cplan::VerificationReport report = cplan::verify_plan(plan, request, true);
  std::printf("verification: ok=%s steps-applied=%llu permutations-verified=%llu "
              "invariants-checked=%llu trace=%s\n",
              report.ok ? "true" : "false",
              static_cast<unsigned long long>(report.steps_applied),
              static_cast<unsigned long long>(report.permutations_verified),
              static_cast<unsigned long long>(report.invariant_evaluations),
              report.trace_digest.short_hex().c_str());

  if (!report.ok) {
    std::fprintf(stderr, "verification failed: %s\n", report.explain().c_str());
    return 2;
  }
  return 0;
}
