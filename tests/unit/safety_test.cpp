#include <algorithm>
#include <string>
#include <vector>

#include "change_planner/model/evaluate.hpp"
#include "change_planner/safety/checker.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/test_framework.hpp"

using namespace cplan_test;

namespace {

cplan::EvaluationContext context_of(const Scenario& scenario) {
  return cplan::EvaluationContext{scenario.request.planning_instant, &scenario.request.constraints,
                                  &scenario.request.evidence};
}

bool has_invariant(const std::vector<cplan::Violation>& violations, cplan::InvariantId invariant) {
  for (const cplan::Violation& violation : violations) {
    if (violation.invariant == invariant && violation.severity == cplan::Severity::violation) {
      return true;
    }
  }
  return false;
}

std::string detail_of(const std::vector<cplan::Violation>& violations,
                      cplan::InvariantId invariant) {
  for (const cplan::Violation& violation : violations) {
    if (violation.invariant == invariant) {
      return violation.detail_code;
    }
  }
  return {};
}

}  // namespace

CPLAN_TEST(safety, healthy_topology_has_no_violations) {
  Scenario scenario = make_ladder(TopologyOptions{});
  const std::vector<cplan::Violation> violations =
      cplan::check_invariants(scenario.state, context_of(scenario));
  CPLAN_CHECK_EQ(hard_violation_count(violations), static_cast<std::size_t>(0));
  CPLAN_CHECK(cplan::state_is_safe(scenario.state, context_of(scenario)));

  Scenario dual = make_dual_homed(TopologyOptions{});
  const std::vector<cplan::Violation> dual_violations =
      cplan::check_invariants(dual.state, context_of(dual));
  CPLAN_CHECK_EQ(hard_violation_count(dual_violations), static_cast<std::size_t>(0));
}

CPLAN_TEST(safety, connectivity_violation_is_detected) {
  Scenario scenario = make_ladder(TopologyOptions{});
  // Break both paths by removing the shared endpoint device.
  cplan::CurrentStateSnapshot broken = scenario.state;
  for (cplan::Device& device : broken.devices) {
    if (device.id.str() == "ep-a") {
      device.spec.state = cplan::EntityState::removed;
    }
  }
  const std::vector<cplan::Violation> violations =
      cplan::check_invariants(broken, context_of(scenario));
  CPLAN_CHECK(has_invariant(violations, cplan::InvariantId::connectivity));
  CPLAN_CHECK(!cplan::state_is_safe(broken, context_of(scenario)));
}

CPLAN_TEST(safety, path_diversity_violation_is_detected) {
  Scenario scenario = make_ladder(TopologyOptions{});
  cplan::CurrentStateSnapshot broken = scenario.state;
  for (cplan::Route& route : broken.routes) {
    if (route.id.str() == "r-2") {
      route.state = cplan::RouteState::inactive;
    }
  }
  const std::vector<cplan::Violation> violations =
      cplan::check_invariants(broken, context_of(scenario));
  CPLAN_CHECK(has_invariant(violations, cplan::InvariantId::path_diversity));
  CPLAN_CHECK_EQ(detail_of(violations, cplan::InvariantId::path_diversity),
                 std::string("insufficient-diversity"));
}

CPLAN_TEST(safety, failure_domain_redundancy_uses_survivability) {
  Scenario scenario = make_dual_homed(TopologyOptions{});
  CPLAN_CHECK(cplan::state_is_safe(scenario.state, context_of(scenario)));

  // Loss of the domain hosting the second path leaves one path: redundancy 1 is
  // satisfied, redundancy 2 is not.
  cplan::CurrentStateSnapshot broken = scenario.state;
  for (cplan::Device& device : broken.devices) {
    if (device.id.str() == "mid-2") {
      device.spec.state = cplan::EntityState::removed;
    }
  }
  const std::vector<cplan::Violation> violations =
      cplan::check_invariants(broken, context_of(scenario));
  CPLAN_CHECK(!has_invariant(violations, cplan::InvariantId::failure_domain_redundancy));

  scenario.request.constraints.items.push_back(
      cplan::ConstraintItem{cplan::ConstraintId::from_validated("strict-redundancy"),
                            cplan::ConstraintKind::connectivity, true, cplan::Permille(0), 1, 1, "",
                            ""});
  for (cplan::Workload& workload : broken.workloads) {
    workload.requirements.min_disjoint_domains = 2;
  }
  const std::vector<cplan::Violation> strict =
      cplan::check_invariants(broken, context_of(scenario));
  CPLAN_CHECK(has_invariant(strict, cplan::InvariantId::failure_domain_redundancy));
}

CPLAN_TEST(safety, capacity_headroom_is_enforced) {
  Scenario scenario = make_ladder(TopologyOptions{});
  // Each link carries exactly one active binding of 100 units.
  cplan::CurrentStateSnapshot loaded = scenario.state;
  for (cplan::Link& link : loaded.links) {
    link.spec.capacity = cplan::CapacityUnits(100);
  }
  CPLAN_CHECK(cplan::state_is_safe(loaded, context_of(scenario)));
  for (cplan::Link& link : loaded.links) {
    link.spec.capacity = cplan::CapacityUnits(99);
  }
  const std::vector<cplan::Violation> violations =
      cplan::check_invariants(loaded, context_of(scenario));
  CPLAN_CHECK(has_invariant(violations, cplan::InvariantId::capacity_headroom));
  CPLAN_CHECK_EQ(detail_of(violations, cplan::InvariantId::capacity_headroom),
                 std::string("link-over-committed"));

  // A headroom obligation tightens the limit further: at 500 permille a link of
  // 199 units may carry at most 99 units.
  for (cplan::ConstraintItem& item : scenario.request.constraints.items) {
    if (item.kind == cplan::ConstraintKind::capacity_headroom) {
      item.headroom = cplan::Permille(500);
    }
  }
  cplan::CurrentStateSnapshot fitted = scenario.state;
  for (cplan::Link& link : fitted.links) {
    link.spec.capacity = cplan::CapacityUnits(200);
  }
  CPLAN_CHECK(cplan::state_is_safe(fitted, context_of(scenario)));
  for (cplan::Link& link : fitted.links) {
    link.spec.capacity = cplan::CapacityUnits(199);
  }
  CPLAN_CHECK(!cplan::state_is_safe(fitted, context_of(scenario)));
}

CPLAN_TEST(safety, maintenance_exclusion_and_window_are_enforced) {
  TopologyOptions options;
  options.with_exclusions = true;
  Scenario scenario = make_ladder(options);

  // A device that is out of service inside its exclusion window is a violation.
  cplan::CurrentStateSnapshot draining = scenario.state;
  for (cplan::Device& device : draining.devices) {
    if (device.id.str() == "mid-1") {
      device.spec.state = cplan::EntityState::draining;
    }
  }
  const std::vector<cplan::Violation> violations =
      cplan::check_invariants(draining, context_of(scenario));
  CPLAN_CHECK(has_invariant(violations, cplan::InvariantId::maintenance_exclusion));

  // A device in maintenance outside every maintenance window is a violation.
  cplan::EvaluationContext late = context_of(scenario);
  late.instant = cplan::TimestampNs(9000000000);
  cplan::CurrentStateSnapshot maintenance = scenario.state;
  for (cplan::Device& device : maintenance.devices) {
    if (device.id.str() == "mid-1") {
      device.spec.state = cplan::EntityState::in_maintenance;
    }
  }
  const std::vector<cplan::Violation> late_violations =
      cplan::check_invariants(maintenance, late);
  CPLAN_CHECK(has_invariant(late_violations, cplan::InvariantId::maintenance_window));
}

CPLAN_TEST(safety, change_serialization_limits_out_of_service_devices) {
  Scenario scenario = make_ladder(TopologyOptions{});
  cplan::CurrentStateSnapshot two_out = scenario.state;
  for (cplan::Device& device : two_out.devices) {
    if (device.id.str() == "mid-1" || device.id.str() == "mid-2") {
      device.spec.state = cplan::EntityState::draining;
    }
  }
  const std::vector<cplan::Violation> violations =
      cplan::check_invariants(two_out, context_of(scenario));
  // Both devices live in different failure domains, so the per-domain limit is
  // respected but the service-continuity limit (one domain in change) is not.
  CPLAN_CHECK(has_invariant(violations, cplan::InvariantId::service_continuity));

  Scenario same_domain = make_ladder(TopologyOptions{});
  cplan::CurrentStateSnapshot pair = same_domain.state;
  for (cplan::Device& device : pair.devices) {
    if (device.id.str() == "mid-1" || device.id.str() == "mid-2") {
      device.spec.state = cplan::EntityState::draining;
      device.spec.domain = cplan::FailureDomainId::from_validated("dom-shared");
    }
  }
  const std::vector<cplan::Violation> pair_violations =
      cplan::check_invariants(pair, context_of(same_domain));
  CPLAN_CHECK(has_invariant(pair_violations, cplan::InvariantId::change_serialization));
}

CPLAN_TEST(safety, workload_contracts_are_enforced) {
  Scenario scenario = make_ladder(TopologyOptions{});
  cplan::NetworkContract contract;
  contract.id = cplan::ContractId::from_validated("contract-strict");
  contract.workloads = {cplan::WorkloadId::from_validated("w-1")};
  contract.required_units = cplan::CapacityUnits(400);
  contract.min_disjoint_domains = 1;
  contract.max_simultaneous_domain_maintenance = 1;
  contract.forbid_concurrent_changes_in_same_domain = true;
  contract.sla_code = "gold";
  scenario.request.constraints.contracts.push_back(contract);

  const std::vector<cplan::Violation> shortfall =
      cplan::check_invariants(scenario.state, context_of(scenario));
  CPLAN_CHECK(has_invariant(shortfall, cplan::InvariantId::workload_contract));

  // Two devices out of service in one domain violates the contract's
  // concurrency clause.
  Scenario serialized = make_ladder(TopologyOptions{});
  serialized.request.constraints.contracts.push_back(contract);
  serialized.request.constraints.contracts.back().required_units = cplan::CapacityUnits(100);
  serialized.request.constraints.contracts.back().min_disjoint_domains = 0;
  cplan::CurrentStateSnapshot two_out = serialized.state;
  for (cplan::Device& device : two_out.devices) {
    if (device.id.str() == "mid-1" || device.id.str() == "mid-2") {
      device.spec.state = cplan::EntityState::draining;
      device.spec.domain = cplan::FailureDomainId::from_validated("dom-shared");
    }
  }
  const std::vector<cplan::Violation> violations =
      cplan::check_invariants(two_out, context_of(serialized));
  CPLAN_CHECK(has_invariant(violations, cplan::InvariantId::workload_contract));
}

CPLAN_TEST(safety, capability_evidence_and_generation_binding_are_fenced) {
  Scenario scenario = make_ladder(TopologyOptions{});
  CPLAN_CHECK(cplan::state_is_safe(scenario.state, context_of(scenario)));

  {
    cplan::EvaluationContext missing = context_of(scenario);
    missing.evidence = nullptr;
    const std::vector<cplan::Violation> violations =
        cplan::check_invariants(scenario.state, missing);
    CPLAN_CHECK(has_invariant(violations, cplan::InvariantId::capability_evidence));
  }
  {
    Scenario stale = make_ladder(TopologyOptions{});
    stale.request.evidence.boot_incarnation = cplan::BootIncarnation(99);
    const std::vector<cplan::Violation> violations =
        cplan::check_invariants(stale.state, context_of(stale));
    CPLAN_CHECK(has_invariant(violations, cplan::InvariantId::generation_binding));
  }
  {
    Scenario stale = make_ladder(TopologyOptions{});
    stale.request.constraints.authority_generation = cplan::AuthorityGeneration(77);
    const std::vector<cplan::Violation> violations =
        cplan::check_invariants(stale.state, context_of(stale));
    CPLAN_CHECK(has_invariant(violations, cplan::InvariantId::generation_binding));
  }
}

CPLAN_TEST(safety, disabled_constraints_are_not_enforced) {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::ConstraintItem& item : scenario.request.constraints.items) {
    if (item.kind == cplan::ConstraintKind::capacity_headroom ||
        item.kind == cplan::ConstraintKind::path_diversity) {
      item.enabled = false;
    }
  }
  cplan::CurrentStateSnapshot broken = scenario.state;
  for (cplan::Link& link : broken.links) {
    link.spec.capacity = cplan::CapacityUnits(1);
  }
  for (cplan::Route& route : broken.routes) {
    if (route.id.str() == "r-2") {
      route.state = cplan::RouteState::inactive;
    }
  }
  const std::vector<cplan::Violation> violations =
      cplan::check_invariants(broken, context_of(scenario));
  CPLAN_CHECK(!has_invariant(violations, cplan::InvariantId::capacity_headroom));
  CPLAN_CHECK(!has_invariant(violations, cplan::InvariantId::path_diversity));
}

CPLAN_TEST(safety, violations_are_deterministically_ordered) {
  Scenario scenario = make_ladder(TopologyOptions{});
  cplan::CurrentStateSnapshot broken = scenario.state;
  for (cplan::Link& link : broken.links) {
    link.spec.capacity = cplan::CapacityUnits(1);
  }
  broken.devices.front().spec.state = cplan::EntityState::in_maintenance;
  const std::vector<cplan::Violation> first =
      cplan::check_invariants(broken, context_of(scenario));
  const std::vector<cplan::Violation> second =
      cplan::check_invariants(broken, context_of(scenario));
  CPLAN_CHECK_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    CPLAN_CHECK(first[i] == second[i]);
  }
  for (std::size_t i = 1; i < first.size(); ++i) {
    CPLAN_CHECK(first[i - 1] < first[i] || first[i - 1] == first[i]);
  }
}
