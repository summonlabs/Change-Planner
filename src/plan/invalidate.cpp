#include "change_planner/plan/invalidate.hpp"

#include <algorithm>
#include <set>

#include "change_planner/core/checked.hpp"
#include "change_planner/model/evaluate.hpp"

namespace cplan {

const char* to_string(InvalidationDimension dimension) noexcept {
  switch (dimension) {
    case InvalidationDimension::none:
      return "none";
    case InvalidationDimension::authority:
      return "authority";
    case InvalidationDimension::incarnation:
      return "incarnation";
    case InvalidationDimension::epoch:
      return "epoch";
    case InvalidationDimension::topology:
      return "topology";
    case InvalidationDimension::capability:
      return "capability";
    case InvalidationDimension::target_intent:
      return "target-intent";
    case InvalidationDimension::constraint_policy:
      return "constraint-policy";
    case InvalidationDimension::planning_instant:
      return "planning-instant";
  }
  return "unknown";
}

Result<InvalidationDimension> parse_invalidation_dimension(std::string_view token) {
  if (token == "none") return InvalidationDimension::none;
  if (token == "authority") return InvalidationDimension::authority;
  if (token == "incarnation") return InvalidationDimension::incarnation;
  if (token == "epoch") return InvalidationDimension::epoch;
  if (token == "topology") return InvalidationDimension::topology;
  if (token == "capability") return InvalidationDimension::capability;
  if (token == "target-intent") return InvalidationDimension::target_intent;
  if (token == "constraint-policy") return InvalidationDimension::constraint_policy;
  if (token == "planning-instant") return InvalidationDimension::planning_instant;
  return Status::error(ErrorCode::malformed_input,
                       "unknown invalidation dimension token: " + std::string(token));
}

namespace {

Permille permille_change(std::uint64_t previous, std::uint64_t current) {
  if (previous == 0) {
    return Permille(current == 0 ? 0u : 1000u);
  }
  const std::uint64_t difference = previous > current ? previous - current : current - previous;
  const auto scaled = checked_mul_u64(difference, 1000);
  if (!scaled.ok()) {
    return Permille(1000);
  }
  const std::uint64_t ratio = scaled.value() / previous;
  return Permille(static_cast<std::uint32_t>(std::min<std::uint64_t>(ratio, 1000000)));
}

bool exempt(const ChangeTolerance& tolerance, const EntityRef& reference) {
  const std::string text = reference.to_string();
  return std::find(tolerance.exempt_identities.begin(), tolerance.exempt_identities.end(), text) !=
             tolerance.exempt_identities.end() ||
         std::find(tolerance.exempt_identities.begin(), tolerance.exempt_identities.end(),
                   reference.identity) != tolerance.exempt_identities.end();
}

}  // namespace

TopologySummary summarize(const CurrentStateSnapshot& state) {
  TopologySummary summary;
  std::uint64_t device_capacity = 0;
  std::uint64_t link_capacity = 0;
  for (const Device& device : state.devices) {
    EntityFingerprint fingerprint;
    fingerprint.reference = EntityRef::of(device.id);
    CanonicalEncoder encoder;
    encode_device(encoder, device);
    fingerprint.content = encoder.digest();
    summary.entities.push_back(fingerprint);
    const auto total = checked_add_u64(device_capacity, device.spec.termination_capacity.value());
    device_capacity = total.ok() ? total.value() : std::numeric_limits<std::uint64_t>::max();
  }
  for (const Link& link : state.links) {
    EntityFingerprint fingerprint;
    fingerprint.reference = EntityRef::of(link.id);
    CanonicalEncoder encoder;
    encode_link(encoder, link);
    fingerprint.content = encoder.digest();
    summary.entities.push_back(fingerprint);
    const auto total = checked_add_u64(link_capacity, link.spec.capacity.value());
    link_capacity = total.ok() ? total.value() : std::numeric_limits<std::uint64_t>::max();
  }
  for (const Route& route : state.routes) {
    EntityFingerprint fingerprint;
    fingerprint.reference = EntityRef::of(route.id);
    CanonicalEncoder encoder;
    encode_route(encoder, route);
    fingerprint.content = encoder.digest();
    summary.entities.push_back(fingerprint);
  }
  std::uint64_t demand = 0;
  for (const Workload& workload : state.workloads) {
    EntityFingerprint fingerprint;
    fingerprint.reference = EntityRef::of(workload.id);
    CanonicalEncoder encoder;
    encode_workload(encoder, workload);
    fingerprint.content = encoder.digest();
    summary.entities.push_back(fingerprint);
    for (const RouteBinding& binding : workload.bindings) {
      if (!binding.active) {
        continue;
      }
      const auto total = checked_add_u64(demand, binding.demand.value());
      demand = total.ok() ? total.value() : std::numeric_limits<std::uint64_t>::max();
    }
  }
  std::sort(summary.entities.begin(), summary.entities.end());
  summary.total_device_capacity = device_capacity;
  summary.total_link_capacity = link_capacity;
  summary.total_active_demand = demand;
  return summary;
}

std::string InvalidationDecision::explain() const {
  std::string out = valid ? "plan remains valid" : "plan invalidated";
  out += " (";
  out += cplan::to_string(dimension);
  out += ")";
  if (!detail_code.empty()) {
    out += " [";
    out += detail_code;
    out += "]";
  }
  if (!message.empty()) {
    out += ": ";
    out += message;
  }
  if (added_entities != 0 || removed_entities != 0 || changed_entities != 0) {
    out += " (added=" + std::to_string(added_entities) +
           " removed=" + std::to_string(removed_entities) +
           " changed=" + std::to_string(changed_entities) + ")";
  }
  return out;
}

InvalidationDecision evaluate_validity(const Plan& plan, const InvalidationContext& context) {
  InvalidationDecision decision;
  decision.valid = true;
  decision.dimension = InvalidationDimension::none;
  decision.tolerance = context.constraints.tolerance;

  const ValidityBinding& binding = plan.binding;

  if (!(binding.authority == context.state.authority)) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::authority;
    decision.detail_code = "authority-changed";
    decision.message = "plan was computed under authority '" + binding.authority.str() +
                       "' but the current authority is '" + context.state.authority.str() + "'";
    decision.evidence.push_back(EntityRef::global());
    return decision;
  }
  if (!(binding.authority_generation == context.state.authority_generation)) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::authority;
    decision.detail_code = "authority-generation-changed";
    decision.message = "plan was computed under authority generation " +
                       std::to_string(binding.authority_generation.value()) +
                       " but the current authority generation is " +
                       std::to_string(context.state.authority_generation.value());
    decision.evidence.push_back(EntityRef::global());
    return decision;
  }
  if (!(binding.boot_incarnation == context.state.boot_incarnation)) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::incarnation;
    decision.detail_code = "incarnation-changed";
    decision.message = "plan was computed under boot incarnation " +
                       std::to_string(binding.boot_incarnation.value()) +
                       " but the current incarnation is " +
                       std::to_string(context.state.boot_incarnation.value());
    decision.evidence.push_back(EntityRef::global());
    return decision;
  }
  if (!(binding.epoch == context.state.epoch)) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::epoch;
    decision.detail_code = "epoch-changed";
    decision.message = "plan was computed in epoch " +
                       std::to_string(binding.epoch.value()) + " but the current epoch is " +
                       std::to_string(context.state.epoch.value());
    decision.evidence.push_back(EntityRef::global());
    return decision;
  }
  if (!(binding.constraint_digest == context.constraints.digest())) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::constraint_policy;
    decision.detail_code = "governing-policy-changed";
    decision.message =
        "the governing safety constraints changed since the plan was computed; no tolerance "
        "applies to policy changes";
    decision.evidence.push_back(EntityRef::global());
    return decision;
  }

  // Capability drift.
  if (context.evidence.capability_version.value() > binding.capability_version.value() &&
      !context.constraints.tolerance.allow_capability_growth) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::capability;
    decision.detail_code = "capability-growth-not-tolerated";
    decision.message = "capability version advanced from " +
                       std::to_string(binding.capability_version.value()) + " to " +
                       std::to_string(context.evidence.capability_version.value());
    decision.evidence.push_back(EntityRef::global());
    return decision;
  }
  if (context.evidence.capability_version.value() < binding.capability_version.value() &&
      !context.constraints.tolerance.allow_capability_shrink) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::capability;
    decision.detail_code = "capability-shrunk";
    decision.message = "capability version regressed from " +
                       std::to_string(binding.capability_version.value()) + " to " +
                       std::to_string(context.evidence.capability_version.value());
    decision.evidence.push_back(EntityRef::global());
    return decision;
  }

  // Topology drift.
  const TopologySummary current = summarize(context.state);
  decision.topology_drift = permille_change(binding.topology.entities.size(),
                                            current.entities.size());
  decision.capacity_drift =
      permille_change(binding.topology.total_link_capacity, current.total_link_capacity);
  decision.demand_drift =
      permille_change(binding.topology.total_active_demand, current.total_active_demand);

  std::uint32_t added = 0;
  std::uint32_t removed = 0;
  std::uint32_t changed = 0;
  for (const EntityFingerprint& entry : current.entities) {
    if (exempt(context.constraints.tolerance, entry.reference)) {
      continue;
    }
    const EntityFingerprint* previous = binding.topology.find(entry.reference);
    if (previous == nullptr) {
      ++added;
      decision.evidence.push_back(entry.reference);
    } else if (!(previous->content == entry.content)) {
      ++changed;
      decision.evidence.push_back(entry.reference);
    }
  }
  for (const EntityFingerprint& entry : binding.topology.entities) {
    if (exempt(context.constraints.tolerance, entry.reference)) {
      continue;
    }
    if (current.find(entry.reference) == nullptr) {
      ++removed;
      decision.evidence.push_back(entry.reference);
    }
  }
  decision.added_entities = added;
  decision.removed_entities = removed;
  decision.changed_entities = changed;

  const ChangeTolerance& tolerance = context.constraints.tolerance;
  if (added > tolerance.max_added_entities) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::topology;
    decision.detail_code = "entities-added-beyond-tolerance";
    decision.message = std::to_string(added) +
                       " entities appeared since the plan was computed, above the declared "
                       "tolerance of " + std::to_string(tolerance.max_added_entities);
    return decision;
  }
  if (removed > tolerance.max_removed_entities) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::topology;
    decision.detail_code = "entities-removed-beyond-tolerance";
    decision.message = std::to_string(removed) +
                       " entities disappeared since the plan was computed, above the declared "
                       "tolerance of " + std::to_string(tolerance.max_removed_entities);
    return decision;
  }
  if (decision.capacity_drift.value() > tolerance.capacity_drift.value()) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::topology;
    decision.detail_code = "capacity-drift-beyond-tolerance";
    decision.message = "capacity drifted by " +
                       std::to_string(decision.capacity_drift.value()) +
                       " permille which exceeds the declared tolerance of " +
                       std::to_string(tolerance.capacity_drift.value()) + " permille";
    return decision;
  }
  if (decision.demand_drift.value() > tolerance.demand_drift.value()) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::topology;
    decision.detail_code = "demand-drift-beyond-tolerance";
    decision.message = "active demand drifted by " +
                       std::to_string(decision.demand_drift.value()) +
                       " permille which exceeds the declared tolerance of " +
                       std::to_string(tolerance.demand_drift.value()) + " permille";
    return decision;
  }
  if (changed > 0 && tolerance.capacity_drift.value() == 0 &&
      tolerance.max_added_entities == 0 && tolerance.max_removed_entities == 0) {
    decision.valid = false;
    decision.dimension = InvalidationDimension::topology;
    decision.detail_code = "entities-changed-beyond-tolerance";
    decision.message = std::to_string(changed) +
                       " entities changed since the plan was computed and the declared tolerance "
                       "permits no drift";
    return decision;
  }

  // Target intent drift.
  if (!(binding.target_digest == context.target.digest())) {
    if (context.target.revision == binding.target_revision) {
      decision.valid = false;
      decision.dimension = InvalidationDimension::target_intent;
      decision.detail_code = "target-content-changed";
      decision.message =
          "the target intent content changed without a revision bump; the plan is refused";
      decision.evidence.push_back(EntityRef::global());
      return decision;
    }
    decision.valid = false;
    decision.dimension = InvalidationDimension::target_intent;
    decision.detail_code = "target-revision-changed";
    decision.message = "the target intent advanced from revision " +
                       std::to_string(binding.target_revision.value()) + " to " +
                       std::to_string(context.target.revision.value()) +
                       "; the plan is refused and must be regenerated";
    decision.evidence.push_back(EntityRef::global());
    return decision;
  }

  // Planning instant drift: a step bound to a maintenance or exclusion window
  // that no longer contains the plan instant cannot be executed as planned.
  for (const Step& step : plan.steps) {
    if (step.windows.empty()) {
      continue;
    }
    const Device* device = context.state.find_device(step.operation.device);
    if (device == nullptr) {
      decision.valid = false;
      decision.dimension = InvalidationDimension::topology;
      decision.detail_code = "step-target-missing";
      decision.message = "step '" + step.id.str() + "' targets a device that no longer exists";
      decision.evidence.push_back(EntityRef::of(step.operation.device));
      return decision;
    }
    if (!change_permitted_at(*device, context.planning_instant, nullptr)) {
      decision.valid = false;
      decision.dimension = InvalidationDimension::planning_instant;
      decision.detail_code = "exclusion-window-now-covers-step";
      decision.message = "device '" + device->id.str() +
                         "' is now inside an exclusion window at the planning instant";
      decision.evidence.push_back(EntityRef::of(device->id));
      return decision;
    }
    bool open = false;
    for (const WindowId& window_id : step.windows) {
      for (const MaintenanceWindow& window : device->maintenance_windows) {
        if (window.id == window_id && window.contains(context.planning_instant)) {
          open = true;
          break;
        }
      }
      if (open) {
        break;
      }
    }
    if (!open) {
      decision.valid = false;
      decision.dimension = InvalidationDimension::planning_instant;
      decision.detail_code = "maintenance-window-closed";
      decision.message = "step '" + step.id.str() +
                         "' was bound to a maintenance window that is not open at the planning "
                         "instant";
      decision.evidence.push_back(EntityRef::of(device->id));
      return decision;
    }
  }

  std::sort(decision.evidence.begin(), decision.evidence.end());
  decision.evidence.erase(std::unique(decision.evidence.begin(), decision.evidence.end()),
                          decision.evidence.end());
  if (decision.valid && !decision.evidence.empty()) {
    decision.detail_code = "within-declared-tolerance";
    decision.message = "topology drifted within the declared tolerances";
  }
  return decision;
}

}  // namespace cplan
