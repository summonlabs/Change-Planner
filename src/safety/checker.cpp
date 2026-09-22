#include "change_planner/safety/checker.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>

#include "change_planner/core/checked.hpp"

namespace cplan {

const char* to_string(InvariantId id) noexcept {
  switch (id) {
    case InvariantId::connectivity:
      return "connectivity";
    case InvariantId::failure_domain_redundancy:
      return "failure-domain-redundancy";
    case InvariantId::path_diversity:
      return "path-diversity";
    case InvariantId::capacity_headroom:
      return "capacity-headroom";
    case InvariantId::maintenance_exclusion:
      return "maintenance-exclusion";
    case InvariantId::maintenance_window:
      return "maintenance-window";
    case InvariantId::change_serialization:
      return "change-serialization";
    case InvariantId::workload_contract:
      return "workload-contract";
    case InvariantId::capability_evidence:
      return "capability-evidence";
    case InvariantId::generation_binding:
      return "generation-binding";
    case InvariantId::service_continuity:
      return "service-continuity";
  }
  return "unknown";
}

Result<InvariantId> parse_invariant_id(std::string_view token) {
  if (token == "connectivity") return InvariantId::connectivity;
  if (token == "failure-domain-redundancy") return InvariantId::failure_domain_redundancy;
  if (token == "path-diversity") return InvariantId::path_diversity;
  if (token == "capacity-headroom") return InvariantId::capacity_headroom;
  if (token == "maintenance-exclusion") return InvariantId::maintenance_exclusion;
  if (token == "maintenance-window") return InvariantId::maintenance_window;
  if (token == "change-serialization") return InvariantId::change_serialization;
  if (token == "workload-contract") return InvariantId::workload_contract;
  if (token == "capability-evidence") return InvariantId::capability_evidence;
  if (token == "generation-binding") return InvariantId::generation_binding;
  if (token == "service-continuity") return InvariantId::service_continuity;
  return Status::error(ErrorCode::malformed_input,
                       "unknown invariant token: " + std::string(token));
}

const char* to_string(Severity severity) noexcept {
  switch (severity) {
    case Severity::violation:
      return "violation";
    case Severity::note:
      return "note";
  }
  return "unknown";
}

std::string Violation::to_string() const {
  std::string out = std::string(cplan::to_string(invariant)) + "[" +
                    std::string(cplan::to_string(severity)) + "]: " + message;
  if (!evidence.empty()) {
    out += " evidence=";
    for (std::size_t i = 0; i < evidence.size(); ++i) {
      if (i != 0) {
        out += ",";
      }
      out += evidence[i].to_string();
    }
  }
  return out;
}

namespace {

// Search budget for the exact maximum-link-disjoint-path computation. When the
// budget is exhausted the checker reports a violation instead of assuming the
// diversity requirement is met: an unproven safety property is never certified.
constexpr std::uint64_t kDiversitySearchBudget = 200000;

bool device_serviceable(const Device& device) {
  return device.spec.state == EntityState::present;
}

bool link_serviceable(const Link& link) {
  return link.state == EntityState::present;
}

Violation make_violation(InvariantId invariant, std::vector<EntityRef> evidence,
                         std::string detail_code, std::string message) {
  Violation violation;
  violation.invariant = invariant;
  violation.severity = Severity::violation;
  violation.evidence = std::move(evidence);
  violation.detail_code = std::move(detail_code);
  violation.message = std::move(message);
  std::sort(violation.evidence.begin(), violation.evidence.end());
  violation.evidence.erase(std::unique(violation.evidence.begin(), violation.evidence.end()),
                           violation.evidence.end());
  return violation;
}

bool route_carries_traffic(const CurrentStateSnapshot& state, const Route& route) {
  if (route.state != RouteState::active) {
    return false;
  }
  if (!route_is_walkable(state, route)) {
    return false;
  }
  for (const LinkId& link_id : route.spec.path) {
    const Link* link = state.find_link(link_id);
    if (link == nullptr || !link_serviceable(*link)) {
      return false;
    }
  }
  const Device* source = state.find_device(route.spec.source);
  const Device* sink = state.find_device(route.spec.sink);
  if (source == nullptr || sink == nullptr) {
    return false;
  }
  return device_serviceable(*source) && device_serviceable(*sink);
}

struct EligibleRoute {
  const Route* route{nullptr};
  const RouteBinding* binding{nullptr};
};

std::vector<EligibleRoute> eligible_routes(const CurrentStateSnapshot& state,
                                           const Workload& workload) {
  std::vector<EligibleRoute> eligible;
  for (const RouteBinding& binding : workload.bindings) {
    if (!binding.active) {
      continue;
    }
    const Route* route = state.find_route(binding.route);
    if (route == nullptr || !route_carries_traffic(state, *route)) {
      continue;
    }
    bool forbidden = false;
    for (const FailureDomainId& domain : route_domains(state, *route)) {
      if (std::find(workload.requirements.forbidden_domains.begin(),
                    workload.requirements.forbidden_domains.end(),
                    domain) != workload.requirements.forbidden_domains.end()) {
        forbidden = true;
        break;
      }
    }
    if (forbidden) {
      continue;
    }
    eligible.push_back(EligibleRoute{route, &binding});
  }
  return eligible;
}

void explore_disjoint(std::size_t index, std::size_t taken,
                      const std::vector<std::vector<LinkId>>& paths, std::set<LinkId>& used,
                      std::size_t& best, std::size_t required, std::uint64_t& budget,
                      bool& exhausted) {
  if (exhausted) {
    return;
  }
  if (budget == 0) {
    exhausted = true;
    return;
  }
  --budget;
  if (best >= required) {
    return;
  }
  if (index == paths.size()) {
    best = std::max(best, taken);
    return;
  }
  if (taken + (paths.size() - index) <= best) {
    return;
  }
  bool disjoint = true;
  for (const LinkId& link_id : paths[index]) {
    if (used.count(link_id) != 0) {
      disjoint = false;
      break;
    }
  }
  if (disjoint) {
    for (const LinkId& link_id : paths[index]) {
      used.insert(link_id);
    }
    explore_disjoint(index + 1, taken + 1, paths, used, best, required, budget, exhausted);
    for (const LinkId& link_id : paths[index]) {
      used.erase(link_id);
    }
  }
  explore_disjoint(index + 1, taken, paths, used, best, required, budget, exhausted);
}

struct DiversityResult {
  std::size_t maximum{0};
  bool budget_exhausted{false};
  std::size_t considered_paths{0};
};

DiversityResult maximum_link_disjoint_paths(const std::vector<EligibleRoute>& eligible,
                                            std::size_t ceiling) {
  DiversityResult result;
  const std::size_t limit = std::min<std::size_t>(eligible.size(), 24);
  result.considered_paths = limit;
  std::vector<std::vector<LinkId>> paths;
  paths.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    paths.push_back(eligible[i].route->spec.path);
  }
  std::set<LinkId> used;
  std::uint64_t budget = kDiversitySearchBudget;
  explore_disjoint(0, 0, paths, used, result.maximum, ceiling, budget, result.budget_exhausted);
  return result;
}

// Failure-domain redundancy is expressed as survivability: for every failure
// domain, how many eligible paths remain if that domain is lost? The minimum
// over all domains is the redundancy the workload actually has.
struct RedundancyResult {
  std::size_t surviving_paths{0};
  FailureDomainId worst_domain;
  bool has_domain{false};
};

RedundancyResult domain_redundancy(const CurrentStateSnapshot& state,
                                   const std::vector<EligibleRoute>& eligible) {
  RedundancyResult result;
  result.surviving_paths = eligible.size();
  std::set<FailureDomainId> domains;
  std::vector<std::vector<FailureDomainId>> per_path;
  per_path.reserve(eligible.size());
  for (const EligibleRoute& entry : eligible) {
    std::vector<FailureDomainId> path_domains = route_domains(state, *entry.route);
    for (const FailureDomainId& domain : path_domains) {
      domains.insert(domain);
    }
    per_path.push_back(std::move(path_domains));
  }
  for (const FailureDomainId& domain : domains) {
    std::size_t surviving = 0;
    for (const std::vector<FailureDomainId>& path_domains : per_path) {
      if (std::find(path_domains.begin(), path_domains.end(), domain) == path_domains.end()) {
        ++surviving;
      }
    }
    if (!result.has_domain || surviving < result.surviving_paths) {
      result.surviving_paths = surviving;
      result.worst_domain = domain;
      result.has_domain = true;
    }
  }
  return result;
}

}  // namespace

bool change_permitted_at(const Device& device, TimestampNs instant, std::string* blocking_reason) {
  for (const ExclusionWindow& window : device.exclusions) {
    if (window.contains(instant)) {
      if (blocking_reason != nullptr) {
        *blocking_reason = "device '" + device.id.str() + "' is inside exclusion window '" +
                           window.id.str() + "'" +
                           (window.reason_code.empty() ? "" : " (" + window.reason_code + ")");
      }
      return false;
    }
  }
  return true;
}

bool maintenance_permitted_at(const Device& device, TimestampNs instant,
                              std::string* blocking_reason) {
  for (const MaintenanceWindow& window : device.maintenance_windows) {
    if (window.contains(instant)) {
      return true;
    }
  }
  if (blocking_reason != nullptr) {
    if (device.maintenance_windows.empty()) {
      *blocking_reason = "device '" + device.id.str() +
                         "' declares no maintenance window, so no maintenance change can be "
                         "scheduled";
    } else {
      TimestampNs next = device.maintenance_windows.front().begin;
      for (const MaintenanceWindow& window : device.maintenance_windows) {
        if (window.begin > instant && window.begin < next) {
          next = window.begin;
        }
      }
      *blocking_reason = "device '" + device.id.str() +
                         "' has no maintenance window open at the planning instant (next window "
                         "begins at " +
                         std::to_string(next.value()) + ")";
    }
  }
  return false;
}

std::vector<DeviceId> devices_out_of_service(const CurrentStateSnapshot& state,
                                             const FailureDomainId& domain) {
  std::vector<DeviceId> result;
  for (const Device& device : state.devices) {
    if (device.spec.domain != domain) {
      continue;
    }
    if (device.spec.state == EntityState::draining ||
        device.spec.state == EntityState::in_maintenance ||
        device.spec.state == EntityState::removed) {
      result.push_back(device.id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<Violation> check_invariants(const CurrentStateSnapshot& state,
                                        const EvaluationContext& context) {
  std::vector<Violation> violations;

  // ---- generation binding -------------------------------------------------
  if (context.constraints != nullptr &&
      context.constraints->enforced(ConstraintKind::generation_binding)) {
    if (state.authority_generation != context.constraints->authority_generation) {
      violations.push_back(make_violation(
          InvariantId::generation_binding, {EntityRef::global()}, "authority-generation-mismatch",
          "snapshot authority generation " + std::to_string(state.authority_generation.value()) +
              " does not match the governing constraint generation " +
              std::to_string(context.constraints->authority_generation.value())));
    }
  }
  if (context.evidence != nullptr) {
    if (context.evidence->topology_generation != state.topology_generation) {
      violations.push_back(make_violation(
          InvariantId::generation_binding, {EntityRef::global()}, "topology-generation-mismatch",
          "capability evidence generation " +
              std::to_string(context.evidence->topology_generation.value()) +
              " does not match the snapshot generation " +
              std::to_string(state.topology_generation.value())));
    }
    if (context.evidence->boot_incarnation != state.boot_incarnation) {
      violations.push_back(make_violation(
          InvariantId::generation_binding, {EntityRef::global()}, "boot-incarnation-mismatch",
          "capability evidence was observed under boot incarnation " +
              std::to_string(context.evidence->boot_incarnation.value()) +
              " but the snapshot was observed under " +
              std::to_string(state.boot_incarnation.value())));
    }
  }

  // ---- capability evidence ------------------------------------------------
  if (context.constraints != nullptr &&
      context.constraints->enforced(ConstraintKind::capability_evidence)) {
    for (const Device& device : state.devices) {
      if (device.spec.state == EntityState::removed) {
        continue;
      }
      if (context.baseline != nullptr && context.baseline->find_device(device.id) == nullptr) {
        continue;  // created by the plan under evaluation
      }
      if (context.evidence == nullptr) {
        violations.push_back(make_violation(
            InvariantId::capability_evidence, {EntityRef::of(device.id)}, "evidence-absent",
            "no capability evidence supplied for device '" + device.id.str() + "'"));
        continue;
      }
      const CapabilityEvidence* evidence = context.evidence->find(device.id);
      if (evidence == nullptr) {
        violations.push_back(make_violation(
            InvariantId::capability_evidence, {EntityRef::of(device.id)}, "evidence-missing",
            "no capability evidence recorded for device '" + device.id.str() + "'"));
      }
    }
  }

  // ---- connectivity, redundancy, diversity, capacity ----------------------
  std::map<std::string, std::uint64_t> link_load;
  std::map<std::string, std::uint64_t> device_load;

  Permille headroom(0);
  if (context.constraints != nullptr) {
    if (const ConstraintItem* item =
            context.constraints->find(ConstraintKind::capacity_headroom)) {
      headroom = item->headroom;
    }
  }

  for (const Workload& workload : state.workloads) {
    const std::vector<EligibleRoute> eligible = eligible_routes(state, workload);

    const bool check_connectivity =
        context.constraints == nullptr ||
        context.constraints->enforced(ConstraintKind::connectivity);
    const bool check_redundancy =
        context.constraints == nullptr ||
        context.constraints->enforced(ConstraintKind::failure_domain_redundancy);
    const bool check_diversity =
        context.constraints == nullptr ||
        context.constraints->enforced(ConstraintKind::path_diversity);
    const bool check_contracts =
        context.constraints == nullptr ||
        context.constraints->enforced(ConstraintKind::workload_contract);

    if (check_connectivity && workload.requirements.require_connectivity && eligible.empty()) {
      violations.push_back(make_violation(
          InvariantId::connectivity, {EntityRef::of(workload.id)}, "no-eligible-route",
          "workload '" + workload.id.str() +
              "' requires connectivity but has no active, connected, permitted route"));
    }

    if (check_redundancy && workload.requirements.min_disjoint_domains > 0) {
      const RedundancyResult redundancy = domain_redundancy(state, eligible);
      if (redundancy.surviving_paths < workload.requirements.min_disjoint_domains) {
        std::string detail =
            "workload '" + workload.id.str() + "' has " +
            std::to_string(redundancy.surviving_paths) +
            " path(s) surviving the loss of a single failure domain but requires " +
            std::to_string(workload.requirements.min_disjoint_domains);
        std::vector<EntityRef> evidence{EntityRef::of(workload.id)};
        if (redundancy.has_domain) {
          detail += " (worst domain: '" + redundancy.worst_domain.str() + "')";
          evidence.push_back(EntityRef::of(redundancy.worst_domain));
        }
        violations.push_back(make_violation(InvariantId::failure_domain_redundancy,
                                            std::move(evidence), "insufficient-redundancy",
                                            detail));
      }
    }

    if (check_diversity && workload.requirements.min_link_disjoint_paths > 0) {
      const DiversityResult diversity = maximum_link_disjoint_paths(
          eligible, workload.requirements.min_link_disjoint_paths);
      if (diversity.budget_exhausted) {
        violations.push_back(make_violation(
            InvariantId::path_diversity, {EntityRef::of(workload.id)},
            "diversity-proof-budget-exhausted",
            "workload '" + workload.id.str() +
                "' exceeds the exact path-diversity search budget; safety cannot be certified"));
      } else if (diversity.maximum < workload.requirements.min_link_disjoint_paths) {
        violations.push_back(make_violation(
            InvariantId::path_diversity, {EntityRef::of(workload.id)},
            "insufficient-diversity",
            "workload '" + workload.id.str() + "' has " + std::to_string(diversity.maximum) +
                " link-disjoint path(s) in service but requires " +
                std::to_string(workload.requirements.min_link_disjoint_paths)));
      }
    }

    for (const EligibleRoute& entry : eligible) {
      const std::uint64_t demand = entry.binding->demand.value();
      for (const LinkId& link_id : entry.route->spec.path) {
        auto [slot, inserted] = link_load.try_emplace(link_id.str(), 0);
        static_cast<void>(inserted);
        const auto total = checked_add_u64(slot->second, demand);
        slot->second = total.ok() ? total.value() : std::numeric_limits<std::uint64_t>::max();
      }
      for (const DeviceId& device_id : {entry.route->spec.source, entry.route->spec.sink}) {
        auto [slot, inserted] = device_load.try_emplace(device_id.str(), 0);
        static_cast<void>(inserted);
        const auto total = checked_add_u64(slot->second, demand);
        slot->second = total.ok() ? total.value() : std::numeric_limits<std::uint64_t>::max();
      }
    }

    if (check_contracts && workload.requirements.min_guaranteed_capacity.value() > 0) {
      for (const EligibleRoute& entry : eligible) {
        std::uint64_t available = std::numeric_limits<std::uint64_t>::max();
        for (const LinkId& link_id : entry.route->spec.path) {
          const Link* link = state.find_link(link_id);
          if (link == nullptr) {
            continue;
          }
          const std::uint64_t load = link_load.count(link_id.str()) != 0 ? link_load[link_id.str()] : 0;
          const std::uint64_t usable =
              link->spec.capacity.value() > link->spec.reserved.value()
                  ? link->spec.capacity.value() - link->spec.reserved.value()
                  : 0;
          available = std::min(available, usable > load ? usable - load : 0);
        }
        for (const DeviceId& device_id : {entry.route->spec.source, entry.route->spec.sink}) {
          const Device* device = state.find_device(device_id);
          if (device == nullptr) {
            continue;
          }
          const std::uint64_t load =
              device_load.count(device_id.str()) != 0 ? device_load[device_id.str()] : 0;
          const std::uint64_t usable = device->spec.termination_capacity.value() >
                                               device->spec.termination_reserved.value()
                                           ? device->spec.termination_capacity.value() -
                                                 device->spec.termination_reserved.value()
                                           : 0;
          available = std::min(available, usable > load ? usable - load : 0);
        }
        if (available < workload.requirements.min_guaranteed_capacity.value()) {
          violations.push_back(make_violation(
              InvariantId::workload_contract,
              {EntityRef::of(workload.id), EntityRef::of(entry.route->id)}, "guarantee-shortfall",
              "workload '" + workload.id.str() + "' has " + std::to_string(available) +
                  " units of headroom on route '" + entry.route->id.str() +
                  "' but its contract guarantees " +
                  std::to_string(workload.requirements.min_guaranteed_capacity.value())));
        }
      }
    }
  }

  if (context.constraints != nullptr &&
      context.constraints->enforced(ConstraintKind::capacity_headroom)) {
    for (const Link& link : state.links) {
      if (link.state == EntityState::removed) {
        continue;
      }
      const std::uint64_t load = link_load.count(link.id.str()) != 0 ? link_load[link.id.str()] : 0;
      const std::uint64_t usable = link.spec.capacity.value() > link.spec.reserved.value()
                                       ? link.spec.capacity.value() - link.spec.reserved.value()
                                       : 0;
      const auto allowed = checked_mul_u64(usable, 1000 - std::min<std::uint32_t>(headroom.value(), 1000));
      const std::uint64_t limit = allowed.ok() ? allowed.value() / 1000 : usable;
      if (load > limit) {
        violations.push_back(make_violation(
            InvariantId::capacity_headroom, {EntityRef::of(link.id)}, "link-over-committed",
            "link '" + link.id.str() + "' carries " + std::to_string(load) +
                " units but the headroom policy allows at most " + std::to_string(limit)));
      }
    }
    for (const Device& device : state.devices) {
      if (device.spec.state == EntityState::removed) {
        continue;
      }
      const std::uint64_t load =
          device_load.count(device.id.str()) != 0 ? device_load[device.id.str()] : 0;
      const std::uint64_t usable =
          device.spec.termination_capacity.value() > device.spec.termination_reserved.value()
              ? device.spec.termination_capacity.value() - device.spec.termination_reserved.value()
              : 0;
      const auto allowed = checked_mul_u64(usable, 1000 - std::min<std::uint32_t>(headroom.value(), 1000));
      const std::uint64_t limit = allowed.ok() ? allowed.value() / 1000 : usable;
      if (load > limit) {
        violations.push_back(make_violation(
            InvariantId::capacity_headroom, {EntityRef::of(device.id)}, "device-over-committed",
            "device '" + device.id.str() + "' terminates " + std::to_string(load) +
                " units but the headroom policy allows at most " + std::to_string(limit)));
      }
    }
  }

  // ---- maintenance windows and exclusions ---------------------------------
  if (context.constraints != nullptr) {
    for (const Device& device : state.devices) {
      if (context.constraints->enforced(ConstraintKind::maintenance_exclusion)) {
        if (device.spec.state == EntityState::draining ||
            device.spec.state == EntityState::in_maintenance) {
          std::string reason;
          if (!change_permitted_at(device, context.instant, &reason)) {
            violations.push_back(make_violation(InvariantId::maintenance_exclusion,
                                                {EntityRef::of(device.id)}, "change-in-exclusion",
                                                reason));
          }
        }
      }
      if (context.constraints->enforced(ConstraintKind::maintenance_window)) {
        if (device.spec.state == EntityState::in_maintenance) {
          std::string reason;
          if (!maintenance_permitted_at(device, context.instant, &reason)) {
            violations.push_back(make_violation(InvariantId::maintenance_window,
                                                {EntityRef::of(device.id)}, "no-open-window",
                                                reason));
          }
        }
      }
    }
  }

  // ---- change serialization and continuity --------------------------------
  if (context.constraints != nullptr &&
      context.constraints->enforced(ConstraintKind::change_serialization)) {
    std::set<FailureDomainId> domains;
    for (const Device& device : state.devices) {
      domains.insert(device.spec.domain);
    }
    std::uint32_t max_domains = 1;
    std::uint32_t permitted = 1;
    if (const ConstraintItem* item =
            context.constraints->find(ConstraintKind::change_serialization)) {
      max_domains = item->max_domains_in_maintenance;
      permitted = item->max_concurrent_changes;
    }
    std::size_t domains_in_change = 0;
    for (const FailureDomainId& domain : domains) {
      const std::vector<DeviceId> out_of_service = devices_out_of_service(state, domain);
      if (out_of_service.empty()) {
        continue;
      }
      ++domains_in_change;
      std::uint32_t domain_permitted = permitted;
      for (const DeviceId& device_id : out_of_service) {
        const Device* device = state.find_device(device_id);
        if (device != nullptr && device->spec.max_concurrent_changes < domain_permitted) {
          domain_permitted = device->spec.max_concurrent_changes;
        }
      }
      if (out_of_service.size() > domain_permitted) {
        violations.push_back(make_violation(
            InvariantId::change_serialization, {EntityRef::of(domain)}, "domain-over-serialized",
            "failure domain '" + domain.str() + "' has " +
                std::to_string(out_of_service.size()) +
                " device(s) out of service but permits " +
                std::to_string(domain_permitted)));
      }
    }
    if (domains_in_change > max_domains) {
      violations.push_back(make_violation(
          InvariantId::service_continuity, {}, "too-many-domains-in-change",
          std::to_string(domains_in_change) +
              " failure domain(s) have devices out of service but the policy permits " +
              std::to_string(max_domains)));
    }
  }

  // ---- workload / network contracts ---------------------------------------
  if (context.constraints != nullptr &&
      context.constraints->enforced(ConstraintKind::workload_contract)) {
    for (const NetworkContract& contract : context.constraints->contracts) {
      std::uint64_t active_demand = 0;
      std::set<std::string> contract_domains;
      std::set<std::string> contract_change_domains;
      std::vector<std::vector<FailureDomainId>> contract_paths;
      std::set<FailureDomainId> contract_domain_paths;
      bool missing_workload = false;
      for (const WorkloadId& workload_id : contract.workloads) {
        const Workload* workload = state.find_workload(workload_id);
        if (workload == nullptr) {
          missing_workload = true;
          continue;
        }
        const std::vector<EligibleRoute> eligible = eligible_routes(state, *workload);
        for (const EligibleRoute& entry : eligible) {
          const auto total = checked_add_u64(active_demand, entry.binding->demand.value());
          active_demand = total.ok() ? total.value() : std::numeric_limits<std::uint64_t>::max();
          const std::vector<FailureDomainId> path_domains = route_domains(state, *entry.route);
          for (const FailureDomainId& domain : path_domains) {
            contract_domains.insert(domain.str());
            contract_domain_paths.insert(domain);
            if (!devices_out_of_service(state, domain).empty()) {
              contract_change_domains.insert(domain.str());
            }
          }
          contract_paths.push_back(path_domains);
        }
      }
      if (missing_workload) {
        violations.push_back(make_violation(
            InvariantId::workload_contract, {EntityRef::of(contract.id)}, "contract-workload-missing",
            "contract '" + contract.id.str() + "' references a workload that is not in the state"));
      }
      if (active_demand < contract.required_units.value()) {
        violations.push_back(make_violation(
            InvariantId::workload_contract, {EntityRef::of(contract.id)}, "contract-capacity-shortfall",
            "contract '" + contract.id.str() + "' has " + std::to_string(active_demand) +
                " units of active capacity but requires " +
                std::to_string(contract.required_units.value())));
      }
      if (contract.min_disjoint_domains > 0) {
        std::size_t redundancy = contract_paths.empty()
                                     ? 0
                                     : std::numeric_limits<std::size_t>::max();
        for (const auto& domain_entry : contract_domain_paths) {
          std::size_t surviving = 0;
          for (const std::vector<FailureDomainId>& path_domains : contract_paths) {
            if (std::find(path_domains.begin(), path_domains.end(), domain_entry) ==
                path_domains.end()) {
              ++surviving;
            }
          }
          redundancy = std::min(redundancy, surviving);
        }
        if (contract_paths.empty()) {
          redundancy = 0;
        }
        if (redundancy < contract.min_disjoint_domains) {
          violations.push_back(make_violation(
              InvariantId::workload_contract, {EntityRef::of(contract.id)},
              "contract-redundancy-shortfall",
              "contract '" + contract.id.str() + "' has " + std::to_string(redundancy) +
                  " path(s) surviving the loss of a single failure domain but requires " +
                  std::to_string(contract.min_disjoint_domains)));
        }
      }
      if (contract.max_simultaneous_domain_maintenance > 0 &&
          contract_change_domains.size() > contract.max_simultaneous_domain_maintenance) {
        violations.push_back(make_violation(
            InvariantId::workload_contract, {EntityRef::of(contract.id)},
            "contract-maintenance-concurrency",
            "contract '" + contract.id.str() + "' has " +
                std::to_string(contract_change_domains.size()) +
                " domain(s) in change but permits " +
                std::to_string(contract.max_simultaneous_domain_maintenance)));
      }
      if (contract.forbid_concurrent_changes_in_same_domain) {
        for (const std::string& domain_text : contract_change_domains) {
          const auto domain = FailureDomainId::parse(domain_text);
          if (!domain.ok()) {
            continue;
          }
          const std::vector<DeviceId> out_of_service = devices_out_of_service(state, domain.value());
          if (out_of_service.size() > 1) {
            violations.push_back(make_violation(
                InvariantId::workload_contract, {EntityRef::of(contract.id)},
                "contract-domain-concurrency",
                "contract '" + contract.id.str() + "' forbids concurrent changes in domain '" +
                    domain_text + "' but " + std::to_string(out_of_service.size()) +
                    " devices are out of service"));
          }
        }
      }
    }
  }

  std::sort(violations.begin(), violations.end());
  return violations;
}

bool state_is_safe(const CurrentStateSnapshot& state, const EvaluationContext& context) {
  for (const Violation& violation : check_invariants(state, context)) {
    if (violation.severity == Severity::violation) {
      return false;
    }
  }
  return true;
}

}  // namespace cplan
