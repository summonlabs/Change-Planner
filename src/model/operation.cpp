#include "change_planner/model/operation.hpp"

#include <algorithm>
#include <set>

namespace cplan {

const char* to_string(EntityKind kind) noexcept {
  switch (kind) {
    case EntityKind::global:
      return "global";
    case EntityKind::device:
      return "device";
    case EntityKind::link:
      return "link";
    case EntityKind::route:
      return "route";
    case EntityKind::workload:
      return "workload";
    case EntityKind::failure_domain:
      return "failure-domain";
    case EntityKind::policy:
      return "policy";
    case EntityKind::config:
      return "config";
    case EntityKind::contract:
      return "contract";
  }
  return "unknown";
}

Result<EntityKind> parse_entity_kind(std::string_view token) {
  if (token == "global") return EntityKind::global;
  if (token == "device") return EntityKind::device;
  if (token == "link") return EntityKind::link;
  if (token == "route") return EntityKind::route;
  if (token == "workload") return EntityKind::workload;
  if (token == "failure-domain") return EntityKind::failure_domain;
  if (token == "policy") return EntityKind::policy;
  if (token == "config") return EntityKind::config;
  if (token == "contract") return EntityKind::contract;
  return Status::error(ErrorCode::malformed_input,
                       "unknown entity kind token: " + std::string(token));
}

const char* to_string(OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::device_add:
      return "device-add";
    case OperationKind::device_remove:
      return "device-remove";
    case OperationKind::device_set_capacity:
      return "device-set-capacity";
    case OperationKind::device_set_state:
      return "device-set-state";
    case OperationKind::device_set_windows:
      return "device-set-windows";
    case OperationKind::link_add:
      return "link-add";
    case OperationKind::link_remove:
      return "link-remove";
    case OperationKind::link_set_capacity:
      return "link-set-capacity";
    case OperationKind::route_add:
      return "route-add";
    case OperationKind::route_remove:
      return "route-remove";
    case OperationKind::route_set_path:
      return "route-set-path";
    case OperationKind::route_set_state:
      return "route-set-state";
    case OperationKind::workload_add:
      return "workload-add";
    case OperationKind::workload_remove:
      return "workload-remove";
    case OperationKind::workload_set_bindings:
      return "workload-set-bindings";
    case OperationKind::workload_set_requirements:
      return "workload-set-requirements";
    case OperationKind::policy_bind:
      return "policy-bind";
    case OperationKind::policy_unbind:
      return "policy-unbind";
    case OperationKind::config_set:
      return "config-set";
    case OperationKind::config_clear:
      return "config-clear";
    case OperationKind::drain_resource:
      return "drain-resource";
    case OperationKind::enter_maintenance:
      return "enter-maintenance";
    case OperationKind::exit_maintenance:
      return "exit-maintenance";
  }
  return "unknown";
}

Result<OperationKind> parse_operation_kind(std::string_view token) {
  if (token == "device-add") return OperationKind::device_add;
  if (token == "device-remove") return OperationKind::device_remove;
  if (token == "device-set-capacity") return OperationKind::device_set_capacity;
  if (token == "device-set-state") return OperationKind::device_set_state;
  if (token == "device-set-windows") return OperationKind::device_set_windows;
  if (token == "link-add") return OperationKind::link_add;
  if (token == "link-remove") return OperationKind::link_remove;
  if (token == "link-set-capacity") return OperationKind::link_set_capacity;
  if (token == "route-add") return OperationKind::route_add;
  if (token == "route-remove") return OperationKind::route_remove;
  if (token == "route-set-path") return OperationKind::route_set_path;
  if (token == "route-set-state") return OperationKind::route_set_state;
  if (token == "workload-add") return OperationKind::workload_add;
  if (token == "workload-remove") return OperationKind::workload_remove;
  if (token == "workload-set-bindings") return OperationKind::workload_set_bindings;
  if (token == "workload-set-requirements") return OperationKind::workload_set_requirements;
  if (token == "policy-bind") return OperationKind::policy_bind;
  if (token == "policy-unbind") return OperationKind::policy_unbind;
  if (token == "config-set") return OperationKind::config_set;
  if (token == "config-clear") return OperationKind::config_clear;
  if (token == "drain-resource") return OperationKind::drain_resource;
  if (token == "enter-maintenance") return OperationKind::enter_maintenance;
  if (token == "exit-maintenance") return OperationKind::exit_maintenance;
  return Status::error(ErrorCode::malformed_input,
                       "unknown operation kind token: " + std::string(token));
}

const char* to_string(OperationClass operation_class) noexcept {
  switch (operation_class) {
    case OperationClass::configuration:
      return "configuration";
    case OperationClass::maintenance:
      return "maintenance";
  }
  return "unknown";
}

const char* to_string(ConditionKind kind) noexcept {
  switch (kind) {
    case ConditionKind::entity_absent:
      return "entity-absent";
    case ConditionKind::entity_present:
      return "entity-present";
    case ConditionKind::entity_state_is:
      return "entity-state-is";
    case ConditionKind::route_walkable:
      return "route-walkable";
    case ConditionKind::route_state_is:
      return "route-state-is";
    case ConditionKind::config_equals:
      return "config-equals";
    case ConditionKind::policy_bound:
      return "policy-bound";
    case ConditionKind::binding_active:
      return "binding-active";
    case ConditionKind::capacity_at_least:
      return "capacity-at-least";
    case ConditionKind::invariant_satisfied:
      return "invariant-satisfied";
    case ConditionKind::maintenance_window_open:
      return "maintenance-window-open";
    case ConditionKind::exclusion_inactive:
      return "exclusion-inactive";
    case ConditionKind::device_drained:
      return "device-drained";
  }
  return "unknown";
}

Result<ConditionKind> parse_condition_kind(std::string_view token) {
  if (token == "entity-absent") return ConditionKind::entity_absent;
  if (token == "entity-present") return ConditionKind::entity_present;
  if (token == "entity-state-is") return ConditionKind::entity_state_is;
  if (token == "route-walkable") return ConditionKind::route_walkable;
  if (token == "route-state-is") return ConditionKind::route_state_is;
  if (token == "config-equals") return ConditionKind::config_equals;
  if (token == "policy-bound") return ConditionKind::policy_bound;
  if (token == "binding-active") return ConditionKind::binding_active;
  if (token == "capacity-at-least") return ConditionKind::capacity_at_least;
  if (token == "invariant-satisfied") return ConditionKind::invariant_satisfied;
  if (token == "maintenance-window-open") return ConditionKind::maintenance_window_open;
  if (token == "exclusion-inactive") return ConditionKind::exclusion_inactive;
  if (token == "device-drained") return ConditionKind::device_drained;
  return Status::error(ErrorCode::malformed_input,
                       "unknown condition kind token: " + std::string(token));
}

// ---------------------------------------------------------------------------
// EntityRef
// ---------------------------------------------------------------------------

std::string EntityRef::to_string() const {
  if (kind == EntityKind::global) {
    return "global";
  }
  return std::string(cplan::to_string(kind)) + ":" + identity;
}

EntityRef EntityRef::of(const DeviceId& id) { return EntityRef{EntityKind::device, id.str()}; }
EntityRef EntityRef::of(const LinkId& id) { return EntityRef{EntityKind::link, id.str()}; }
EntityRef EntityRef::of(const RouteId& id) { return EntityRef{EntityKind::route, id.str()}; }
EntityRef EntityRef::of(const WorkloadId& id) { return EntityRef{EntityKind::workload, id.str()}; }
EntityRef EntityRef::of(const FailureDomainId& id) {
  return EntityRef{EntityKind::failure_domain, id.str()};
}
EntityRef EntityRef::of(const PolicyId& id) { return EntityRef{EntityKind::policy, id.str()}; }

EntityRef EntityRef::of(const ContractId& id) { return EntityRef{EntityKind::contract, id.str()}; }

EntityRef EntityRef::global() { return EntityRef{EntityKind::global, std::string()}; }

// Composite identity for configuration keys. '|' cannot appear in an identity
// (see is_valid_identity_text), so the separator is unambiguous.
EntityRef EntityRef::of(const DeviceId& device, const ConfigKey& key) {
  return EntityRef{EntityKind::config, device.str() + "|" + key.str()};
}

namespace {

Result<std::pair<DeviceId, ConfigKey>> split_config_identity(const std::string& identity) {
  const std::size_t separator = identity.find('|');
  if (separator == std::string::npos || identity.find('|', separator + 1) != std::string::npos) {
    return Status::error(ErrorCode::malformed_input,
                         "config reference is not in 'device|key' form: " + identity);
  }
  const auto device = DeviceId::parse(identity.substr(0, separator));
  if (!device.ok()) {
    return device.status();
  }
  const auto key = ConfigKey::parse(identity.substr(separator + 1));
  if (!key.ok()) {
    return key.status();
  }
  return std::make_pair(device.value(), key.value());
}

}  // namespace

// ---------------------------------------------------------------------------
// Operation
// ---------------------------------------------------------------------------

Digest Operation::identity_digest() const {
  CanonicalEncoder encoder;
  encode(encoder);
  return encoder.digest();
}

OperationClass Operation::operation_class() const {
  switch (kind) {
    case OperationKind::drain_resource:
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance:
      return OperationClass::maintenance;
    default:
      return OperationClass::configuration;
  }
}

bool Operation::requires_maintenance_window() const {
  switch (kind) {
    case OperationKind::drain_resource:
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance:
    case OperationKind::device_remove:
      return true;
    default:
      return false;
  }
}

std::string Operation::target_identity() const {
  switch (kind) {
    case OperationKind::device_add:
    case OperationKind::device_remove:
    case OperationKind::device_set_capacity:
    case OperationKind::device_set_state:
    case OperationKind::device_set_windows:
    case OperationKind::drain_resource:
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance:
      return device.str();
    case OperationKind::link_add:
    case OperationKind::link_remove:
    case OperationKind::link_set_capacity:
      return link.str();
    case OperationKind::route_add:
    case OperationKind::route_remove:
    case OperationKind::route_set_path:
    case OperationKind::route_set_state:
      return route.str();
    case OperationKind::workload_add:
    case OperationKind::workload_remove:
    case OperationKind::workload_set_bindings:
    case OperationKind::workload_set_requirements:
      return workload.str();
    case OperationKind::policy_bind:
    case OperationKind::policy_unbind:
      return policy.str();
    case OperationKind::config_set:
    case OperationKind::config_clear:
      return device.str() + "|" + config_key.str();
  }
  return std::string();
}

bool operator<(const Operation& lhs, const Operation& rhs) {
  if (lhs.kind != rhs.kind) {
    return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
  }
  const std::string lhs_target = lhs.target_identity();
  const std::string rhs_target = rhs.target_identity();
  if (lhs_target != rhs_target) {
    return lhs_target < rhs_target;
  }
  return lhs.identity_digest() < rhs.identity_digest();
}

std::vector<EntityRef> Operation::write_set() const {
  switch (kind) {
    case OperationKind::device_add:
    case OperationKind::device_remove:
    case OperationKind::device_set_capacity:
    case OperationKind::device_set_state:
    case OperationKind::device_set_windows:
    case OperationKind::drain_resource:
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance:
      return {EntityRef::of(device)};
    case OperationKind::link_add:
    case OperationKind::link_remove:
    case OperationKind::link_set_capacity:
      return {EntityRef::of(link)};
    case OperationKind::route_add:
    case OperationKind::route_remove:
    case OperationKind::route_set_path:
    case OperationKind::route_set_state:
      return {EntityRef::of(route)};
    case OperationKind::workload_add:
    case OperationKind::workload_remove:
    case OperationKind::workload_set_bindings:
    case OperationKind::workload_set_requirements:
      return {EntityRef::of(workload)};
    case OperationKind::policy_bind:
    case OperationKind::policy_unbind:
      return {EntityRef::of(policy)};
    case OperationKind::config_set:
    case OperationKind::config_clear:
      return {EntityRef::of(device, config_key)};
  }
  return {};
}

std::vector<EntityRef> Operation::read_set() const {
  std::vector<EntityRef> result;
  switch (kind) {
    case OperationKind::link_add:
    case OperationKind::link_remove:
    case OperationKind::link_set_capacity:
      if (!link_spec.endpoint_a.empty()) result.push_back(EntityRef::of(link_spec.endpoint_a));
      if (!link_spec.endpoint_b.empty()) result.push_back(EntityRef::of(link_spec.endpoint_b));
      break;
    case OperationKind::route_add:
    case OperationKind::route_set_path:
      if (!route_spec.source.empty()) result.push_back(EntityRef::of(route_spec.source));
      if (!route_spec.sink.empty()) result.push_back(EntityRef::of(route_spec.sink));
      for (const LinkId& link_id : route_spec.path) result.push_back(EntityRef::of(link_id));
      break;
    case OperationKind::route_remove:
    case OperationKind::route_set_state:
      if (!route_spec.source.empty()) result.push_back(EntityRef::of(route_spec.source));
      if (!route_spec.sink.empty()) result.push_back(EntityRef::of(route_spec.sink));
      break;
    case OperationKind::workload_add:
    case OperationKind::workload_remove:
    case OperationKind::workload_set_bindings:
    case OperationKind::workload_set_requirements:
      for (const RouteBinding& binding : bindings) result.push_back(EntityRef::of(binding.route));
      break;
    case OperationKind::policy_bind:
    case OperationKind::policy_unbind:
      if (policy_target.scope != PolicyScope::global && !policy_target.identity.empty()) {
        result.push_back(EntityRef{static_cast<EntityKind>(policy_target.scope), policy_target.identity});
      }
      break;
    case OperationKind::config_set:
    case OperationKind::config_clear:
      if (!device.empty()) result.push_back(EntityRef::of(device));
      break;
    default:
      break;
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<DeviceId> Operation::capability_subjects(const CurrentStateSnapshot& state) const {
  std::vector<DeviceId> subjects;
  auto push = [&subjects](const DeviceId& id) {
    if (!id.empty()) {
      subjects.push_back(id);
    }
  };

  switch (kind) {
    case OperationKind::device_add:
      // The device does not exist yet: no runtime can be asked to support it.
      break;
    case OperationKind::device_remove:
    case OperationKind::device_set_capacity:
    case OperationKind::device_set_state:
    case OperationKind::device_set_windows:
    case OperationKind::drain_resource:
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance:
    case OperationKind::config_set:
    case OperationKind::config_clear:
      push(device);
      break;
    case OperationKind::link_add:
    case OperationKind::link_set_capacity:
      push(link_spec.endpoint_a);
      push(link_spec.endpoint_b);
      break;
    case OperationKind::link_remove: {
      push(link_spec.endpoint_a);
      push(link_spec.endpoint_b);
      if (const Link* existing = state.find_link(link); existing != nullptr) {
        push(existing->spec.endpoint_a);
        push(existing->spec.endpoint_b);
      }
      break;
    }
    case OperationKind::route_add:
    case OperationKind::route_set_path:
      push(route_spec.source);
      push(route_spec.sink);
      break;
    case OperationKind::route_remove:
    case OperationKind::route_set_state: {
      push(route_spec.source);
      push(route_spec.sink);
      if (const Route* existing = state.find_route(route); existing != nullptr) {
        push(existing->spec.source);
        push(existing->spec.sink);
      }
      break;
    }
    case OperationKind::workload_add:
    case OperationKind::workload_remove:
    case OperationKind::workload_set_bindings:
    case OperationKind::workload_set_requirements: {
      for (const RouteBinding& binding : bindings) {
        if (const Route* route_in_state = state.find_route(binding.route)) {
          push(route_in_state->spec.source);
          push(route_in_state->spec.sink);
        }
      }
      if (const Workload* existing = state.find_workload(workload)) {
        for (const RouteBinding& binding : existing->bindings) {
          if (const Route* route_in_state = state.find_route(binding.route)) {
            push(route_in_state->spec.source);
            push(route_in_state->spec.sink);
          }
        }
      }
      break;
    }
    case OperationKind::policy_bind:
    case OperationKind::policy_unbind: {
      switch (policy_target.scope) {
        case PolicyScope::device: {
          const auto target = DeviceId::parse(policy_target.identity);
          if (target.ok()) {
            push(target.value());
          }
          break;
        }
        case PolicyScope::link: {
          const auto target = LinkId::parse(policy_target.identity);
          if (target.ok()) {
            if (const Link* existing = state.find_link(target.value())) {
              push(existing->spec.endpoint_a);
              push(existing->spec.endpoint_b);
            }
          }
          break;
        }
        case PolicyScope::route: {
          const auto target = RouteId::parse(policy_target.identity);
          if (target.ok()) {
            if (const Route* existing = state.find_route(target.value())) {
              push(existing->spec.source);
              push(existing->spec.sink);
            }
          }
          break;
        }
        case PolicyScope::global:
        case PolicyScope::workload:
          break;
      }
      break;
    }
  }

  std::sort(subjects.begin(), subjects.end());
  subjects.erase(std::unique(subjects.begin(), subjects.end()), subjects.end());
  return subjects;
}

ChangeCapability Operation::required_capability() const {
  switch (kind) {
    case OperationKind::device_add:
      return ChangeCapability::device_add;
    case OperationKind::device_remove:
      return ChangeCapability::device_remove;
    case OperationKind::device_set_capacity:
      return ChangeCapability::device_capacity;
    case OperationKind::device_set_state:
      return ChangeCapability::device_state;
    case OperationKind::device_set_windows:
      return ChangeCapability::device_maintenance;
    case OperationKind::link_add:
      return ChangeCapability::link_add;
    case OperationKind::link_remove:
      return ChangeCapability::link_remove;
    case OperationKind::link_set_capacity:
      return ChangeCapability::link_capacity;
    case OperationKind::route_add:
    case OperationKind::route_set_path:
      return ChangeCapability::route_add;
    case OperationKind::route_remove:
      return ChangeCapability::route_remove;
    case OperationKind::route_set_state:
      return ChangeCapability::route_state;
    case OperationKind::workload_add:
    case OperationKind::workload_remove:
    case OperationKind::workload_set_bindings:
      return ChangeCapability::workload_binding;
    case OperationKind::workload_set_requirements:
      return ChangeCapability::workload_requirements;
    case OperationKind::policy_bind:
    case OperationKind::policy_unbind:
      return ChangeCapability::policy_bind;
    case OperationKind::config_set:
    case OperationKind::config_clear:
      return ChangeCapability::config_set;
    case OperationKind::drain_resource:
      return ChangeCapability::device_drain;
    case OperationKind::enter_maintenance:
    case OperationKind::exit_maintenance:
      return ChangeCapability::device_maintenance;
  }
  return ChangeCapability::none;
}

std::string Operation::to_string() const {
  std::string out = cplan::to_string(kind);
  const std::string target = target_identity();
  if (!target.empty()) {
    out += " ";
    out += target;
  }
  switch (kind) {
    case OperationKind::device_add:
    case OperationKind::device_set_capacity:
      out += " capacity=" + std::to_string(capacity.value());
      out += " domain=" + device_spec.domain.str();
      break;
    case OperationKind::device_set_state:
      out += " state=" + std::string(cplan::to_string(entity_state));
      break;
    case OperationKind::link_add:
    case OperationKind::link_set_capacity:
      out += " " + link_spec.endpoint_a.str() + "--" + link_spec.endpoint_b.str();
      out += " capacity=" + std::to_string(link_spec.capacity.value());
      break;
    case OperationKind::route_add:
    case OperationKind::route_set_path: {
      out += " " + route_spec.source.str() + "->" + route_spec.sink.str() + " via";
      for (const LinkId& link_id : route_spec.path) {
        out += " " + link_id.str();
      }
      break;
    }
    case OperationKind::route_set_state:
      out += " state=" + std::string(cplan::to_string(route_state));
      break;
    case OperationKind::workload_set_bindings:
      out += " bindings=" + std::to_string(bindings.size());
      break;
    case OperationKind::workload_set_requirements:
      out += " min-disjoint-domains=" + std::to_string(requirements.min_disjoint_domains);
      break;
    case OperationKind::policy_bind:
      out += " scope=" + std::string(cplan::to_string(policy_target.scope));
      out += " value=" + value_code;
      break;
    case OperationKind::config_set:
      out += " value=" + value_code;
      break;
    default:
      break;
  }
  if (!reason_code.empty()) {
    out += " reason=" + reason_code;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Canonical codecs
// ---------------------------------------------------------------------------

void encode_entity_ref(CanonicalEncoder& out, const EntityRef& value) {
  encode_value(out, static_cast<std::uint8_t>(value.kind));
  encode_value(out, value.identity);
}

EntityRef decode_entity_ref(CanonicalDecoder& in) {
  EntityRef value;
  const std::uint8_t raw_kind = in.get_u8();
  if (!in.ok()) {
    return value;
  }
  if (raw_kind > static_cast<std::uint8_t>(EntityKind::contract)) {
    in.fail(ErrorCode::malformed_input, "entity kind out of range during decode");
    return value;
  }
  value.kind = static_cast<EntityKind>(raw_kind);
  value.identity = in.get_text();
  return value;
}

namespace {

void encode_entity_state(CanonicalEncoder& out, EntityState state) {
  encode_value(out, static_cast<std::uint8_t>(state));
}

EntityState decode_entity_state(CanonicalDecoder& in) {
  const std::uint8_t raw = in.get_u8();
  if (!in.ok()) {
    return EntityState::present;
  }
  if (raw > static_cast<std::uint8_t>(EntityState::removed)) {
    in.fail(ErrorCode::malformed_input, "entity state out of range during decode");
    return EntityState::present;
  }
  return static_cast<EntityState>(raw);
}

void encode_route_state(CanonicalEncoder& out, RouteState state) {
  encode_value(out, static_cast<std::uint8_t>(state));
}

RouteState decode_route_state(CanonicalDecoder& in) {
  const std::uint8_t raw = in.get_u8();
  if (!in.ok()) {
    return RouteState::inactive;
  }
  if (raw > static_cast<std::uint8_t>(RouteState::active)) {
    in.fail(ErrorCode::malformed_input, "route state out of range during decode");
    return RouteState::inactive;
  }
  return static_cast<RouteState>(raw);
}

}  // namespace

void encode_operation(CanonicalEncoder& out, const Operation& value) {
  out.put_tag("operation");
  encode_value(out, static_cast<std::uint8_t>(value.kind));
  encode_value(out, value.device);
  encode_device_spec(out, value.device_spec);
  encode_list(out, value.exclusions, encode_exclusion_window);
  encode_list(out, value.maintenance_windows, encode_maintenance_window);
  encode_entity_state(out, value.entity_state);
  encode_value(out, value.capacity);
  encode_value(out, value.link);
  encode_link_spec(out, value.link_spec);
  encode_value(out, value.route);
  encode_route_spec(out, value.route_spec);
  encode_route_state(out, value.route_state);
  encode_value(out, value.workload);
  encode_list(out, value.bindings, encode_route_binding);
  encode_workload_requirements(out, value.requirements);
  encode_value(out, value.contract_code);
  encode_value(out, value.policy);
  encode_policy_target(out, value.policy_target);
  encode_value(out, value.value_code);
  encode_value(out, value.enforced);
  encode_value(out, value.config_key);
  encode_value(out, value.window);
  encode_value(out, value.estimated_duration);
  encode_value(out, value.risk);
  encode_value(out, value.reason_code);
}

Operation decode_operation(CanonicalDecoder& in) {
  Operation value;
  in.get_tag("operation");
  const std::uint8_t raw_kind = in.get_u8();
  if (!in.ok()) {
    return value;
  }
  if (raw_kind > static_cast<std::uint8_t>(OperationKind::exit_maintenance)) {
    in.fail(ErrorCode::malformed_input, "operation kind out of range during decode");
    return value;
  }
  value.kind = static_cast<OperationKind>(raw_kind);
  value.device = decode_value<DeviceId>(in);
  value.device_spec = decode_device_spec(in);
  value.exclusions = decode_list<ExclusionWindow>(in, 8, decode_exclusion_window);
  value.maintenance_windows = decode_list<MaintenanceWindow>(in, 8, decode_maintenance_window);
  value.entity_state = decode_entity_state(in);
  value.capacity = decode_value<CapacityUnits>(in);
  value.link = decode_value<LinkId>(in);
  value.link_spec = decode_link_spec(in);
  value.route = decode_value<RouteId>(in);
  value.route_spec = decode_route_spec(in);
  value.route_state = decode_route_state(in);
  value.workload = decode_value<WorkloadId>(in);
  value.bindings = decode_list<RouteBinding>(in, 9, decode_route_binding);
  value.requirements = decode_workload_requirements(in);
  value.contract_code = in.get_text();
  value.policy = decode_value<PolicyId>(in);
  value.policy_target = decode_policy_target(in);
  value.value_code = in.get_text();
  value.enforced = in.get_bool();
  value.config_key = decode_value<ConfigKey>(in);
  value.window = decode_value<WindowId>(in);
  value.estimated_duration = decode_value<DurationNs>(in);
  value.risk = decode_value<RiskScore>(in);
  value.reason_code = in.get_text();
  return value;
}

void encode_condition(CanonicalEncoder& out, const Condition& value) {
  out.put_tag("condition");
  encode_value(out, static_cast<std::uint8_t>(value.kind));
  encode_entity_ref(out, value.subject);
  encode_value(out, value.parameter);
  encode_value(out, value.quantity);
}

Condition decode_condition(CanonicalDecoder& in) {
  Condition value;
  in.get_tag("condition");
  const std::uint8_t raw_kind = in.get_u8();
  if (!in.ok()) {
    return value;
  }
  if (raw_kind > static_cast<std::uint8_t>(ConditionKind::device_drained)) {
    in.fail(ErrorCode::malformed_input, "condition kind out of range during decode");
    return value;
  }
  value.kind = static_cast<ConditionKind>(raw_kind);
  value.subject = decode_entity_ref(in);
  value.parameter = in.get_text();
  value.quantity = in.get_u64();
  return value;
}

void Operation::encode(CanonicalEncoder& out) const { encode_operation(out, *this); }

Operation Operation::decode(CanonicalDecoder& in) { return decode_operation(in); }

void Condition::encode(CanonicalEncoder& out) const { encode_condition(out, *this); }

Condition Condition::decode(CanonicalDecoder& in) { return decode_condition(in); }

std::string Condition::to_string() const {
  std::string out = cplan::to_string(kind);
  out += "(";
  out += subject.to_string();
  if (!parameter.empty()) {
    out += ", " + parameter;
  }
  if (quantity != 0) {
    out += ", " + std::to_string(quantity);
  }
  out += ")";
  return out;
}

}  // namespace cplan
