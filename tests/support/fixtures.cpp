#include "tests/support/fixtures.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>

#include "change_planner/model/evaluate.hpp"
#include "change_planner/plan/graph.hpp"

namespace cplan_test {

using cplan::CapacityUnits;
using cplan::ChangeCapability;
using cplan::CurrentStateSnapshot;
using cplan::DemandUnits;
using cplan::DesiredAction;
using cplan::DesiredConfig;
using cplan::DesiredDevice;
using cplan::DesiredLink;
using cplan::DesiredPolicy;
using cplan::DesiredRoute;
using cplan::DesiredWorkload;
using cplan::Device;
using cplan::DeviceId;
using cplan::DeviceSpec;
using cplan::EntityState;
using cplan::FailureDomainId;
using cplan::Link;
using cplan::LinkId;
using cplan::LinkSpec;
using cplan::MaintenanceWindow;
using cplan::PolicyObject;
using cplan::PolicyScope;
using cplan::PolicyTarget;
using cplan::Route;
using cplan::RouteBinding;
using cplan::RouteId;
using cplan::RouteSpec;
using cplan::RouteState;
using cplan::WindowId;
using cplan::Workload;
using cplan::WorkloadId;
using cplan::WorkloadRequirements;

std::uint64_t Rng::next_u64() {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

std::uint32_t Rng::next_bounded(std::uint32_t bound) {
  if (bound == 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(next_u64() % bound);
}

bool Rng::chance(std::uint32_t numerator, std::uint32_t denominator) {
  if (denominator == 0) {
    return false;
  }
  return next_bounded(denominator) < numerator;
}

cplan::CapabilitySet all_capabilities() {
  cplan::CapabilitySet capabilities;
  capabilities.add(ChangeCapability::device_add);
  capabilities.add(ChangeCapability::device_remove);
  capabilities.add(ChangeCapability::device_capacity);
  capabilities.add(ChangeCapability::device_state);
  capabilities.add(ChangeCapability::device_drain);
  capabilities.add(ChangeCapability::device_maintenance);
  capabilities.add(ChangeCapability::link_add);
  capabilities.add(ChangeCapability::link_remove);
  capabilities.add(ChangeCapability::link_capacity);
  capabilities.add(ChangeCapability::route_add);
  capabilities.add(ChangeCapability::route_remove);
  capabilities.add(ChangeCapability::route_state);
  capabilities.add(ChangeCapability::policy_bind);
  capabilities.add(ChangeCapability::config_set);
  capabilities.add(ChangeCapability::workload_binding);
  capabilities.add(ChangeCapability::workload_requirements);
  return capabilities;
}

namespace {

// Timestamps are strongly typed: arithmetic goes through an explicit helper
// rather than an implicit conversion.
cplan::TimestampNs shifted(cplan::TimestampNs base, std::int64_t delta) {
  return cplan::TimestampNs(base.value() + delta);
}

DeviceId device_id(const std::string& text) { return DeviceId::from_validated(text); }
LinkId link_id(const std::string& text) { return LinkId::from_validated(text); }
RouteId route_id(const std::string& text) { return RouteId::from_validated(text); }
WorkloadId workload_id(const std::string& text) { return WorkloadId::from_validated(text); }
FailureDomainId domain_id(const std::string& text) {
  return FailureDomainId::from_validated(text);
}

Device make_device(const std::string& id, const std::string& domain, const TopologyOptions& options,
                   cplan::TimestampNs window_begin, cplan::TimestampNs window_end) {
  Device device;
  device.id = device_id(id);
  device.spec.domain = domain_id(domain);
  device.spec.state = EntityState::present;
  device.spec.termination_capacity = options.device_capacity;
  device.spec.capabilities = all_capabilities();
  device.spec.max_concurrent_changes = 1;
  device.spec.profile_code = "generic-fabric-endpoint";
  if (options.with_maintenance_windows) {
    MaintenanceWindow window;
    window.id = WindowId::from_validated("mw-" + id);
    window.begin = window_begin;
    window.end = window_end;
    window.ticket_code = "generic-change-ticket";
    device.maintenance_windows.push_back(window);
  }
  if (options.with_exclusions) {
    cplan::ExclusionWindow exclusion;
    exclusion.id = WindowId::from_validated("ex-" + id);
    exclusion.begin = shifted(options.instant, -500);
    exclusion.end = shifted(options.instant, 500);
    exclusion.reason_code = "peak-traffic";
    device.exclusions.push_back(exclusion);
  }
  return device;
}

Link make_link(const std::string& id, const std::string& a, const std::string& b,
               const TopologyOptions& options, std::uint32_t latency_class = 1) {
  Link link;
  link.id = link_id(id);
  link.spec.endpoint_a = device_id(a);
  link.spec.endpoint_b = device_id(b);
  link.spec.capacity = options.link_capacity;
  link.spec.latency_class = latency_class;
  link.state = EntityState::present;
  return link;
}

Route make_route(const std::string& id, const std::string& source, const std::string& sink,
                 const std::vector<std::string>& path, RouteState state = RouteState::active) {
  Route route;
  route.id = route_id(id);
  route.spec.source = device_id(source);
  route.spec.sink = device_id(sink);
  for (const std::string& hop : path) {
    route.spec.path.push_back(link_id(hop));
  }
  route.state = state;
  return route;
}

Workload make_workload(const std::string& id, const std::vector<std::pair<std::string, bool>>& bindings,
                       DemandUnits demand, std::uint32_t min_paths, std::uint32_t min_domains) {
  Workload workload;
  workload.id = workload_id(id);
  for (const auto& entry : bindings) {
    RouteBinding binding;
    binding.route = route_id(entry.first);
    binding.active = entry.second;
    binding.demand = demand;
    workload.bindings.push_back(binding);
  }
  workload.requirements.require_connectivity = true;
  workload.requirements.min_link_disjoint_paths = min_paths;
  workload.requirements.min_disjoint_domains = min_domains;
  workload.contract_code = "generic-connectivity-contract";
  return workload;
}

}  // namespace

cplan::TopologyEvidence evidence_for(const CurrentStateSnapshot& state) {
  cplan::TopologyEvidence evidence;
  evidence.topology_generation = state.topology_generation;
  evidence.boot_incarnation = state.boot_incarnation;
  evidence.capability_version = cplan::CapabilityVersion(7);
  evidence.observed_at = state.observed_at;
  for (const Device& device : state.devices) {
    cplan::CapabilityEvidence entry;
    entry.device = device.id;
    entry.capabilities = device.spec.capabilities;
    entry.capability_version = evidence.capability_version;
    evidence.devices.push_back(std::move(entry));
  }
  evidence.canonicalize();
  return evidence;
}

void refresh_request(Scenario& scenario, const cplan::Objective& objective,
                     const cplan::PlanningLimits& limits) {
  scenario.state.canonicalize();
  scenario.target.canonicalize();
  scenario.constraints.canonicalize();
  scenario.evidence.canonicalize();

  scenario.target.based_on_generation = scenario.state.topology_generation;
  scenario.target.authority = scenario.state.authority;
  scenario.target.authority_generation = scenario.state.authority_generation;
  scenario.target.based_on_epoch = scenario.state.epoch;
  // The governing constraints are issued under the same authority generation as
  // the snapshot they will be applied to.
  scenario.constraints.authority_generation = scenario.state.authority_generation;

  scenario.request = cplan::PlanRequest{};
  scenario.request.plan_id = cplan::PlanId::from_validated("plan-fixture");
  scenario.request.generation = cplan::PlanGeneration(1);
  scenario.request.planning_instant = scenario.state.observed_at;
  scenario.request.state = scenario.state;
  scenario.request.target = scenario.target;
  scenario.request.constraints = scenario.constraints;
  scenario.request.evidence = scenario.evidence;
  scenario.request.objective = objective;
  scenario.request.limits = limits;
  scenario.request.label = "fixture";
}

cplan::PlanResult run_plan(const Scenario& scenario) {
  return cplan::generate_plan(scenario.request, cplan::StopToken{});
}

namespace {

std::atomic<std::uint64_t> g_scratch_counter{0};

}  // namespace

std::string make_scratch_directory(const std::string& tag) {
  const std::uint64_t sequence = g_scratch_counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path base = std::filesystem::temp_directory_path();
  base /= "cplan-tests-" + tag + "-" + std::to_string(sequence);
  std::error_code error;
  std::filesystem::remove_all(base, error);
  std::filesystem::create_directories(base, error);
  return base.string();
}

void remove_scratch_directory(const std::string& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

std::vector<std::byte> read_file_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::byte> bytes;
  if (!stream.is_open()) {
    return bytes;
  }
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    const std::streamsize count = stream.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[i])));
    }
  }
  return bytes;
}

void write_file_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::size_t hard_violation_count(const std::vector<cplan::Violation>& violations) {
  std::size_t count = 0;
  for (const cplan::Violation& violation : violations) {
    if (violation.severity == cplan::Severity::violation) {
      ++count;
    }
  }
  return count;
}

namespace {

void finish_scenario(Scenario& scenario) {
  scenario.state.canonicalize();
  scenario.evidence = evidence_for(scenario.state);
  scenario.target = cplan::target_from_state(scenario.state, cplan::TargetRevision(7), "fixture");
  refresh_request(scenario);
}

}  // namespace

Scenario make_ladder(const TopologyOptions& options) {
  Scenario scenario;
  scenario.constraints = cplan::default_constraints(options.headroom, 1, 1);
  scenario.state.topology_generation = cplan::TopologyGeneration(11);
  scenario.state.authority = cplan::AuthorityId::from_validated("fabric-authority");
  scenario.state.authority_generation = cplan::AuthorityGeneration(3);
  scenario.state.boot_incarnation = cplan::BootIncarnation(5);
  scenario.state.epoch = cplan::Epoch(2);
  scenario.state.observed_at = options.instant;

  const cplan::TimestampNs window_begin = shifted(options.instant, -1000);
  const cplan::TimestampNs window_end = shifted(options.instant, 100000);
  scenario.state.devices.push_back(make_device("ep-a", "dom-a", options, window_begin, window_end));
  scenario.state.devices.push_back(make_device("ep-b", "dom-d", options, window_begin, window_end));
  scenario.state.devices.push_back(make_device("mid-1", "dom-b", options, window_begin, window_end));
  scenario.state.devices.push_back(make_device("mid-2", "dom-c", options, window_begin, window_end));

  scenario.state.links.push_back(make_link("l-a1", "ep-a", "mid-1", options));
  scenario.state.links.push_back(make_link("l-1b", "mid-1", "ep-b", options));
  scenario.state.links.push_back(make_link("l-a2", "ep-a", "mid-2", options));
  scenario.state.links.push_back(make_link("l-2b", "mid-2", "ep-b", options));

  scenario.state.routes.push_back(make_route("r-1", "ep-a", "ep-b", {"l-a1", "l-1b"}));
  scenario.state.routes.push_back(make_route("r-2", "ep-a", "ep-b", {"l-a2", "l-2b"}));

  scenario.state.workloads.push_back(
      make_workload("w-1", {{"r-1", true}, {"r-2", true}}, options.demand, 2, 0));

  finish_scenario(scenario);
  return scenario;
}

Scenario make_dual_homed(const TopologyOptions& options) {
  Scenario scenario;
  scenario.constraints = cplan::default_constraints(options.headroom, 1, 1);
  scenario.state.topology_generation = cplan::TopologyGeneration(21);
  scenario.state.authority = cplan::AuthorityId::from_validated("fabric-authority");
  scenario.state.authority_generation = cplan::AuthorityGeneration(4);
  scenario.state.boot_incarnation = cplan::BootIncarnation(6);
  scenario.state.epoch = cplan::Epoch(1);
  scenario.state.observed_at = options.instant;

  const cplan::TimestampNs window_begin = shifted(options.instant, -1000);
  const cplan::TimestampNs window_end = shifted(options.instant, 100000);
  scenario.state.devices.push_back(make_device("ep-a1", "dom-a1", options, window_begin, window_end));
  scenario.state.devices.push_back(make_device("ep-b1", "dom-b1", options, window_begin, window_end));
  scenario.state.devices.push_back(make_device("mid-1", "dom-c1", options, window_begin, window_end));
  scenario.state.devices.push_back(make_device("ep-a2", "dom-a2", options, window_begin, window_end));
  scenario.state.devices.push_back(make_device("ep-b2", "dom-b2", options, window_begin, window_end));
  scenario.state.devices.push_back(make_device("mid-2", "dom-c2", options, window_begin, window_end));

  scenario.state.links.push_back(make_link("l-1a", "ep-a1", "mid-1", options));
  scenario.state.links.push_back(make_link("l-1b", "mid-1", "ep-b1", options));
  scenario.state.links.push_back(make_link("l-2a", "ep-a2", "mid-2", options));
  scenario.state.links.push_back(make_link("l-2b", "mid-2", "ep-b2", options));

  scenario.state.routes.push_back(make_route("r-1", "ep-a1", "ep-b1", {"l-1a", "l-1b"}));
  scenario.state.routes.push_back(make_route("r-2", "ep-a2", "ep-b2", {"l-2a", "l-2b"}));

  scenario.state.workloads.push_back(
      make_workload("w-1", {{"r-1", true}, {"r-2", true}}, options.demand, 2, 1));

  finish_scenario(scenario);
  return scenario;
}

Scenario make_random(const TopologyOptions& options, Rng& rng, std::uint32_t device_count,
                     std::uint32_t domain_count, std::uint32_t extra_links) {
  Scenario scenario;
  scenario.constraints = cplan::default_constraints(options.headroom, 1, 1);
  scenario.state.topology_generation = cplan::TopologyGeneration(100 + rng.next_bounded(1000));
  scenario.state.authority = cplan::AuthorityId::from_validated("fabric-authority");
  scenario.state.authority_generation = cplan::AuthorityGeneration(1 + rng.next_bounded(50));
  scenario.state.boot_incarnation = cplan::BootIncarnation(1 + rng.next_bounded(50));
  scenario.state.epoch = cplan::Epoch(1 + rng.next_bounded(10));
  scenario.state.observed_at = options.instant;

  const cplan::TimestampNs window_begin = shifted(options.instant, -1000);
  const cplan::TimestampNs window_end = shifted(options.instant, 100000);

  const std::uint32_t devices = std::max<std::uint32_t>(2, device_count);
  const std::uint32_t domains = std::max<std::uint32_t>(1, domain_count);
  for (std::uint32_t i = 0; i < devices; ++i) {
    const std::string id = "d" + std::to_string(i);
    const std::string domain = "dom-" + std::to_string(i % domains);
    scenario.state.devices.push_back(make_device(id, domain, options, window_begin, window_end));
  }

  // Spanning chain guarantees connectivity.
  std::uint32_t link_counter = 0;
  for (std::uint32_t i = 0; i + 1 < devices; ++i) {
    const std::string id = "l" + std::to_string(link_counter++);
    scenario.state.links.push_back(
        make_link(id, "d" + std::to_string(i), "d" + std::to_string(i + 1), options));
  }
  for (std::uint32_t i = 0; i < extra_links; ++i) {
    const std::uint32_t a = rng.next_bounded(devices);
    std::uint32_t b = rng.next_bounded(devices);
    if (a == b) {
      b = (b + 1) % devices;
    }
    const std::string id = "l" + std::to_string(link_counter++);
    bool duplicate = false;
    for (const Link& link : scenario.state.links) {
      if ((link.spec.endpoint_a.str() == "d" + std::to_string(a) &&
           link.spec.endpoint_b.str() == "d" + std::to_string(b)) ||
          (link.spec.endpoint_a.str() == "d" + std::to_string(b) &&
           link.spec.endpoint_b.str() == "d" + std::to_string(a))) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    scenario.state.links.push_back(
        make_link(id, "d" + std::to_string(a), "d" + std::to_string(b), options, 1 + (i % 3)));
  }

  // Routes: chain walks from device 0 to device n-1 following consecutive links.
  std::vector<LinkId> chain;
  for (const Link& link : scenario.state.links) {
    if (link.id.str().rfind("l", 0) == 0) {
      chain.push_back(link.id);
    }
  }
  if (!chain.empty()) {
    Route route;
    route.id = route_id("r-chain");
    route.spec.source = device_id("d0");
    route.spec.sink = device_id("d" + std::to_string(devices - 1));
    route.spec.path.assign(chain.begin(), chain.begin() + (devices - 1));
    route.state = RouteState::active;
    scenario.state.routes.push_back(route);
    scenario.state.workloads.push_back(
        make_workload("w-chain", {{"r-chain", true}}, options.demand, 1, 0));
  }

  // An unused device gives the target something removable without breaking a
  // contract.
  finish_scenario(scenario);
  return scenario;
}

cplan::Result<CurrentStateSnapshot> apply_plan(const cplan::Plan& plan,
                                               const cplan::PlanRequest& request) {
  const cplan::EvaluationContext evaluation{request.planning_instant, &request.constraints,
                                            &request.evidence};
  CurrentStateSnapshot state = request.state;
  for (const cplan::Stage& stage : plan.stages) {
    std::vector<cplan::StepId> ids = stage.steps;
    std::sort(ids.begin(), ids.end());
    for (const cplan::StepId& id : ids) {
      const cplan::Step* step = plan.find_step(id);
      if (step == nullptr) {
        return cplan::Status::error(cplan::ErrorCode::not_found, "step missing from plan");
      }
      auto applied = cplan::apply_operation(state, step->operation, evaluation);
      if (!applied.ok()) {
        return applied.status();
      }
      state = applied.value().state;
    }
  }
  return state;
}

}  // namespace cplan_test
