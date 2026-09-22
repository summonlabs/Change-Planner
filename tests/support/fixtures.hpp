#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "change_planner/model/constraints.hpp"
#include "change_planner/model/evidence.hpp"
#include "change_planner/model/target.hpp"
#include "change_planner/plan/generate.hpp"
#include "change_planner/plan/verify.hpp"

namespace cplan_test {

// Deterministic splitmix64 generator: every randomized scenario is reproducible
// from its seed.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next_u64();
  std::uint32_t next_bounded(std::uint32_t bound);
  bool chance(std::uint32_t numerator, std::uint32_t denominator);
  std::uint64_t seed() const { return state_; }

 private:
  std::uint64_t state_;
};

struct TopologyOptions {
  cplan::CapacityUnits link_capacity{10000};
  cplan::CapacityUnits device_capacity{20000};
  cplan::DemandUnits demand{100};
  cplan::Permille headroom{0};
  cplan::TimestampNs instant{1000000000};
  bool with_maintenance_windows{true};
  bool with_exclusions{false};
  std::uint32_t max_stage_width{4};
};

struct Scenario {
  cplan::CurrentStateSnapshot state;
  cplan::TargetIntent target;
  cplan::SafetyConstraints constraints;
  cplan::TopologyEvidence evidence;
  cplan::PlanRequest request;
};

// Two disjoint paths between a shared pair of endpoints.
Scenario make_ladder(const TopologyOptions& options);

// Two fully independent routes serving one workload, each in its own failure
// domain, so single-domain failure tolerance is meaningful.
Scenario make_dual_homed(const TopologyOptions& options);

// Random connected topology with random links, routes and workloads.
Scenario make_random(const TopologyOptions& options, Rng& rng, std::uint32_t device_count,
                     std::uint32_t domain_count, std::uint32_t extra_links);

// Rebuilds the plan request from the scenario members and canonicalizes every
// collection. Always call after mutating a scenario.
void refresh_request(Scenario& scenario,
                     const cplan::Objective& objective = cplan::Objective::default_objective(),
                     const cplan::PlanningLimits& limits = {});

// Convenience: builds a request, plans, and returns the result.
cplan::PlanResult run_plan(const Scenario& scenario);

// Full capability set for every device in the state.
cplan::CapabilitySet all_capabilities();

// Evidence that matches the state exactly (generation, incarnation, versions).
cplan::TopologyEvidence evidence_for(const cplan::CurrentStateSnapshot& state);

// Applies every step of a plan in canonical order and returns the resulting
// state; fails when the plan cannot be applied.
cplan::Result<cplan::CurrentStateSnapshot> apply_plan(const cplan::Plan& plan,
                                                      const cplan::PlanRequest& request);

// Counts violations with Severity::violation.
std::size_t hard_violation_count(const std::vector<cplan::Violation>& violations);

// Creates a fresh, unique scratch directory for file-based tests and returns its
// absolute path. Callers remove it with remove_scratch_directory.
std::string make_scratch_directory(const std::string& tag);
void remove_scratch_directory(const std::string& path);

// Reads a whole file into bytes (test helper for corruption scenarios).
std::vector<std::byte> read_file_bytes(const std::string& path);
void write_file_bytes(const std::string& path, const std::vector<std::byte>& bytes);

}  // namespace cplan_test
