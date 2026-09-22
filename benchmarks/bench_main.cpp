// benchmarks/bench_main.cpp
//
// Throughput benchmark for the Change Planner public API.
//
// Every measured sample is a *completed* planning operation: one full
// generate_plan() run followed by an exhaustive verify_plan() over the plan it
// produced. Submission latency is never measured -- the steady-clock interval
// wraps finished work only.
//
// The benchmark never reads a clock for planning decisions: the planning instant
// is the logical TimestampNs carried by the snapshot, and every scenario is
// derived from the seed below. The only clock read is the interval used to
// report throughput.
//
// Usage: cplan_bench [iterations] [seed]
//   iterations  measured samples per topology size (default 5)
//   seed        deterministic scenario seed (default 20240917)
//
// The default sample count is deliberately modest: the measured interval covers
// exhaustive interleaving verification of a 128-step plan over a 64-device
// snapshot, which costs tens of seconds per sample on a developer machine. Pass
// an explicit count to trade runtime for a tighter throughput estimate.

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "change_planner/model/constraints.hpp"
#include "change_planner/model/evidence.hpp"
#include "change_planner/model/target.hpp"
#include "change_planner/plan/generate.hpp"
#include "change_planner/plan/verify.hpp"

namespace {

// ---------------------------------------------------------------------------
// Deterministic pseudo-randomness. Every scenario is reproducible from its seed;
// nothing here consults the environment, the clock or the platform.
// ---------------------------------------------------------------------------
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next_u64() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::uint32_t next_bounded(std::uint32_t bound) noexcept {
    if (bound == 0) {
      return 0;
    }
    return static_cast<std::uint32_t>(next_u64() % bound);
  }

 private:
  std::uint64_t state_;
};

// The logical planning instant shared by every scenario. Fixed, so planning
// behaviour never depends on when the benchmark is executed.
constexpr std::int64_t kPlanningInstantNs = 1700000000000000000;

struct BenchOptions {
  cplan::CapacityUnits link_capacity{10000};
  cplan::CapacityUnits device_capacity{20000};
  cplan::DemandUnits demand{100};
  std::uint32_t min_link_disjoint_paths{2};
};

cplan::CapabilitySet all_capabilities() {
  cplan::CapabilitySet capabilities;
  capabilities.add(cplan::ChangeCapability::device_add);
  capabilities.add(cplan::ChangeCapability::device_remove);
  capabilities.add(cplan::ChangeCapability::device_capacity);
  capabilities.add(cplan::ChangeCapability::device_state);
  capabilities.add(cplan::ChangeCapability::device_drain);
  capabilities.add(cplan::ChangeCapability::device_maintenance);
  capabilities.add(cplan::ChangeCapability::link_add);
  capabilities.add(cplan::ChangeCapability::link_remove);
  capabilities.add(cplan::ChangeCapability::link_capacity);
  capabilities.add(cplan::ChangeCapability::route_add);
  capabilities.add(cplan::ChangeCapability::route_remove);
  capabilities.add(cplan::ChangeCapability::route_state);
  capabilities.add(cplan::ChangeCapability::policy_bind);
  capabilities.add(cplan::ChangeCapability::config_set);
  capabilities.add(cplan::ChangeCapability::workload_binding);
  capabilities.add(cplan::ChangeCapability::workload_requirements);
  return capabilities;
}

cplan::TimestampNs shifted(cplan::TimestampNs base, std::int64_t delta) {
  return cplan::TimestampNs(base.value() + delta);
}

cplan::Device make_device(const std::string& id, const std::string& domain,
                          const BenchOptions& options, cplan::TimestampNs window_begin,
                          cplan::TimestampNs window_end) {
  cplan::Device device;
  device.id = cplan::DeviceId::from_validated(id);
  device.spec.domain = cplan::FailureDomainId::from_validated(domain);
  device.spec.state = cplan::EntityState::present;
  device.spec.termination_capacity = options.device_capacity;
  device.spec.termination_reserved = cplan::CapacityUnits(0);
  device.spec.capabilities = all_capabilities();
  device.spec.max_concurrent_changes = 1;
  device.spec.profile_code = "generic-fabric-endpoint";

  cplan::MaintenanceWindow window;
  window.id = cplan::WindowId::from_validated("mw-" + id);
  window.begin = window_begin;
  window.end = window_end;
  window.ticket_code = "generic-change-ticket";
  device.maintenance_windows.push_back(std::move(window));
  return device;
}

cplan::Link make_link(const std::string& id, const std::string& a, const std::string& b,
                      const BenchOptions& options) {
  cplan::Link link;
  link.id = cplan::LinkId::from_validated(id);
  link.spec.endpoint_a = cplan::DeviceId::from_validated(a);
  link.spec.endpoint_b = cplan::DeviceId::from_validated(b);
  link.spec.capacity = options.link_capacity;
  link.spec.latency_class = 1;
  link.state = cplan::EntityState::present;
  return link;
}

cplan::Route make_route(const std::string& id, const std::string& source, const std::string& sink,
                        const std::vector<cplan::LinkId>& path) {
  cplan::Route route;
  route.id = cplan::RouteId::from_validated(id);
  route.spec.source = cplan::DeviceId::from_validated(source);
  route.spec.sink = cplan::DeviceId::from_validated(sink);
  route.spec.path = path;
  route.state = cplan::RouteState::active;
  return route;
}

cplan::Workload make_workload(const std::string& id, const std::vector<std::string>& routes,
                              cplan::DemandUnits demand, std::uint32_t min_paths) {
  cplan::Workload workload;
  workload.id = cplan::WorkloadId::from_validated(id);
  for (const std::string& route : routes) {
    cplan::RouteBinding binding;
    binding.route = cplan::RouteId::from_validated(route);
    binding.active = true;
    binding.demand = demand;
    workload.bindings.push_back(std::move(binding));
  }
  workload.requirements.require_connectivity = true;
  workload.requirements.min_link_disjoint_paths = min_paths;
  workload.requirements.min_disjoint_domains = 0;
  workload.contract_code = "generic-connectivity-contract";
  return workload;
}

struct Scenario {
  cplan::CurrentStateSnapshot state;
  cplan::TargetIntent target;
  cplan::SafetyConstraints constraints;
  cplan::TopologyEvidence evidence;
  cplan::PlanRequest request;
};

// A dual-path chain: two link-disjoint chains of equal length between one shared
// pair of endpoints. Each intermediate device owns its failure domain, so the two
// chains are domain-disjoint as well. The single workload requires two
// link-disjoint paths, which only the complete pair can satisfy.
Scenario make_dual_path_chain(std::uint32_t device_count, SplitMix64& rng,
                              const BenchOptions& options) {
  const std::uint32_t devices = device_count < 4 ? 4 : device_count;
  const std::uint32_t per_path = (devices - 2) / 2;

  Scenario scenario;
  scenario.constraints = cplan::default_constraints(cplan::Permille(0), 1, 1);
  scenario.state.topology_generation = cplan::TopologyGeneration(11);
  scenario.state.authority = cplan::AuthorityId::from_validated("bench-authority");
  scenario.state.authority_generation = cplan::AuthorityGeneration(3);
  scenario.state.boot_incarnation = cplan::BootIncarnation(5);
  scenario.state.epoch = cplan::Epoch(2);
  scenario.state.observed_at = cplan::TimestampNs(kPlanningInstantNs);

  const cplan::TimestampNs window_begin = shifted(scenario.state.observed_at, -1000);
  const cplan::TimestampNs window_end = shifted(scenario.state.observed_at, 1000000);

  scenario.state.devices.push_back(
      make_device("ep-src", "dom-src", options, window_begin, window_end));
  scenario.state.devices.push_back(
      make_device("ep-dst", "dom-dst", options, window_begin, window_end));

  std::vector<cplan::LinkId> path_a;
  std::vector<cplan::LinkId> path_b;

  for (std::uint32_t branch = 0; branch < 2; ++branch) {
    const std::string letter = branch == 0 ? "a" : "b";
    std::vector<cplan::LinkId>& path = branch == 0 ? path_a : path_b;
    std::string previous = "ep-src";
    for (std::uint32_t hop = 0; hop < per_path; ++hop) {
      const std::string device_id = "mid-" + letter + std::to_string(hop);
      scenario.state.devices.push_back(
          make_device(device_id, "dom-" + device_id, options, window_begin, window_end));
      const std::string link_id = "l-" + letter + std::to_string(hop);
      scenario.state.links.push_back(make_link(link_id, previous, device_id, options));
      path.push_back(cplan::LinkId::from_validated(link_id));
      previous = device_id;
    }
    const std::string tail_id = "l-" + letter + "-tail";
    scenario.state.links.push_back(make_link(tail_id, previous, "ep-dst", options));
    path.push_back(cplan::LinkId::from_validated(tail_id));
  }

  scenario.state.routes.push_back(make_route("r-a", "ep-src", "ep-dst", path_a));
  scenario.state.routes.push_back(make_route("r-b", "ep-src", "ep-dst", path_b));
  scenario.state.workloads.push_back(
      make_workload("w-1", {"r-a", "r-b"}, options.demand, options.min_link_disjoint_paths));
  scenario.state.canonicalize();

  // Capability evidence observed for exactly this snapshot.
  scenario.evidence.topology_generation = scenario.state.topology_generation;
  scenario.evidence.boot_incarnation = scenario.state.boot_incarnation;
  scenario.evidence.capability_version = cplan::CapabilityVersion(7);
  scenario.evidence.observed_at = scenario.state.observed_at;
  for (const cplan::Device& device : scenario.state.devices) {
    cplan::CapabilityEvidence entry;
    entry.device = device.id;
    entry.capabilities = device.spec.capabilities;
    entry.capability_version = scenario.evidence.capability_version;
    scenario.evidence.devices.push_back(std::move(entry));
  }
  scenario.evidence.canonicalize();

  // Deterministic target change: a capacity bump on every link plus one config
  // entry per device.
  scenario.target = cplan::target_from_state(scenario.state, cplan::TargetRevision(1), "bench");
  for (cplan::DesiredLink& link : scenario.target.links) {
    const std::uint64_t bump = 1000u + rng.next_bounded(4000);
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + bump);
  }
  for (const cplan::Device& device : scenario.state.devices) {
    cplan::DesiredConfig config;
    config.action = cplan::DesiredAction::ensure;
    config.device = device.id;
    config.key = cplan::ConfigKey::from_validated("mtu");
    config.value_code = std::to_string(9000u + rng.next_bounded(8) * 4u);
    scenario.target.configs.push_back(std::move(config));
  }
  scenario.target.canonicalize();

  scenario.target.based_on_generation = scenario.state.topology_generation;
  scenario.target.authority = scenario.state.authority;
  scenario.target.authority_generation = scenario.state.authority_generation;
  scenario.target.based_on_epoch = scenario.state.epoch;
  scenario.constraints.authority_generation = scenario.state.authority_generation;
  scenario.constraints.canonicalize();

  scenario.request.plan_id = cplan::PlanId::from_validated("bench-plan");
  scenario.request.generation = cplan::PlanGeneration(1);
  scenario.request.planning_instant = scenario.state.observed_at;
  scenario.request.state = scenario.state;
  scenario.request.target = scenario.target;
  scenario.request.constraints = scenario.constraints;
  scenario.request.evidence = scenario.evidence;
  scenario.request.objective = cplan::Objective::default_objective();
  scenario.request.limits = cplan::PlanningLimits{};
  scenario.request.label = "bench";
  return scenario;
}

struct Row {
  std::uint32_t devices{0};
  std::uint64_t steps{0};
  std::uint64_t stages{0};
  double plans_per_second{0.0};
  double steps_per_second{0.0};
  double permutations_per_second{0.0};
};

struct SizeResult {
  bool ok{false};
  Row row;
  std::string error;
};

// Runs one measured phase for a single topology size. Every sample is a complete
// generate + exhaustively verify operation.
SizeResult measure_size(std::uint32_t device_count, std::uint64_t iterations, std::uint64_t seed) {
  SizeResult outcome;

  const std::uint64_t mixed =
      seed ^ (static_cast<std::uint64_t>(device_count) * 0x100000001B3ull);
  SplitMix64 rng(mixed);
  const Scenario scenario = make_dual_path_chain(device_count, rng, BenchOptions{});

  // Warm-up sample, deliberately outside the measured interval: first-touch page
  // faults and allocator growth are not part of reported throughput.
  {
    const cplan::PlanResult warm = cplan::generate_plan(scenario.request);
    if (!warm.ok()) {
      outcome.error = "warm-up generate_plan refused: " + warm.refusal().explain();
      return outcome;
    }
    const cplan::VerificationReport report = cplan::verify_plan(warm.plan(), scenario.request, true);
    if (!report.ok) {
      outcome.error = "warm-up verify_plan failed: " + report.explain();
      return outcome;
    }
  }

  std::uint64_t total_steps = 0;
  std::uint64_t total_permutations = 0;
  cplan::Digest reference_digest;
  bool have_reference = false;

  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  for (std::uint64_t sample = 0; sample < iterations; ++sample) {
    const cplan::PlanResult result = cplan::generate_plan(scenario.request);
    if (!result.ok()) {
      outcome.error = "generate_plan refused: " + result.refusal().explain();
      return outcome;
    }
    const cplan::Plan& plan = result.plan();
    const cplan::VerificationReport report = cplan::verify_plan(plan, scenario.request, true);
    if (!report.ok) {
      outcome.error = "verify_plan failed: " + report.explain();
      return outcome;
    }

    const cplan::Digest digest = plan.content_digest();
    if (!have_reference) {
      reference_digest = digest;
      have_reference = true;
    } else if (!(digest == reference_digest)) {
      outcome.error = "plan content digest changed between samples: planning is not deterministic";
      return outcome;
    }

    total_steps += static_cast<std::uint64_t>(plan.steps.size());
    total_permutations += report.permutations_verified;
    outcome.row.steps = static_cast<std::uint64_t>(plan.steps.size());
    outcome.row.stages = static_cast<std::uint64_t>(plan.stages.size());
  }
  const std::chrono::steady_clock::time_point finished = std::chrono::steady_clock::now();

  const double seconds = std::chrono::duration<double>(finished - started).count();
  const double per_second = seconds > 0.0 ? 1.0 / seconds : 0.0;

  outcome.row.devices = device_count;
  outcome.row.plans_per_second = static_cast<double>(iterations) * per_second;
  outcome.row.steps_per_second = static_cast<double>(total_steps) * per_second;
  outcome.row.permutations_per_second = static_cast<double>(total_permutations) * per_second;
  outcome.ok = true;
  return outcome;
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  const std::from_chars_result parsed = std::from_chars(first, last, value);
  if (parsed.ec != std::errc{} || parsed.ptr != last) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t iterations = 5;
  std::uint64_t seed = 20240917;

  if (argc > 1 && !parse_u64(argv[1], iterations)) {
    std::fprintf(stderr, "cplan_bench: argument 1 must be a non-negative iteration count\n");
    return 2;
  }
  if (argc > 2 && !parse_u64(argv[2], seed)) {
    std::fprintf(stderr, "cplan_bench: argument 2 must be a non-negative seed\n");
    return 2;
  }
  if (iterations == 0) {
    iterations = 1;
  }

  std::printf(
      "cplan_bench: dual-path chain, link capacity bump + per-device config set; measured work = "
      "completed generate_plan + exhaustive verify_plan; iterations=%llu seed=%llu\n",
      static_cast<unsigned long long>(iterations), static_cast<unsigned long long>(seed));
  std::printf("%-9s %8s %8s %14s %16s %20s\n", "devices", "steps", "stages", "plans/sec",
              "steps/sec", "verified perms/sec");

  const std::uint32_t sizes[] = {8, 16, 32, 64};
  for (const std::uint32_t device_count : sizes) {
    const SizeResult result = measure_size(device_count, iterations, seed);
    if (!result.ok) {
      std::fprintf(stderr, "cplan_bench: topology of %u devices failed: %s\n", device_count,
                   result.error.c_str());
      return 1;
    }
    std::printf("%-9u %8llu %8llu %14.2f %16.0f %20.0f\n", device_count,
                static_cast<unsigned long long>(result.row.steps),
                static_cast<unsigned long long>(result.row.stages), result.row.plans_per_second,
                result.row.steps_per_second, result.row.permutations_per_second);
  }
  return 0;
}
