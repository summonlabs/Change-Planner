#include "change_planner/model/evaluate.hpp"

#include <algorithm>

#include "change_planner/core/checked.hpp"

namespace cplan {
namespace {

Device* mutable_device(CurrentStateSnapshot& state, const DeviceId& id) {
  for (Device& device : state.devices) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

Link* mutable_link(CurrentStateSnapshot& state, const LinkId& id) {
  for (Link& link : state.links) {
    if (link.id == id) {
      return &link;
    }
  }
  return nullptr;
}

Route* mutable_route(CurrentStateSnapshot& state, const RouteId& id) {
  for (Route& route : state.routes) {
    if (route.id == id) {
      return &route;
    }
  }
  return nullptr;
}

Workload* mutable_workload(CurrentStateSnapshot& state, const WorkloadId& id) {
  for (Workload& workload : state.workloads) {
    if (workload.id == id) {
      return &workload;
    }
  }
  return nullptr;
}

PolicyObject* mutable_policy(CurrentStateSnapshot& state, const PolicyId& id) {
  for (PolicyObject& policy : state.policies) {
    if (policy.id == id) {
      return &policy;
    }
  }
  return nullptr;
}

ConfigEntry* mutable_config(CurrentStateSnapshot& state, const DeviceId& device,
                            const ConfigKey& key) {
  for (ConfigEntry& entry : state.configs) {
    if (entry.device == device && entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

bool drain_before_maintenance_enabled(const EvaluationContext& context) {
  return context.constraints != nullptr &&
         context.constraints->enforced(ConstraintKind::drain_before_maintenance);
}

Status not_found(const char* kind, const std::string& identity) {
  return Status::error(ErrorCode::not_found,
                       std::string(kind) + " '" + identity + "' does not exist");
}

Status conflict(const char* kind, const std::string& identity, const std::string& detail) {
  return Status::error(ErrorCode::conflict,
                       std::string(kind) + " '" + identity + "': " + detail);
}

Status impossible(const std::string& detail) {
  return Status::error(ErrorCode::unsafe_transition, detail);
}

}  // namespace

Result<AppliedOperation> apply_operation(const CurrentStateSnapshot& state,
                                         const Operation& operation,
                                         const EvaluationContext& context) {
  // Window gates come first: a change that is not permitted at the planning
  // instant must never reach the mutation below.
  if (context.constraints != nullptr &&
      context.constraints->enforced(ConstraintKind::maintenance_exclusion)) {
    for (const DeviceId& subject : operation.capability_subjects(state)) {
      const Device* device = state.find_device(subject);
      if (device == nullptr) {
        continue;
      }
      std::string reason;
      if (!change_permitted_at(*device, context.instant, &reason)) {
        return impossible(reason);
      }
    }
  }
  if (context.constraints != nullptr &&
      context.constraints->enforced(ConstraintKind::maintenance_window) &&
      operation.requires_maintenance_window() && !operation.device.empty()) {
    const Device* device = state.find_device(operation.device);
    if (device != nullptr) {
      std::string reason;
      if (!maintenance_permitted_at(*device, context.instant, &reason)) {
        return impossible(reason);
      }
    }
  }

  CurrentStateSnapshot next = state;

  switch (operation.kind) {
    case OperationKind::device_add: {
      if (next.find_device(operation.device) != nullptr) {
        return conflict("device", operation.device.str(), "already exists in the snapshot");
      }
      Device device;
      device.id = operation.device;
      device.spec = operation.device_spec;
      device.exclusions = operation.exclusions;
      device.maintenance_windows = operation.maintenance_windows;
      if (device.spec.state != EntityState::present &&
          device.spec.state != EntityState::draining &&
          device.spec.state != EntityState::in_maintenance) {
        return impossible("device '" + operation.device.str() +
                          "' cannot be added in a removed or absent state");
      }
      next.devices.push_back(std::move(device));
      break;
    }
    case OperationKind::device_remove: {
      Device* device = mutable_device(next, operation.device);
      if (device == nullptr) {
        return not_found("device", operation.device.str());
      }
      if (device->spec.state == EntityState::removed) {
        return conflict("device", operation.device.str(), "is already removed");
      }
      if (drain_before_maintenance_enabled(context) &&
          device->spec.state != EntityState::draining &&
          device->spec.state != EntityState::in_maintenance) {
        return impossible("device '" + operation.device.str() +
                          "' must be drained before removal (drain-before-maintenance policy)");
      }
      device->spec.state = EntityState::removed;
      break;
    }
    case OperationKind::device_set_capacity: {
      Device* device = mutable_device(next, operation.device);
      if (device == nullptr) {
        return not_found("device", operation.device.str());
      }
      device->spec.termination_capacity = operation.capacity;
      break;
    }
    case OperationKind::device_set_state: {
      Device* device = mutable_device(next, operation.device);
      if (device == nullptr) {
        return not_found("device", operation.device.str());
      }
      const EntityState target = operation.entity_state;
      if (target == EntityState::removed || target == EntityState::absent) {
        return impossible("device '" + operation.device.str() +
                          "' must be removed or added through the dedicated operation");
      }
      if (target == EntityState::in_maintenance && drain_before_maintenance_enabled(context) &&
          device->spec.state == EntityState::present) {
        return impossible("device '" + operation.device.str() +
                          "' must be drained before entering maintenance");
      }
      device->spec.state = target;
      break;
    }
    case OperationKind::device_set_windows: {
      Device* device = mutable_device(next, operation.device);
      if (device == nullptr) {
        return not_found("device", operation.device.str());
      }
      device->exclusions = operation.exclusions;
      device->maintenance_windows = operation.maintenance_windows;
      break;
    }
    case OperationKind::link_add: {
      if (next.find_link(operation.link) != nullptr) {
        return conflict("link", operation.link.str(), "already exists in the snapshot");
      }
      if (next.find_device(operation.link_spec.endpoint_a) == nullptr) {
        return not_found("device", operation.link_spec.endpoint_a.str());
      }
      if (next.find_device(operation.link_spec.endpoint_b) == nullptr) {
        return not_found("device", operation.link_spec.endpoint_b.str());
      }
      if (operation.link_spec.endpoint_a == operation.link_spec.endpoint_b) {
        return impossible("link '" + operation.link.str() + "' has identical endpoints");
      }
      Link link;
      link.id = operation.link;
      link.spec = operation.link_spec;
      link.state = EntityState::present;
      next.links.push_back(std::move(link));
      break;
    }
    case OperationKind::link_remove: {
      Link* link = mutable_link(next, operation.link);
      if (link == nullptr) {
        return not_found("link", operation.link.str());
      }
      if (link->state == EntityState::removed) {
        return conflict("link", operation.link.str(), "is already removed");
      }
      link->state = EntityState::removed;
      break;
    }
    case OperationKind::link_set_capacity: {
      Link* link = mutable_link(next, operation.link);
      if (link == nullptr) {
        return not_found("link", operation.link.str());
      }
      if (operation.capacity.value() < link->spec.reserved.value()) {
        return Status::error(ErrorCode::capacity_exceeded,
                             "link '" + operation.link.str() +
                                 "' capacity would fall below its reserved capacity");
      }
      link->spec.capacity = operation.capacity;
      break;
    }
    case OperationKind::route_add: {
      if (next.find_route(operation.route) != nullptr) {
        return conflict("route", operation.route.str(), "already exists in the snapshot");
      }
      if (operation.route_spec.path.empty()) {
        return impossible("route '" + operation.route.str() + "' has an empty path");
      }
      if (next.find_device(operation.route_spec.source) == nullptr) {
        return not_found("device", operation.route_spec.source.str());
      }
      if (next.find_device(operation.route_spec.sink) == nullptr) {
        return not_found("device", operation.route_spec.sink.str());
      }
      for (const LinkId& link_id : operation.route_spec.path) {
        if (next.find_link(link_id) == nullptr) {
          return not_found("link", link_id.str());
        }
      }
      Route route;
      route.id = operation.route;
      route.spec = operation.route_spec;
      route.state = operation.route_state;
      if (!route_is_walkable(next, route)) {
        return impossible("route '" + operation.route.str() +
                          "' path is not connected from source to sink");
      }
      next.routes.push_back(std::move(route));
      break;
    }
    case OperationKind::route_remove: {
      Route* route = mutable_route(next, operation.route);
      if (route == nullptr) {
        return not_found("route", operation.route.str());
      }
      for (const Workload& workload : next.workloads) {
        for (const RouteBinding& binding : workload.bindings) {
          if (binding.route == operation.route) {
            return conflict("route", operation.route.str(),
                            "is still bound by workload '" + workload.id.str() + "'");
          }
        }
      }
      next.routes.erase(std::remove_if(next.routes.begin(), next.routes.end(),
                                       [&operation](const Route& candidate) {
                                         return candidate.id == operation.route;
                                       }),
                        next.routes.end());
      break;
    }
    case OperationKind::route_set_path: {
      Route* route = mutable_route(next, operation.route);
      if (route == nullptr) {
        return not_found("route", operation.route.str());
      }
      for (const LinkId& link_id : operation.route_spec.path) {
        if (next.find_link(link_id) == nullptr) {
          return not_found("link", link_id.str());
        }
      }
      Route candidate;
      candidate.id = route->id;
      candidate.spec = operation.route_spec;
      candidate.state = route->state;
      if (candidate.spec.path.empty()) {
        return impossible("route '" + operation.route.str() + "' has an empty path");
      }
      if (!route_is_walkable(next, candidate)) {
        return impossible("route '" + operation.route.str() +
                          "' path is not connected from source to sink");
      }
      route->spec = candidate.spec;
      break;
    }
    case OperationKind::route_set_state: {
      Route* route = mutable_route(next, operation.route);
      if (route == nullptr) {
        return not_found("route", operation.route.str());
      }
      if (operation.route_state == RouteState::active && !route_is_walkable(next, *route)) {
        return impossible("route '" + operation.route.str() +
                          "' cannot be activated because its path is not connected");
      }
      route->state = operation.route_state;
      break;
    }
    case OperationKind::workload_add: {
      if (next.find_workload(operation.workload) != nullptr) {
        return conflict("workload", operation.workload.str(), "already exists in the snapshot");
      }
      Workload workload;
      workload.id = operation.workload;
      workload.bindings = operation.bindings;
      workload.requirements = operation.requirements;
      workload.contract_code = operation.contract_code;
      for (const RouteBinding& binding : workload.bindings) {
        if (next.find_route(binding.route) == nullptr) {
          return not_found("route", binding.route.str());
        }
      }
      next.workloads.push_back(std::move(workload));
      break;
    }
    case OperationKind::workload_remove: {
      const Workload* workload = next.find_workload(operation.workload);
      if (workload == nullptr) {
        return not_found("workload", operation.workload.str());
      }
      next.workloads.erase(std::remove_if(next.workloads.begin(), next.workloads.end(),
                                          [&operation](const Workload& candidate) {
                                            return candidate.id == operation.workload;
                                          }),
                           next.workloads.end());
      break;
    }
    case OperationKind::workload_set_bindings: {
      Workload* workload = mutable_workload(next, operation.workload);
      if (workload == nullptr) {
        return not_found("workload", operation.workload.str());
      }
      for (const RouteBinding& binding : operation.bindings) {
        if (next.find_route(binding.route) == nullptr) {
          return not_found("route", binding.route.str());
        }
      }
      workload->bindings = operation.bindings;
      break;
    }
    case OperationKind::workload_set_requirements: {
      Workload* workload = mutable_workload(next, operation.workload);
      if (workload == nullptr) {
        return not_found("workload", operation.workload.str());
      }
      workload->requirements = operation.requirements;
      workload->contract_code = operation.contract_code;
      break;
    }
    case OperationKind::policy_bind: {
      if (operation.policy.empty()) {
        return Status::error(ErrorCode::invalid_argument, "policy operation without a policy id");
      }
      PolicyObject* existing = mutable_policy(next, operation.policy);
      if (existing != nullptr) {
        existing->target = operation.policy_target;
        existing->value_code = operation.value_code;
        existing->enforced = operation.enforced;
        break;
      }
      PolicyObject policy;
      policy.id = operation.policy;
      policy.target = operation.policy_target;
      policy.value_code = operation.value_code;
      policy.enforced = operation.enforced;
      next.policies.push_back(std::move(policy));
      break;
    }
    case OperationKind::policy_unbind: {
      if (next.find_policy(operation.policy) == nullptr) {
        return not_found("policy", operation.policy.str());
      }
      next.policies.erase(std::remove_if(next.policies.begin(), next.policies.end(),
                                         [&operation](const PolicyObject& candidate) {
                                           return candidate.id == operation.policy;
                                         }),
                          next.policies.end());
      break;
    }
    case OperationKind::config_set: {
      if (next.find_device(operation.device) == nullptr) {
        return not_found("device", operation.device.str());
      }
      ConfigEntry* existing = mutable_config(next, operation.device, operation.config_key);
      if (existing != nullptr) {
        const auto revision = checked_increment(existing->revision);
        if (!revision.ok()) {
          return revision.status();
        }
        existing->value_code = operation.value_code;
        existing->revision = revision.value();
        break;
      }
      ConfigEntry entry;
      entry.device = operation.device;
      entry.key = operation.config_key;
      entry.value_code = operation.value_code;
      entry.revision = ConfigRevision(0);
      next.configs.push_back(std::move(entry));
      break;
    }
    case OperationKind::config_clear: {
      if (mutable_config(next, operation.device, operation.config_key) == nullptr) {
        return not_found("config key", operation.config_key.str());
      }
      next.configs.erase(
          std::remove_if(next.configs.begin(), next.configs.end(),
                         [&operation](const ConfigEntry& candidate) {
                           return candidate.device == operation.device &&
                                  candidate.key == operation.config_key;
                         }),
          next.configs.end());
      break;
    }
    case OperationKind::drain_resource: {
      Device* device = mutable_device(next, operation.device);
      if (device == nullptr) {
        return not_found("device", operation.device.str());
      }
      if (device->spec.state == EntityState::removed) {
        return conflict("device", operation.device.str(), "is removed");
      }
      if (device->spec.state == EntityState::draining) {
        return conflict("device", operation.device.str(), "is already draining");
      }
      if (device->spec.state == EntityState::in_maintenance) {
        return conflict("device", operation.device.str(), "is already in maintenance");
      }
      device->spec.state = EntityState::draining;
      break;
    }
    case OperationKind::enter_maintenance: {
      Device* device = mutable_device(next, operation.device);
      if (device == nullptr) {
        return not_found("device", operation.device.str());
      }
      if (device->spec.state == EntityState::in_maintenance) {
        return conflict("device", operation.device.str(), "is already in maintenance");
      }
      if (device->spec.state == EntityState::removed) {
        return conflict("device", operation.device.str(), "is removed");
      }
      if (drain_before_maintenance_enabled(context) &&
          device->spec.state != EntityState::draining) {
        return impossible("device '" + operation.device.str() +
                          "' must be drained before entering maintenance");
      }
      device->spec.state = EntityState::in_maintenance;
      break;
    }
    case OperationKind::exit_maintenance: {
      Device* device = mutable_device(next, operation.device);
      if (device == nullptr) {
        return not_found("device", operation.device.str());
      }
      if (device->spec.state != EntityState::draining &&
          device->spec.state != EntityState::in_maintenance) {
        return conflict("device", operation.device.str(),
                        "is not draining or in maintenance");
      }
      device->spec.state = EntityState::present;
      break;
    }
  }

  next.canonicalize();
  CPLAN_TRY(next.validate());

  AppliedOperation applied;
  applied.effects = operation.write_set();
  applied.state = std::move(next);
  return applied;
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

std::vector<Condition> derive_preconditions(const CurrentStateSnapshot& state,
                                            const Operation& operation,
                                            const EvaluationContext& context) {
  static_cast<void>(context);
  std::vector<Condition> conditions;
  auto add = [&conditions](ConditionKind kind, EntityRef subject, std::string parameter,
                           std::uint64_t quantity) {
    Condition condition;
    condition.kind = kind;
    condition.subject = std::move(subject);
    condition.parameter = std::move(parameter);
    condition.quantity = quantity;
    conditions.push_back(std::move(condition));
  };

  switch (operation.kind) {
    case OperationKind::device_add:
      add(ConditionKind::entity_absent, EntityRef::of(operation.device), "", 0);
      break;
    case OperationKind::device_remove:
    case OperationKind::device_set_capacity:
    case OperationKind::device_set_state:
    case OperationKind::device_set_windows:
    case OperationKind::drain_resource:
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance: {
      add(ConditionKind::entity_present, EntityRef::of(operation.device), "", 0);
      const Device* device = state.find_device(operation.device);
      if (device != nullptr) {
        add(ConditionKind::entity_state_is, EntityRef::of(operation.device),
            to_string(device->spec.state), 0);
      }
      add(ConditionKind::exclusion_inactive, EntityRef::of(operation.device), "", 0);
      if (operation.requires_maintenance_window()) {
        add(ConditionKind::maintenance_window_open, EntityRef::of(operation.device), "", 0);
      }
      if (operation.kind == OperationKind::enter_maintenance ||
          operation.kind == OperationKind::device_remove) {
        add(ConditionKind::device_drained, EntityRef::of(operation.device), "", 0);
      }
      break;
    }
    case OperationKind::link_add:
      add(ConditionKind::entity_absent, EntityRef::of(operation.link), "", 0);
      add(ConditionKind::entity_present, EntityRef::of(operation.link_spec.endpoint_a), "", 0);
      add(ConditionKind::entity_present, EntityRef::of(operation.link_spec.endpoint_b), "", 0);
      break;
    case OperationKind::link_remove:
    case OperationKind::link_set_capacity: {
      add(ConditionKind::entity_present, EntityRef::of(operation.link), "", 0);
      const Link* link = state.find_link(operation.link);
      const LinkSpec& spec = link != nullptr ? link->spec : operation.link_spec;
      add(ConditionKind::entity_present, EntityRef::of(spec.endpoint_a), "", 0);
      add(ConditionKind::entity_present, EntityRef::of(spec.endpoint_b), "", 0);
      add(ConditionKind::exclusion_inactive, EntityRef::of(spec.endpoint_a), "", 0);
      add(ConditionKind::exclusion_inactive, EntityRef::of(spec.endpoint_b), "", 0);
      break;
    }
    case OperationKind::route_add:
      add(ConditionKind::entity_absent, EntityRef::of(operation.route), "", 0);
      add(ConditionKind::entity_present, EntityRef::of(operation.route_spec.source), "", 0);
      add(ConditionKind::entity_present, EntityRef::of(operation.route_spec.sink), "", 0);
      for (const LinkId& link_id : operation.route_spec.path) {
        add(ConditionKind::entity_present, EntityRef::of(link_id), "", 0);
      }
      add(ConditionKind::route_walkable, EntityRef::of(operation.route), "", 0);
      break;
    case OperationKind::route_remove:
    case OperationKind::route_set_state:
    case OperationKind::route_set_path: {
      add(ConditionKind::entity_present, EntityRef::of(operation.route), "", 0);
      const Route* route = state.find_route(operation.route);
      if (route != nullptr) {
        add(ConditionKind::entity_present, EntityRef::of(route->spec.source), "", 0);
        add(ConditionKind::entity_present, EntityRef::of(route->spec.sink), "", 0);
      }
      if (operation.kind == OperationKind::route_set_path) {
        for (const LinkId& link_id : operation.route_spec.path) {
          add(ConditionKind::entity_present, EntityRef::of(link_id), "", 0);
        }
        add(ConditionKind::route_walkable, EntityRef::of(operation.route), "", 0);
      }
      break;
    }
    case OperationKind::workload_add:
      add(ConditionKind::entity_absent, EntityRef::of(operation.workload), "", 0);
      for (const RouteBinding& binding : operation.bindings) {
        add(ConditionKind::entity_present, EntityRef::of(binding.route), "", 0);
      }
      break;
    case OperationKind::workload_remove:
    case OperationKind::workload_set_bindings:
    case OperationKind::workload_set_requirements: {
      add(ConditionKind::entity_present, EntityRef::of(operation.workload), "", 0);
      const std::vector<RouteBinding>* bindings = &operation.bindings;
      const Workload* workload = state.find_workload(operation.workload);
      if (operation.kind == OperationKind::workload_set_requirements && workload != nullptr) {
        bindings = &workload->bindings;
      }
      if (operation.kind != OperationKind::workload_remove) {
        for (const RouteBinding& binding : *bindings) {
          add(ConditionKind::entity_present, EntityRef::of(binding.route), "", 0);
        }
      }
      break;
    }
    case OperationKind::policy_bind: {
      const PolicyObject* existing = state.find_policy(operation.policy);
      if (existing == nullptr) {
        add(ConditionKind::entity_absent, EntityRef::of(operation.policy), "", 0);
      } else {
        add(ConditionKind::policy_bound, EntityRef::of(operation.policy), "", 0);
      }
      if (operation.policy_target.scope != PolicyScope::global) {
        add(ConditionKind::entity_present,
            EntityRef{static_cast<EntityKind>(operation.policy_target.scope),
                      operation.policy_target.identity},
            "", 0);
      }
      break;
    }
    case OperationKind::policy_unbind:
      add(ConditionKind::policy_bound, EntityRef::of(operation.policy), "", 0);
      break;
    case OperationKind::config_set:
    case OperationKind::config_clear:
      add(ConditionKind::entity_present, EntityRef::of(operation.device), "", 0);
      add(ConditionKind::exclusion_inactive, EntityRef::of(operation.device), "", 0);
      break;
  }

  std::sort(conditions.begin(), conditions.end());
  conditions.erase(std::unique(conditions.begin(), conditions.end()), conditions.end());
  return conditions;
}

std::vector<Condition> derive_postconditions(const CurrentStateSnapshot& state,
                                             const Operation& operation,
                                             const EvaluationContext& context) {
  static_cast<void>(state);
  static_cast<void>(context);
  std::vector<Condition> conditions;
  auto add = [&conditions](ConditionKind kind, EntityRef subject, std::string parameter,
                           std::uint64_t quantity) {
    Condition condition;
    condition.kind = kind;
    condition.subject = std::move(subject);
    condition.parameter = std::move(parameter);
    condition.quantity = quantity;
    conditions.push_back(std::move(condition));
  };

  switch (operation.kind) {
    case OperationKind::device_add:
      add(ConditionKind::entity_present, EntityRef::of(operation.device), "", 0);
      add(ConditionKind::capacity_at_least, EntityRef::of(operation.device), "",
          operation.device_spec.termination_capacity.value());
      break;
    case OperationKind::device_remove:
      add(ConditionKind::entity_absent, EntityRef::of(operation.device), "", 0);
      break;
    case OperationKind::device_set_capacity:
      add(ConditionKind::capacity_at_least, EntityRef::of(operation.device), "",
          operation.capacity.value());
      break;
    case OperationKind::device_set_state:
    case OperationKind::drain_resource:
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance: {
      const EntityState target = operation.kind == OperationKind::device_set_state
                                     ? operation.entity_state
                                     : (operation.kind == OperationKind::drain_resource
                                            ? EntityState::draining
                                            : (operation.kind == OperationKind::enter_maintenance
                                                   ? EntityState::in_maintenance
                                                   : EntityState::present));
      add(ConditionKind::entity_state_is, EntityRef::of(operation.device), to_string(target), 0);
      break;
    }
    case OperationKind::device_set_windows:
      add(ConditionKind::entity_present, EntityRef::of(operation.device), "", 0);
      break;
    case OperationKind::link_add:
      add(ConditionKind::entity_present, EntityRef::of(operation.link), "", 0);
      break;
    case OperationKind::link_remove:
      add(ConditionKind::entity_absent, EntityRef::of(operation.link), "", 0);
      break;
    case OperationKind::link_set_capacity:
      add(ConditionKind::capacity_at_least, EntityRef::of(operation.link), "",
          operation.capacity.value());
      break;
    case OperationKind::route_add:
      add(ConditionKind::entity_present, EntityRef::of(operation.route), "", 0);
      add(ConditionKind::route_state_is, EntityRef::of(operation.route),
          to_string(operation.route_state), 0);
      add(ConditionKind::route_walkable, EntityRef::of(operation.route), "", 0);
      break;
    case OperationKind::route_remove:
      add(ConditionKind::entity_absent, EntityRef::of(operation.route), "", 0);
      break;
    case OperationKind::route_set_path:
      add(ConditionKind::route_walkable, EntityRef::of(operation.route), "", 0);
      break;
    case OperationKind::route_set_state:
      add(ConditionKind::route_state_is, EntityRef::of(operation.route),
          to_string(operation.route_state), 0);
      break;
    case OperationKind::workload_add:
    case OperationKind::workload_set_bindings:
    case OperationKind::workload_set_requirements:
      add(ConditionKind::entity_present, EntityRef::of(operation.workload), "", 0);
      for (const RouteBinding& binding : operation.bindings) {
        if (binding.active) {
          add(ConditionKind::binding_active, EntityRef::of(operation.workload),
              binding.route.str(), 0);
        }
      }
      break;
    case OperationKind::workload_remove:
      add(ConditionKind::entity_absent, EntityRef::of(operation.workload), "", 0);
      break;
    case OperationKind::policy_bind:
      add(ConditionKind::policy_bound, EntityRef::of(operation.policy), "", 0);
      break;
    case OperationKind::policy_unbind:
      add(ConditionKind::entity_absent, EntityRef::of(operation.policy), "", 0);
      break;
    case OperationKind::config_set:
      add(ConditionKind::config_equals, EntityRef::of(operation.device, operation.config_key),
          operation.value_code, 0);
      break;
    case OperationKind::config_clear:
      add(ConditionKind::entity_absent, EntityRef::of(operation.device, operation.config_key), "",
          0);
      break;
  }

  std::sort(conditions.begin(), conditions.end());
  conditions.erase(std::unique(conditions.begin(), conditions.end()), conditions.end());
  return conditions;
}

namespace {

Result<bool> entity_exists(const CurrentStateSnapshot& state, const EntityRef& subject) {
  switch (subject.kind) {
    case EntityKind::global:
      return true;
    case EntityKind::device: {
      const auto id = DeviceId::parse(subject.identity);
      if (!id.ok()) return id.status();
      const Device* device = state.find_device(id.value());
      return device != nullptr && device->spec.state != EntityState::removed;
    }
    case EntityKind::link: {
      const auto id = LinkId::parse(subject.identity);
      if (!id.ok()) return id.status();
      const Link* link = state.find_link(id.value());
      return link != nullptr && link->state != EntityState::removed;
    }
    case EntityKind::route: {
      const auto id = RouteId::parse(subject.identity);
      if (!id.ok()) return id.status();
      return state.find_route(id.value()) != nullptr;
    }
    case EntityKind::workload: {
      const auto id = WorkloadId::parse(subject.identity);
      if (!id.ok()) return id.status();
      return state.find_workload(id.value()) != nullptr;
    }
    case EntityKind::failure_domain: {
      const auto id = FailureDomainId::parse(subject.identity);
      if (!id.ok()) return id.status();
      for (const Device& device : state.devices) {
        if (device.spec.domain == id.value() && device.spec.state != EntityState::removed) {
          return true;
        }
      }
      return false;
    }
    case EntityKind::policy: {
      const auto id = PolicyId::parse(subject.identity);
      if (!id.ok()) return id.status();
      return state.find_policy(id.value()) != nullptr;
    }
    case EntityKind::config: {
      const std::size_t separator = subject.identity.find('|');
      if (separator == std::string::npos) {
        return Status::error(ErrorCode::malformed_input,
                             "config reference is not in 'device|key' form: " + subject.identity);
      }
      const auto device = DeviceId::parse(subject.identity.substr(0, separator));
      if (!device.ok()) return device.status();
      const auto key = ConfigKey::parse(subject.identity.substr(separator + 1));
      if (!key.ok()) return key.status();
      return state.find_config(device.value(), key.value()) != nullptr;
    }
    case EntityKind::contract:
      return true;
  }
  return Status::error(ErrorCode::malformed_input, "unhandled entity kind");
}

}  // namespace

Result<bool> evaluate_condition(const CurrentStateSnapshot& state, const Condition& condition,
                                const EvaluationContext& context) {
  switch (condition.kind) {
    case ConditionKind::entity_present:
      return entity_exists(state, condition.subject);
    case ConditionKind::entity_absent: {
      auto present = entity_exists(state, condition.subject);
      if (!present.ok()) {
        return present.status();
      }
      return !present.value();
    }
    case ConditionKind::entity_state_is: {
      if (condition.subject.kind == EntityKind::device) {
        const auto id = DeviceId::parse(condition.subject.identity);
        if (!id.ok()) return id.status();
        const Device* device = state.find_device(id.value());
        if (device == nullptr) return false;
        return condition.parameter == to_string(device->spec.state);
      }
      if (condition.subject.kind == EntityKind::link) {
        const auto id = LinkId::parse(condition.subject.identity);
        if (!id.ok()) return id.status();
        const Link* link = state.find_link(id.value());
        if (link == nullptr) return false;
        return condition.parameter == to_string(link->state);
      }
      return Status::error(ErrorCode::malformed_input,
                           "entity_state_is requires a device or link subject");
    }
    case ConditionKind::route_walkable: {
      const auto id = RouteId::parse(condition.subject.identity);
      if (!id.ok()) return id.status();
      const Route* route = state.find_route(id.value());
      if (route == nullptr) return false;
      if (!route_is_walkable(state, *route)) return false;
      for (const LinkId& link_id : route->spec.path) {
        const Link* link = state.find_link(link_id);
        if (link == nullptr || link->state != EntityState::present) return false;
      }
      const Device* source = state.find_device(route->spec.source);
      const Device* sink = state.find_device(route->spec.sink);
      return source != nullptr && sink != nullptr &&
             source->spec.state != EntityState::removed &&
             sink->spec.state != EntityState::removed;
    }
    case ConditionKind::route_state_is: {
      const auto id = RouteId::parse(condition.subject.identity);
      if (!id.ok()) return id.status();
      const Route* route = state.find_route(id.value());
      if (route == nullptr) return false;
      return condition.parameter == to_string(route->state);
    }
    case ConditionKind::config_equals: {
      const std::size_t separator = condition.subject.identity.find('|');
      if (separator == std::string::npos) {
        return Status::error(ErrorCode::malformed_input,
                             "config reference is not in 'device|key' form");
      }
      const auto device = DeviceId::parse(condition.subject.identity.substr(0, separator));
      if (!device.ok()) return device.status();
      const auto key = ConfigKey::parse(condition.subject.identity.substr(separator + 1));
      if (!key.ok()) return key.status();
      const ConfigEntry* entry = state.find_config(device.value(), key.value());
      return entry != nullptr && entry->value_code == condition.parameter;
    }
    case ConditionKind::policy_bound: {
      const auto id = PolicyId::parse(condition.subject.identity);
      if (!id.ok()) return id.status();
      return state.find_policy(id.value()) != nullptr;
    }
    case ConditionKind::binding_active: {
      const auto workload_id = WorkloadId::parse(condition.subject.identity);
      if (!workload_id.ok()) return workload_id.status();
      const Workload* workload = state.find_workload(workload_id.value());
      if (workload == nullptr) return false;
      for (const RouteBinding& binding : workload->bindings) {
        if (binding.route.str() == condition.parameter) {
          return binding.active;
        }
      }
      return false;
    }
    case ConditionKind::capacity_at_least: {
      if (condition.subject.kind == EntityKind::device) {
        const auto id = DeviceId::parse(condition.subject.identity);
        if (!id.ok()) return id.status();
        const Device* device = state.find_device(id.value());
        return device != nullptr && device->spec.termination_capacity.value() >= condition.quantity;
      }
      if (condition.subject.kind == EntityKind::link) {
        const auto id = LinkId::parse(condition.subject.identity);
        if (!id.ok()) return id.status();
        const Link* link = state.find_link(id.value());
        return link != nullptr && link->spec.capacity.value() >= condition.quantity;
      }
      return Status::error(ErrorCode::malformed_input,
                           "capacity_at_least requires a device or link subject");
    }
    case ConditionKind::invariant_satisfied: {
      const auto invariant = parse_invariant_id(condition.parameter);
      if (!invariant.ok()) {
        return invariant.status();
      }
      for (const Violation& violation : check_invariants(state, context)) {
        if (violation.invariant == invariant.value() &&
            violation.severity == Severity::violation) {
          return false;
        }
      }
      return true;
    }
    case ConditionKind::maintenance_window_open: {
      const auto id = DeviceId::parse(condition.subject.identity);
      if (!id.ok()) return id.status();
      const Device* device = state.find_device(id.value());
      if (device == nullptr) return false;
      std::string reason;
      return maintenance_permitted_at(*device, context.instant, &reason);
    }
    case ConditionKind::exclusion_inactive: {
      const auto id = DeviceId::parse(condition.subject.identity);
      if (!id.ok()) return id.status();
      const Device* device = state.find_device(id.value());
      if (device == nullptr) return false;
      std::string reason;
      return change_permitted_at(*device, context.instant, &reason);
    }
    case ConditionKind::device_drained: {
      const auto id = DeviceId::parse(condition.subject.identity);
      if (!id.ok()) return id.status();
      const Device* device = state.find_device(id.value());
      if (device == nullptr) return false;
      return device->spec.state == EntityState::draining ||
             device->spec.state == EntityState::in_maintenance;
    }
  }
  return Status::error(ErrorCode::malformed_input, "unhandled condition kind");
}

}  // namespace cplan
