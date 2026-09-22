#include "change_planner/plan/graph.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "change_planner/core/checked.hpp"
#include "change_planner/model/evaluate.hpp"

namespace cplan {
namespace {

bool drain_before_maintenance(const EvaluationContext& context) {
  return context.constraints != nullptr &&
         context.constraints->enforced(ConstraintKind::drain_before_maintenance);
}

Status target_conflict(const std::string& detail) {
  return Status::error(ErrorCode::unsupported_capability, detail);
}

Operation make_device_operation(OperationKind kind, const DeviceId& device) {
  Operation operation;
  operation.kind = kind;
  operation.device = device;
  return operation;
}

Operation make_device_add(const DesiredDevice& desired) {
  Operation operation;
  operation.kind = OperationKind::device_add;
  operation.device = desired.id;
  operation.device_spec = desired.spec;
  operation.exclusions = desired.exclusions;
  operation.maintenance_windows = desired.maintenance_windows;
  operation.reason_code = "target-ensure";
  return operation;
}

Operation make_device_capacity(const Device& current, const DeviceSpec& desired) {
  Operation operation;
  operation.kind = OperationKind::device_set_capacity;
  operation.device = current.id;
  operation.capacity = desired.termination_capacity;
  operation.device_spec.termination_reserved = desired.termination_reserved;
  operation.reason_code = "target-capacity";
  return operation;
}

Operation make_device_windows(const DeviceId& id, const std::vector<ExclusionWindow>& exclusions,
                              const std::vector<MaintenanceWindow>& maintenance_windows) {
  Operation operation;
  operation.kind = OperationKind::device_set_windows;
  operation.device = id;
  operation.exclusions = exclusions;
  operation.maintenance_windows = maintenance_windows;
  operation.reason_code = "target-windows";
  return operation;
}

Operation make_link_add(const DesiredLink& desired) {
  Operation operation;
  operation.kind = OperationKind::link_add;
  operation.link = desired.id;
  operation.link_spec = desired.spec;
  operation.reason_code = "target-ensure";
  return operation;
}

Operation make_link_capacity(const Link& current, const LinkSpec& desired) {
  Operation operation;
  operation.kind = OperationKind::link_set_capacity;
  operation.link = current.id;
  operation.link_spec = desired;
  operation.capacity = desired.capacity;
  operation.reason_code = "target-capacity";
  return operation;
}

Operation make_route_add(const DesiredRoute& desired) {
  Operation operation;
  operation.kind = OperationKind::route_add;
  operation.route = desired.id;
  operation.route_spec = desired.spec;
  operation.route_state = desired.state;
  operation.reason_code = "target-ensure";
  return operation;
}

Operation make_route_path(const Route& current, const RouteSpec& desired) {
  Operation operation;
  operation.kind = OperationKind::route_set_path;
  operation.route = current.id;
  operation.route_spec = desired;
  operation.reason_code = "target-path";
  return operation;
}

Operation make_route_state(const Route& current, RouteState desired) {
  Operation operation;
  operation.kind = OperationKind::route_set_state;
  operation.route = current.id;
  operation.route_state = desired;
  operation.reason_code = "target-state";
  return operation;
}

Operation make_workload_add(const DesiredWorkload& desired) {
  Operation operation;
  operation.kind = OperationKind::workload_add;
  operation.workload = desired.id;
  operation.bindings = desired.bindings;
  operation.requirements = desired.requirements;
  operation.contract_code = desired.contract_code;
  operation.reason_code = "target-ensure";
  return operation;
}

Operation make_workload_bindings(const WorkloadId& id, std::vector<RouteBinding> bindings) {
  Operation operation;
  operation.kind = OperationKind::workload_set_bindings;
  operation.workload = id;
  operation.bindings = std::move(bindings);
  operation.reason_code = "target-bindings";
  return operation;
}

Operation make_workload_requirements(const DesiredWorkload& desired) {
  Operation operation;
  operation.kind = OperationKind::workload_set_requirements;
  operation.workload = desired.id;
  operation.requirements = desired.requirements;
  operation.contract_code = desired.contract_code;
  operation.reason_code = "target-requirements";
  return operation;
}

Operation make_policy(const DesiredPolicy& desired) {
  Operation operation;
  operation.kind = OperationKind::policy_bind;
  operation.policy = desired.id;
  operation.policy_target = desired.target;
  operation.value_code = desired.value_code;
  operation.enforced = desired.enforced;
  operation.reason_code = "target-ensure";
  return operation;
}

Operation make_config(const DesiredConfig& desired) {
  Operation operation;
  operation.kind = OperationKind::config_set;
  operation.device = desired.device;
  operation.config_key = desired.key;
  operation.value_code = desired.value_code;
  operation.reason_code = "target-ensure";
  return operation;
}

bool device_declared(const TargetIntent& target, const DeviceId& id) {
  for (const DesiredDevice& desired : target.devices) {
    if (desired.id == id && desired.action == DesiredAction::ensure) {
      return true;
    }
  }
  return false;
}

bool link_declared(const TargetIntent& target, const LinkId& id) {
  for (const DesiredLink& desired : target.links) {
    if (desired.id == id && desired.action == DesiredAction::ensure) {
      return true;
    }
  }
  return false;
}

bool route_declared(const TargetIntent& target, const RouteId& id) {
  for (const DesiredRoute& desired : target.routes) {
    if (desired.id == id && desired.action == DesiredAction::ensure) {
      return true;
    }
  }
  return false;
}

bool has_device(const CurrentStateSnapshot& state, const TargetIntent& target, const DeviceId& id) {
  const Device* device = state.find_device(id);
  if (device != nullptr && device->spec.state != EntityState::removed) {
    return true;
  }
  return device_declared(target, id);
}

bool has_link(const CurrentStateSnapshot& state, const TargetIntent& target, const LinkId& id) {
  const Link* link = state.find_link(id);
  if (link != nullptr && link->state != EntityState::removed) {
    return true;
  }
  return link_declared(target, id);
}

bool has_route(const CurrentStateSnapshot& state, const TargetIntent& target, const RouteId& id) {
  if (state.find_route(id) != nullptr) {
    return true;
  }
  return route_declared(target, id);
}

void append(std::vector<Operation>& operations, Operation operation) {
  operations.push_back(std::move(operation));
}

// Two operations that update disjoint fields of the same entity commute with
// respect to the final state, so no static ordering edge is imposed: the
// safety-checked search decides which order (if any) is legal.
bool independent_field_updates(const Operation& lhs, const Operation& rhs) {
  const auto is_kind = [](const Operation& operation, OperationKind kind) {
    return operation.kind == kind;
  };
  if (!lhs.device.empty() && lhs.device == rhs.device) {
    if ((is_kind(lhs, OperationKind::device_set_capacity) &&
         is_kind(rhs, OperationKind::device_set_windows)) ||
        (is_kind(lhs, OperationKind::device_set_windows) &&
         is_kind(rhs, OperationKind::device_set_capacity))) {
      return true;
    }
  }
  if (!lhs.workload.empty() && lhs.workload == rhs.workload) {
    if ((is_kind(lhs, OperationKind::workload_set_bindings) &&
         is_kind(rhs, OperationKind::workload_set_requirements)) ||
        (is_kind(lhs, OperationKind::workload_set_requirements) &&
         is_kind(rhs, OperationKind::workload_set_bindings))) {
      return true;
    }
  }
  if (!lhs.route.empty() && lhs.route == rhs.route) {
    if ((is_kind(lhs, OperationKind::route_set_path) &&
         is_kind(rhs, OperationKind::route_set_state)) ||
        (is_kind(lhs, OperationKind::route_set_state) &&
         is_kind(rhs, OperationKind::route_set_path))) {
      return true;
    }
  }
  return false;
}

bool entity_out_of_service_transition(const CurrentStateSnapshot& state, const Operation& operation,
                                      FailureDomainId* domain) {
  switch (operation.kind) {
    case OperationKind::drain_resource:
    case OperationKind::enter_maintenance:
    case OperationKind::device_remove: {
      const Device* device = state.find_device(operation.device);
      if (device == nullptr) {
        return false;
      }
      *domain = device->spec.domain;
      return true;
    }
    case OperationKind::device_set_state: {
      if (operation.entity_state != EntityState::draining &&
          operation.entity_state != EntityState::in_maintenance) {
        return false;
      }
      const Device* device = state.find_device(operation.device);
      if (device == nullptr) {
        return false;
      }
      *domain = device->spec.domain;
      return true;
    }
    default:
      return false;
  }
}

// Failure domains a step disturbs in a way that removes service capacity. Only
// changes that take a device out of service consume the per-domain change
// budget; an in-place capacity change does not.
std::vector<FailureDomainId> touched_domains(const CurrentStateSnapshot& state,
                                             const Operation& operation) {
  std::vector<FailureDomainId> domains;
  FailureDomainId domain;
  if (entity_out_of_service_transition(state, operation, &domain)) {
    domains.push_back(domain);
  }
  std::sort(domains.begin(), domains.end());
  domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
  return domains;
}

}  // namespace

StepId step_id_for(const Operation& operation) {
  return StepId::from_validated("step-" + operation.identity_digest().hex().substr(0, 24));
}

RiskScore derive_risk(const Operation& operation) {
  std::uint32_t base = 10;
  switch (operation.kind) {
    case OperationKind::config_set:
    case OperationKind::config_clear:
    case OperationKind::policy_bind:
    case OperationKind::policy_unbind:
      base = 10;
      break;
    case OperationKind::workload_set_bindings:
    case OperationKind::workload_set_requirements:
    case OperationKind::workload_add:
    case OperationKind::workload_remove:
      base = 25;
      break;
    case OperationKind::device_set_capacity:
    case OperationKind::link_set_capacity:
      base = 30;
      break;
    case OperationKind::route_set_state:
    case OperationKind::route_set_path:
    case OperationKind::route_add:
    case OperationKind::route_remove:
      base = 40;
      break;
    case OperationKind::link_add:
    case OperationKind::link_remove:
      base = 50;
      break;
    case OperationKind::device_set_state:
    case OperationKind::device_set_windows:
      base = 60;
      break;
    case OperationKind::drain_resource:
      base = 65;
      break;
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance:
      base = 70;
      break;
    case OperationKind::device_add:
    case OperationKind::device_remove:
      base = 80;
      break;
  }
  return RiskScore(base + operation.risk.value());
}

DurationNs derive_duration(const Operation& operation) {
  if (operation.estimated_duration.value() != 0) {
    return operation.estimated_duration;
  }
  switch (operation.kind) {
    case OperationKind::config_set:
    case OperationKind::config_clear:
    case OperationKind::policy_bind:
    case OperationKind::policy_unbind:
      return DurationNs(1000000000ull);
    case OperationKind::workload_set_bindings:
    case OperationKind::workload_set_requirements:
    case OperationKind::workload_add:
    case OperationKind::workload_remove:
      return DurationNs(5000000000ull);
    case OperationKind::route_set_state:
    case OperationKind::route_set_path:
    case OperationKind::route_add:
    case OperationKind::route_remove:
      return DurationNs(10000000000ull);
    case OperationKind::device_set_capacity:
    case OperationKind::link_set_capacity:
    case OperationKind::link_add:
    case OperationKind::link_remove:
      return DurationNs(30000000000ull);
    case OperationKind::device_set_state:
    case OperationKind::device_set_windows:
      return DurationNs(60000000000ull);
    case OperationKind::drain_resource:
      return DurationNs(120000000000ull);
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance:
      return DurationNs(300000000000ull);
    case OperationKind::device_add:
    case OperationKind::device_remove:
      return DurationNs(600000000000ull);
  }
  return DurationNs(1000000000ull);
}

Compensation derive_compensation(const CurrentStateSnapshot& state, const Operation& operation,
                                 const EvaluationContext& context) {
  static_cast<void>(context);
  Compensation compensation;
  compensation.kind = CompensationKind::inverse;
  compensation.available = true;
  compensation.inverse.kind = operation.kind;
  compensation.inverse.device = operation.device;
  compensation.inverse.link = operation.link;
  compensation.inverse.route = operation.route;
  compensation.inverse.workload = operation.workload;
  compensation.inverse.policy = operation.policy;
  compensation.inverse.config_key = operation.config_key;

  switch (operation.kind) {
    case OperationKind::device_add:
      compensation.inverse.kind = OperationKind::device_remove;
      compensation.note_code = "remove the added device";
      break;
    case OperationKind::device_remove: {
      const Device* device = state.find_device(operation.device);
      if (device == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "device state unknown; re-add requires operator input";
        break;
      }
      compensation.inverse.kind = OperationKind::device_add;
      compensation.inverse.device_spec = device->spec;
      compensation.inverse.exclusions = device->exclusions;
      compensation.inverse.maintenance_windows = device->maintenance_windows;
      compensation.note_code = "re-add the removed device";
      break;
    }
    case OperationKind::device_set_capacity: {
      const Device* device = state.find_device(operation.device);
      if (device == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous capacity unknown";
        break;
      }
      compensation.inverse.capacity = device->spec.termination_capacity;
      compensation.inverse.device_spec.termination_reserved = device->spec.termination_reserved;
      compensation.note_code = "restore the previous termination capacity";
      break;
    }
    case OperationKind::device_set_state:
    case OperationKind::drain_resource:
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance: {
      const Device* device = state.find_device(operation.device);
      if (device == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous device state unknown";
        break;
      }
      compensation.inverse.kind = OperationKind::device_set_state;
      compensation.inverse.entity_state = device->spec.state;
      compensation.note_code = "restore the previous device state";
      break;
    }
    case OperationKind::device_set_windows: {
      const Device* device = state.find_device(operation.device);
      if (device == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous windows unknown";
        break;
      }
      compensation.inverse.exclusions = device->exclusions;
      compensation.inverse.maintenance_windows = device->maintenance_windows;
      compensation.note_code = "restore the previous window configuration";
      break;
    }
    case OperationKind::link_add:
      compensation.inverse.kind = OperationKind::link_remove;
      compensation.note_code = "remove the added link";
      break;
    case OperationKind::link_remove: {
      const Link* link = state.find_link(operation.link);
      if (link == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous link configuration unknown";
        break;
      }
      compensation.inverse.kind = OperationKind::link_add;
      compensation.inverse.link_spec = link->spec;
      compensation.note_code = "re-add the removed link";
      break;
    }
    case OperationKind::link_set_capacity: {
      const Link* link = state.find_link(operation.link);
      if (link == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous link capacity unknown";
        break;
      }
      compensation.inverse.link_spec = link->spec;
      compensation.inverse.capacity = link->spec.capacity;
      compensation.note_code = "restore the previous link capacity";
      break;
    }
    case OperationKind::route_add:
      compensation.inverse.kind = OperationKind::route_remove;
      compensation.note_code = "remove the added route";
      break;
    case OperationKind::route_remove: {
      const Route* route = state.find_route(operation.route);
      if (route == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous route configuration unknown";
        break;
      }
      compensation.inverse.kind = OperationKind::route_add;
      compensation.inverse.route_spec = route->spec;
      compensation.inverse.route_state = route->state;
      compensation.note_code = "re-add the removed route";
      break;
    }
    case OperationKind::route_set_path:
    case OperationKind::route_set_state: {
      const Route* route = state.find_route(operation.route);
      if (route == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous route configuration unknown";
        break;
      }
      compensation.inverse.kind = OperationKind::route_set_path;
      compensation.inverse.route_spec = route->spec;
      compensation.inverse.route_state = route->state;
      compensation.note_code = "restore the previous route configuration";
      break;
    }
    case OperationKind::workload_add:
      compensation.inverse.kind = OperationKind::workload_remove;
      compensation.note_code = "remove the added workload binding set";
      break;
    case OperationKind::workload_remove: {
      const Workload* workload = state.find_workload(operation.workload);
      if (workload == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous workload configuration unknown";
        break;
      }
      compensation.inverse.kind = OperationKind::workload_add;
      compensation.inverse.bindings = workload->bindings;
      compensation.inverse.requirements = workload->requirements;
      compensation.inverse.contract_code = workload->contract_code;
      compensation.note_code = "restore the removed workload";
      break;
    }
    case OperationKind::workload_set_bindings:
    case OperationKind::workload_set_requirements: {
      const Workload* workload = state.find_workload(operation.workload);
      if (workload == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous workload state unknown";
        break;
      }
      compensation.inverse.bindings = workload->bindings;
      compensation.inverse.requirements = workload->requirements;
      compensation.inverse.contract_code = workload->contract_code;
      compensation.note_code = "restore the previous workload configuration";
      break;
    }
    case OperationKind::policy_bind: {
      const PolicyObject* existing = state.find_policy(operation.policy);
      if (existing == nullptr) {
        compensation.inverse.kind = OperationKind::policy_unbind;
        compensation.note_code = "unbind the newly bound policy";
        break;
      }
      compensation.inverse.policy_target = existing->target;
      compensation.inverse.value_code = existing->value_code;
      compensation.inverse.enforced = existing->enforced;
      compensation.note_code = "restore the previous policy value";
      break;
    }
    case OperationKind::policy_unbind: {
      const PolicyObject* existing = state.find_policy(operation.policy);
      if (existing == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous policy value unknown";
        break;
      }
      compensation.inverse.kind = OperationKind::policy_bind;
      compensation.inverse.policy_target = existing->target;
      compensation.inverse.value_code = existing->value_code;
      compensation.inverse.enforced = existing->enforced;
      compensation.note_code = "restore the unbound policy";
      break;
    }
    case OperationKind::config_set: {
      const ConfigEntry* existing = state.find_config(operation.device, operation.config_key);
      if (existing == nullptr) {
        compensation.inverse.kind = OperationKind::config_clear;
        compensation.note_code = "clear the newly set configuration value";
        break;
      }
      compensation.inverse.value_code = existing->value_code;
      compensation.note_code = "restore the previous configuration value";
      break;
    }
    case OperationKind::config_clear: {
      const ConfigEntry* existing = state.find_config(operation.device, operation.config_key);
      if (existing == nullptr) {
        compensation.available = false;
        compensation.kind = CompensationKind::manual;
        compensation.note_code = "previous configuration value unknown";
        break;
      }
      compensation.inverse.kind = OperationKind::config_set;
      compensation.inverse.value_code = existing->value_code;
      compensation.note_code = "restore the cleared configuration value";
      break;
    }
  }
  return compensation;
}

Result<DerivedOperations> derive_operations(const CurrentStateSnapshot& state,
                                            const TargetIntent& target,
                                            const EvaluationContext& context) {
  DerivedOperations result;

  for (const DesiredDevice& desired : target.devices) {
    const Device* current = state.find_device(desired.id);
    const bool exists = current != nullptr && current->spec.state != EntityState::removed;
    if (desired.action == DesiredAction::remove) {
      if (!exists) {
        result.notes.push_back("device '" + desired.id.str() + "' already absent");
        continue;
      }
      if (drain_before_maintenance(context) && current->spec.state == EntityState::present) {
        append(result.operations, make_device_operation(OperationKind::drain_resource, desired.id));
      }
      append(result.operations, make_device_operation(OperationKind::device_remove, desired.id));
      continue;
    }
    if (!exists) {
      if (current != nullptr) {
        return target_conflict("device '" + desired.id.str() +
                               "' is present in state 'removed' and cannot be re-added under the "
                               "same identity; express the change with a new identity");
      }
      append(result.operations, make_device_add(desired));
      continue;
    }
    if (!(current->spec.domain == desired.spec.domain)) {
      return target_conflict("device '" + desired.id.str() +
                             "' cannot change failure domain in place: the vendor-neutral "
                             "operation set requires remove plus add, which the target does not "
                             "express");
    }
    if (current->spec.profile_code != desired.spec.profile_code) {
      return target_conflict("device '" + desired.id.str() +
                             "' cannot change its profile in place");
    }
    if (!(current->spec.capabilities == desired.spec.capabilities)) {
      return target_conflict("device '" + desired.id.str() +
                             "' cannot change its declared capability set in place; capability "
                             "changes are evidenced, not planned");
    }
    if (current->spec.max_concurrent_changes != desired.spec.max_concurrent_changes) {
      return target_conflict("device '" + desired.id.str() +
                             "' cannot change its concurrency budget in place");
    }
    if (!(current->spec.termination_capacity == desired.spec.termination_capacity) ||
        !(current->spec.termination_reserved == desired.spec.termination_reserved)) {
      append(result.operations, make_device_capacity(*current, desired.spec));
    }
    if (!(current->exclusions == desired.exclusions) ||
        !(current->maintenance_windows == desired.maintenance_windows)) {
      append(result.operations,
             make_device_windows(desired.id, desired.exclusions, desired.maintenance_windows));
    }

    const EntityState wanted = desired.spec.state;
    if (wanted != current->spec.state) {
      switch (wanted) {
        case EntityState::present:
          if (current->spec.state == EntityState::draining ||
              current->spec.state == EntityState::in_maintenance) {
            append(result.operations,
                   make_device_operation(OperationKind::exit_maintenance, desired.id));
          }
          break;
        case EntityState::draining:
          if (current->spec.state == EntityState::in_maintenance) {
            append(result.operations,
                   make_device_operation(OperationKind::exit_maintenance, desired.id));
          }
          append(result.operations,
                 make_device_operation(OperationKind::drain_resource, desired.id));
          break;
        case EntityState::in_maintenance:
          if (current->spec.state == EntityState::present) {
            append(result.operations,
                   make_device_operation(OperationKind::drain_resource, desired.id));
          }
          append(result.operations,
                 make_device_operation(OperationKind::enter_maintenance, desired.id));
          break;
        case EntityState::removed:
        case EntityState::absent:
          if (drain_before_maintenance(context) && current->spec.state == EntityState::present) {
            append(result.operations,
                   make_device_operation(OperationKind::drain_resource, desired.id));
          }
          append(result.operations, make_device_operation(OperationKind::device_remove, desired.id));
          break;
      }
    }
  }

  for (const DesiredLink& desired : target.links) {
    const Link* current = state.find_link(desired.id);
    const bool exists = current != nullptr && current->state != EntityState::removed;
    if (desired.action == DesiredAction::remove) {
      if (!exists) {
        result.notes.push_back("link '" + desired.id.str() + "' already absent");
        continue;
      }
      Operation operation;
      operation.kind = OperationKind::link_remove;
      operation.link = desired.id;
      operation.reason_code = "target-remove";
      append(result.operations, std::move(operation));
      continue;
    }
    if (!exists) {
      if (current != nullptr) {
        return target_conflict("link '" + desired.id.str() +
                               "' is present in state 'removed' and cannot be re-added under the "
                               "same identity");
      }
      if (!has_device(state, target, desired.spec.endpoint_a)) {
        return Status::error(ErrorCode::invalid_argument,
                             "target link '" + desired.id.str() + "' references device '" +
                                 desired.spec.endpoint_a.str() +
                                 "' which is neither present nor created by the target intent");
      }
      if (!has_device(state, target, desired.spec.endpoint_b)) {
        return Status::error(ErrorCode::invalid_argument,
                             "target link '" + desired.id.str() + "' references device '" +
                                 desired.spec.endpoint_b.str() +
                                 "' which is neither present nor created by the target intent");
      }
      append(result.operations, make_link_add(desired));
      continue;
    }
    if (!(current->spec.endpoint_a == desired.spec.endpoint_a) ||
        !(current->spec.endpoint_b == desired.spec.endpoint_b)) {
      return target_conflict("link '" + desired.id.str() +
                             "' cannot change its endpoints in place");
    }
    if (current->spec.latency_class != desired.spec.latency_class) {
      return target_conflict("link '" + desired.id.str() + "' cannot change its latency class");
    }
    if (!(current->spec.transit_domains == desired.spec.transit_domains)) {
      return target_conflict("link '" + desired.id.str() + "' cannot change its transit domains");
    }
    if (!(current->spec.capacity == desired.spec.capacity) ||
        !(current->spec.reserved == desired.spec.reserved)) {
      append(result.operations, make_link_capacity(*current, desired.spec));
    }
  }

  for (const DesiredRoute& desired : target.routes) {
    const Route* current = state.find_route(desired.id);
    if (desired.action == DesiredAction::remove) {
      if (current == nullptr) {
        result.notes.push_back("route '" + desired.id.str() + "' already absent");
        continue;
      }
      for (const Workload& workload : state.workloads) {
        std::vector<RouteBinding> remaining;
        bool bound = false;
        for (const RouteBinding& binding : workload.bindings) {
          if (binding.route == desired.id) {
            bound = true;
            continue;
          }
          remaining.push_back(binding);
        }
        if (bound) {
          append(result.operations, make_workload_bindings(workload.id, std::move(remaining)));
        }
      }
      Operation operation;
      operation.kind = OperationKind::route_remove;
      operation.route = desired.id;
      operation.reason_code = "target-remove";
      append(result.operations, std::move(operation));
      continue;
    }
    if (current == nullptr) {
      if (!has_device(state, target, desired.spec.source) ||
          !has_device(state, target, desired.spec.sink)) {
        return Status::error(ErrorCode::invalid_argument,
                             "target route '" + desired.id.str() +
                                 "' references an endpoint device that is neither present nor "
                                 "created by the target intent");
      }
      for (const LinkId& link_id : desired.spec.path) {
        if (!has_link(state, target, link_id)) {
          return Status::error(ErrorCode::invalid_argument,
                               "target route '" + desired.id.str() + "' references link '" +
                                   link_id.str() +
                                   "' which is neither present nor created by the target intent");
        }
      }
      append(result.operations, make_route_add(desired));
      continue;
    }
    if (!(current->spec.source == desired.spec.source) ||
        !(current->spec.sink == desired.spec.sink)) {
      return target_conflict("route '" + desired.id.str() +
                             "' cannot change its endpoints in place");
    }
    if (!(current->spec.path == desired.spec.path) || !(current->spec.policy == desired.spec.policy)) {
      for (const LinkId& link_id : desired.spec.path) {
        if (!has_link(state, target, link_id)) {
          return Status::error(ErrorCode::invalid_argument,
                               "target route '" + desired.id.str() + "' references link '" +
                                   link_id.str() +
                                   "' which is neither present nor created by the target intent");
        }
      }
      append(result.operations, make_route_path(*current, desired.spec));
    }
    if (!(current->state == desired.state)) {
      append(result.operations, make_route_state(*current, desired.state));
    }
  }

  for (const DesiredWorkload& desired : target.workloads) {
    const Workload* current = state.find_workload(desired.id);
    if (desired.action == DesiredAction::remove) {
      if (current == nullptr) {
        result.notes.push_back("workload '" + desired.id.str() + "' already absent");
        continue;
      }
      Operation operation;
      operation.kind = OperationKind::workload_remove;
      operation.workload = desired.id;
      operation.reason_code = "target-remove";
      append(result.operations, std::move(operation));
      continue;
    }
    if (current == nullptr) {
      for (const RouteBinding& binding : desired.bindings) {
        if (!has_route(state, target, binding.route)) {
          return Status::error(ErrorCode::invalid_argument,
                               "target workload '" + desired.id.str() + "' binds route '" +
                                   binding.route.str() +
                                   "' which is neither present nor created by the target intent");
        }
      }
      append(result.operations, make_workload_add(desired));
      continue;
    }
    if (!(current->bindings == desired.bindings)) {
      append(result.operations, make_workload_bindings(desired.id, desired.bindings));
    }
    if (!(current->requirements == desired.requirements) ||
        current->contract_code != desired.contract_code) {
      append(result.operations, make_workload_requirements(desired));
    }
  }

  for (const DesiredPolicy& desired : target.policies) {
    const PolicyObject* current = state.find_policy(desired.id);
    if (desired.action == DesiredAction::remove) {
      if (current == nullptr) {
        result.notes.push_back("policy '" + desired.id.str() + "' already absent");
        continue;
      }
      Operation operation;
      operation.kind = OperationKind::policy_unbind;
      operation.policy = desired.id;
      operation.reason_code = "target-remove";
      append(result.operations, std::move(operation));
      continue;
    }
    if (current == nullptr || !(current->target == desired.target) ||
        current->value_code != desired.value_code || current->enforced != desired.enforced) {
      append(result.operations, make_policy(desired));
    }
  }

  for (const DesiredConfig& desired : target.configs) {
    const ConfigEntry* current = state.find_config(desired.device, desired.key);
    if (desired.action == DesiredAction::remove) {
      if (current == nullptr) {
        result.notes.push_back("config key '" + desired.key.str() + "' already absent");
        continue;
      }
      Operation operation;
      operation.kind = OperationKind::config_clear;
      operation.device = desired.device;
      operation.config_key = desired.key;
      operation.reason_code = "target-remove";
      append(result.operations, std::move(operation));
      continue;
    }
    if (!has_device(state, target, desired.device)) {
      return Status::error(ErrorCode::invalid_argument,
                           "target config entry for key '" + desired.key.str() +
                               "' references device '" + desired.device.str() +
                               "' which is neither present nor created by the target intent");
    }
    if (current == nullptr || current->value_code != desired.value_code) {
      append(result.operations, make_config(desired));
    }
  }

  // Deterministic canonical order. Semantic ordering is imposed by dependency
  // derivation and by the safety-checked scheduler, not by this sort.
  std::sort(result.operations.begin(), result.operations.end());
  {
    std::vector<Operation> unique;
    for (Operation& operation : result.operations) {
      if (!unique.empty() && unique.back() == operation) {
        continue;  // identical duplicate operations are emitted once
      }
      unique.push_back(std::move(operation));
    }
    result.operations = std::move(unique);
  }
  std::sort(result.notes.begin(), result.notes.end());
  return result;
}

Result<ConflictAnalysis> derive_dependencies(const CurrentStateSnapshot& state,
                                             const std::vector<Operation>& operations,
                                             const EvaluationContext& context) {
  ConflictAnalysis analysis;

  struct Node {
    const Operation* operation{nullptr};
    StepId id;
    std::vector<EntityRef> writes;
    std::vector<EntityRef> reads;
    std::vector<FailureDomainId> domains;
  };

  std::vector<Node> nodes;
  nodes.reserve(operations.size());
  for (const Operation& operation : operations) {
    Node node;
    node.operation = &operation;
    node.id = step_id_for(operation);
    node.writes = operation.write_set();
    node.reads = operation.read_set();
    node.domains = touched_domains(state, operation);
    nodes.push_back(std::move(node));
  }

  auto intersects = [](const std::vector<EntityRef>& lhs, const std::vector<EntityRef>& rhs) {
    for (const EntityRef& left : lhs) {
      if (std::find(rhs.begin(), rhs.end(), left) != rhs.end()) {
        return true;
      }
    }
    return false;
  };

  auto is_prerequisite = [&state](const Operation& first, const Operation& second) {
    if ((first.kind == OperationKind::drain_resource ||
         first.kind == OperationKind::device_set_state) &&
        first.device == second.device) {
      const bool first_removes_service = first.kind == OperationKind::drain_resource ||
                                         first.entity_state == EntityState::draining;
      if (first_removes_service &&
          (second.kind == OperationKind::device_remove ||
           second.kind == OperationKind::enter_maintenance)) {
        return true;
      }
    }
    if (first.kind == OperationKind::enter_maintenance &&
        second.kind == OperationKind::exit_maintenance && first.device == second.device) {
      return true;
    }
    if (first.kind == OperationKind::device_add) {
      if (!second.device.empty() && second.device == first.device) {
        return true;
      }
      if (second.kind == OperationKind::link_add &&
          (second.link_spec.endpoint_a == first.device ||
           second.link_spec.endpoint_b == first.device)) {
        return true;
      }
      if ((second.kind == OperationKind::route_add ||
           second.kind == OperationKind::route_set_path) &&
          (second.route_spec.source == first.device || second.route_spec.sink == first.device)) {
        return true;
      }
      if ((second.kind == OperationKind::config_set || second.kind == OperationKind::config_clear) &&
          second.device == first.device) {
        return true;
      }
      if ((second.kind == OperationKind::policy_bind ||
           second.kind == OperationKind::policy_unbind) &&
          second.policy_target.scope == PolicyScope::device &&
          second.policy_target.identity == first.device.str()) {
        return true;
      }
    }
    if (first.kind == OperationKind::link_add &&
        (second.kind == OperationKind::route_add ||
         second.kind == OperationKind::route_set_path)) {
      if (std::find(second.route_spec.path.begin(), second.route_spec.path.end(), first.link) !=
          second.route_spec.path.end()) {
        return true;
      }
    }
    if (first.kind == OperationKind::route_add &&
        (second.kind == OperationKind::workload_add ||
         second.kind == OperationKind::workload_set_bindings)) {
      for (const RouteBinding& binding : second.bindings) {
        if (binding.route == first.route) {
          return true;
        }
      }
    }
    if ((first.kind == OperationKind::workload_set_bindings ||
         first.kind == OperationKind::workload_set_requirements ||
         first.kind == OperationKind::workload_remove) &&
        second.kind == OperationKind::route_remove) {
      if (first.kind == OperationKind::workload_remove) {
        const Workload* workload = state.find_workload(first.workload);
        if (workload != nullptr) {
          for (const RouteBinding& binding : workload->bindings) {
            if (binding.route == second.route) {
              return true;
            }
          }
        }
        return false;
      }
      for (const RouteBinding& binding : first.bindings) {
        if (binding.route == second.route) {
          return false;  // still bound, so removal cannot proceed yet
        }
      }
      return true;
    }
    if (first.kind == OperationKind::workload_add && second.kind == OperationKind::route_remove) {
      return false;
    }
    return false;
  };

  for (std::size_t i = 0; i < nodes.size(); ++i) {
    for (std::size_t j = i + 1; j < nodes.size(); ++j) {
      const Operation& first = *nodes[i].operation;
      const Operation& second = *nodes[j].operation;
      DependencyEdge edge;
      edge.from = nodes[i].id;
      edge.to = nodes[j].id;

      if (is_prerequisite(first, second)) {
        edge.reason = DependencyReason::structural_prerequisite;
        edge.detail_code = std::string(to_string(first.kind)) + "-before-" +
                           std::string(to_string(second.kind));
        analysis.edges.push_back(edge);
        continue;
      }
      if (is_prerequisite(second, first)) {
        edge.from = nodes[j].id;
        edge.to = nodes[i].id;
        edge.reason = DependencyReason::structural_prerequisite;
        edge.detail_code = std::string(to_string(second.kind)) + "-before-" +
                           std::string(to_string(first.kind));
        analysis.edges.push_back(edge);
        continue;
      }
      if (independent_field_updates(first, second)) {
        continue;  // commuting field updates: the scheduler decides the order
      }
      if (intersects(nodes[i].writes, nodes[j].writes)) {
        edge.reason = DependencyReason::write_write_conflict;
        edge.detail_code = "same-entity-write";
        analysis.edges.push_back(edge);
        continue;
      }
      const bool i_reads_j = intersects(nodes[i].reads, nodes[j].writes);
      const bool j_reads_i = intersects(nodes[j].reads, nodes[i].writes);
      if (i_reads_j || j_reads_i) {
        edge.reason = DependencyReason::write_read_conflict;
        edge.detail_code = i_reads_j && j_reads_i ? "mutual-dependency" : "reads-written-entity";
        analysis.edges.push_back(edge);
        continue;
      }
      bool same_domain = false;
      for (const FailureDomainId& domain : nodes[i].domains) {
        if (std::find(nodes[j].domains.begin(), nodes[j].domains.end(), domain) !=
            nodes[j].domains.end()) {
          same_domain = true;
          break;
        }
      }
      if (same_domain) {
        edge.reason = DependencyReason::domain_serialization;
        edge.detail_code = "shared-failure-domain-out-of-service";
        analysis.edges.push_back(edge);
        continue;
      }
      if (context.constraints != nullptr) {
        bool contract_conflict = false;
        for (const NetworkContract& contract : context.constraints->contracts) {
          if (!contract.forbid_concurrent_changes_in_same_domain) {
            continue;
          }
          for (const FailureDomainId& domain : nodes[i].domains) {
            if (std::find(nodes[j].domains.begin(), nodes[j].domains.end(), domain) ==
                nodes[j].domains.end()) {
              continue;
            }
            for (const WorkloadId& workload_id : contract.workloads) {
              const Workload* workload = state.find_workload(workload_id);
              if (workload == nullptr) {
                continue;
              }
              for (const RouteBinding& binding : workload->bindings) {
                const Route* route = state.find_route(binding.route);
                if (route == nullptr) {
                  continue;
                }
                const std::vector<FailureDomainId> domains = route_domains(state, *route);
                if (std::find(domains.begin(), domains.end(), domain) != domains.end()) {
                  contract_conflict = true;
                  break;
                }
              }
              if (contract_conflict) {
                break;
              }
            }
            if (contract_conflict) {
              break;
            }
          }
          if (contract_conflict) {
            break;
          }
        }
        if (contract_conflict) {
          edge.reason = DependencyReason::contract_serialization;
          edge.detail_code = "contract-forbids-concurrent-domain-change";
          analysis.edges.push_back(edge);
          continue;
        }
      }
    }
  }

  std::sort(analysis.edges.begin(), analysis.edges.end());
  analysis.edges.erase(std::unique(analysis.edges.begin(), analysis.edges.end()),
                       analysis.edges.end());
  return analysis;
}

}  // namespace cplan
