#include "change_planner/model/state.hpp"

#include <algorithm>
#include <set>

#include "change_planner/core/checked.hpp"

namespace cplan {
namespace {

std::string decode_token(CanonicalDecoder& in, std::size_t max_bytes, const char* field) {
  std::string value = in.get_text();
  if (!in.ok()) {
    return value;
  }
  if (value.size() > max_bytes) {
    in.fail(ErrorCode::size_limit, std::string("token field too long: ") + field);
    return {};
  }
  return value;
}

void encode_capability_set(CanonicalEncoder& out, const CapabilitySet& value) {
  out.put_u32(value.mask());
}

CapabilitySet decode_capability_set(CanonicalDecoder& in) {
  return CapabilitySet(in.get_u32());
}

std::uint8_t decode_enum_byte(CanonicalDecoder& in, std::uint8_t max_value, const char* name) {
  const std::uint8_t value = in.get_u8();
  if (!in.ok()) {
    return 0;
  }
  if (value > max_value) {
    in.fail(ErrorCode::malformed_input, std::string("enum out of range during decode: ") + name);
    return 0;
  }
  return value;
}

template <class T>
void sort_by_identity(std::vector<T>& values) {
  std::sort(values.begin(), values.end(), [](const T& lhs, const T& rhs) { return lhs.id < rhs.id; });
}

// Order-independent duplicate detection: input states are not required to be
// canonicalized before validation.
template <class T>
bool has_duplicate_ids(const std::vector<T>& values) {
  std::set<std::string> seen;
  for (const T& value : values) {
    if (!seen.insert(value.id.str()).second) {
      return true;
    }
  }
  return false;
}

Status duplicate(const char* kind, const std::string& identity) {
  return Status::error(ErrorCode::duplicate_identity,
                       std::string("duplicate ") + kind + " identity: " + identity);
}

Status dangling(const char* kind, const std::string& identity, const std::string& missing) {
  return Status::error(ErrorCode::invalid_argument,
                       std::string(kind) + " '" + identity + "' references missing " + missing);
}

void sort_unique_domains(std::vector<FailureDomainId>& domains) {
  std::sort(domains.begin(), domains.end());
  domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum tokens
// ---------------------------------------------------------------------------

const char* to_string(EntityState state) noexcept {
  switch (state) {
    case EntityState::absent:
      return "absent";
    case EntityState::present:
      return "present";
    case EntityState::draining:
      return "draining";
    case EntityState::in_maintenance:
      return "in-maintenance";
    case EntityState::removed:
      return "removed";
  }
  return "unknown";
}

Result<EntityState> parse_entity_state(std::string_view token) {
  if (token == "absent") return EntityState::absent;
  if (token == "present") return EntityState::present;
  if (token == "draining") return EntityState::draining;
  if (token == "in-maintenance") return EntityState::in_maintenance;
  if (token == "removed") return EntityState::removed;
  return Status::error(ErrorCode::malformed_input,
                       "unknown entity state token: " + std::string(token));
}

const char* to_string(RouteState state) noexcept {
  switch (state) {
    case RouteState::inactive:
      return "inactive";
    case RouteState::active:
      return "active";
  }
  return "unknown";
}

Result<RouteState> parse_route_state(std::string_view token) {
  if (token == "inactive") return RouteState::inactive;
  if (token == "active") return RouteState::active;
  return Status::error(ErrorCode::malformed_input,
                       "unknown route state token: " + std::string(token));
}

const char* to_string(PolicyScope scope) noexcept {
  switch (scope) {
    case PolicyScope::global:
      return "global";
    case PolicyScope::device:
      return "device";
    case PolicyScope::link:
      return "link";
    case PolicyScope::route:
      return "route";
    case PolicyScope::workload:
      return "workload";
  }
  return "unknown";
}

Result<PolicyScope> parse_policy_scope(std::string_view token) {
  if (token == "global") return PolicyScope::global;
  if (token == "device") return PolicyScope::device;
  if (token == "link") return PolicyScope::link;
  if (token == "route") return PolicyScope::route;
  if (token == "workload") return PolicyScope::workload;
  return Status::error(ErrorCode::malformed_input,
                       "unknown policy scope token: " + std::string(token));
}

const char* to_string(ChangeCapability capability) noexcept {
  switch (capability) {
    case ChangeCapability::none:
      return "none";
    case ChangeCapability::device_add:
      return "device-add";
    case ChangeCapability::device_remove:
      return "device-remove";
    case ChangeCapability::device_capacity:
      return "device-capacity";
    case ChangeCapability::device_state:
      return "device-state";
    case ChangeCapability::device_drain:
      return "device-drain";
    case ChangeCapability::device_maintenance:
      return "device-maintenance";
    case ChangeCapability::link_add:
      return "link-add";
    case ChangeCapability::link_remove:
      return "link-remove";
    case ChangeCapability::link_capacity:
      return "link-capacity";
    case ChangeCapability::route_add:
      return "route-add";
    case ChangeCapability::route_remove:
      return "route-remove";
    case ChangeCapability::route_state:
      return "route-state";
    case ChangeCapability::policy_bind:
      return "policy-bind";
    case ChangeCapability::config_set:
      return "config-set";
    case ChangeCapability::workload_binding:
      return "workload-binding";
    case ChangeCapability::workload_requirements:
      return "workload-requirements";
  }
  return "unknown";
}

Result<ChangeCapability> parse_change_capability(std::string_view token) {
  if (token == "none") return ChangeCapability::none;
  if (token == "device-add") return ChangeCapability::device_add;
  if (token == "device-remove") return ChangeCapability::device_remove;
  if (token == "device-capacity") return ChangeCapability::device_capacity;
  if (token == "device-state") return ChangeCapability::device_state;
  if (token == "device-drain") return ChangeCapability::device_drain;
  if (token == "device-maintenance") return ChangeCapability::device_maintenance;
  if (token == "link-add") return ChangeCapability::link_add;
  if (token == "link-remove") return ChangeCapability::link_remove;
  if (token == "link-capacity") return ChangeCapability::link_capacity;
  if (token == "route-add") return ChangeCapability::route_add;
  if (token == "route-remove") return ChangeCapability::route_remove;
  if (token == "route-state") return ChangeCapability::route_state;
  if (token == "policy-bind") return ChangeCapability::policy_bind;
  if (token == "config-set") return ChangeCapability::config_set;
  if (token == "workload-binding") return ChangeCapability::workload_binding;
  if (token == "workload-requirements") return ChangeCapability::workload_requirements;
  return Status::error(ErrorCode::malformed_input,
                       "unknown capability token: " + std::string(token));
}

std::string CapabilitySet::to_string() const {
  if (mask_ == 0) {
    return "none";
  }
  static constexpr ChangeCapability kAll[] = {
      ChangeCapability::device_add,     ChangeCapability::device_remove,
      ChangeCapability::device_capacity, ChangeCapability::device_state,
      ChangeCapability::device_drain,   ChangeCapability::device_maintenance,
      ChangeCapability::link_add,       ChangeCapability::link_remove,
      ChangeCapability::link_capacity,  ChangeCapability::route_add,
      ChangeCapability::route_remove,   ChangeCapability::route_state,
      ChangeCapability::policy_bind,    ChangeCapability::config_set,
      ChangeCapability::workload_binding, ChangeCapability::workload_requirements};
  std::string out;
  for (const ChangeCapability capability : kAll) {
    if (!has(capability)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += cplan::to_string(capability);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Canonical codecs
// ---------------------------------------------------------------------------

void encode_exclusion_window(CanonicalEncoder& out, const ExclusionWindow& value) {
  out.put_tag("excl-window");
  encode_value(out, value.id);
  encode_value(out, value.begin);
  encode_value(out, value.end);
  encode_value(out, value.reason_code);
}

ExclusionWindow decode_exclusion_window(CanonicalDecoder& in) {
  ExclusionWindow value;
  in.get_tag("excl-window");
  value.id = decode_value<WindowId>(in);
  value.begin = decode_value<TimestampNs>(in);
  value.end = decode_value<TimestampNs>(in);
  value.reason_code = decode_token(in, kMaxTokenBytes, "exclusion-reason");
  return value;
}

void encode_maintenance_window(CanonicalEncoder& out, const MaintenanceWindow& value) {
  out.put_tag("maint-window");
  encode_value(out, value.id);
  encode_value(out, value.begin);
  encode_value(out, value.end);
  encode_value(out, value.ticket_code);
}

MaintenanceWindow decode_maintenance_window(CanonicalDecoder& in) {
  MaintenanceWindow value;
  in.get_tag("maint-window");
  value.id = decode_value<WindowId>(in);
  value.begin = decode_value<TimestampNs>(in);
  value.end = decode_value<TimestampNs>(in);
  value.ticket_code = decode_token(in, kMaxTokenBytes, "maintenance-ticket");
  return value;
}

void encode_device_spec(CanonicalEncoder& out, const DeviceSpec& value) {
  encode_value(out, value.domain);
  encode_value(out, static_cast<std::uint8_t>(value.state));
  encode_value(out, value.termination_capacity);
  encode_value(out, value.termination_reserved);
  encode_capability_set(out, value.capabilities);
  encode_value(out, value.max_concurrent_changes);
  encode_value(out, value.profile_code);
}

DeviceSpec decode_device_spec(CanonicalDecoder& in) {
  DeviceSpec value;
  value.domain = decode_value<FailureDomainId>(in);
  value.state = static_cast<EntityState>(
      decode_enum_byte(in, static_cast<std::uint8_t>(EntityState::removed), "entity-state"));
  value.termination_capacity = decode_value<CapacityUnits>(in);
  value.termination_reserved = decode_value<CapacityUnits>(in);
  value.capabilities = decode_capability_set(in);
  value.max_concurrent_changes = in.get_u32();
  value.profile_code = decode_token(in, kMaxTokenBytes, "device-profile");
  return value;
}

void encode_device(CanonicalEncoder& out, const Device& value) {
  out.put_tag("device");
  encode_value(out, value.id);
  encode_device_spec(out, value.spec);
  encode_list(out, value.exclusions, encode_exclusion_window);
  encode_list(out, value.maintenance_windows, encode_maintenance_window);
}

Device decode_device(CanonicalDecoder& in) {
  Device value;
  in.get_tag("device");
  value.id = decode_value<DeviceId>(in);
  value.spec = decode_device_spec(in);
  value.exclusions = decode_list<ExclusionWindow>(in, 8, decode_exclusion_window);
  value.maintenance_windows = decode_list<MaintenanceWindow>(in, 8, decode_maintenance_window);
  return value;
}

void encode_link_spec(CanonicalEncoder& out, const LinkSpec& value) {
  encode_value(out, value.endpoint_a);
  encode_value(out, value.endpoint_b);
  encode_value(out, value.capacity);
  encode_value(out, value.reserved);
  encode_value(out, value.latency_class);
  encode_sequence(out, value.transit_domains);
}

LinkSpec decode_link_spec(CanonicalDecoder& in) {
  LinkSpec value;
  value.endpoint_a = decode_value<DeviceId>(in);
  value.endpoint_b = decode_value<DeviceId>(in);
  value.capacity = decode_value<CapacityUnits>(in);
  value.reserved = decode_value<CapacityUnits>(in);
  value.latency_class = in.get_u32();
  value.transit_domains = decode_sequence<FailureDomainId>(in, 4);
  return value;
}

void encode_link(CanonicalEncoder& out, const Link& value) {
  out.put_tag("link");
  encode_value(out, value.id);
  encode_link_spec(out, value.spec);
  encode_value(out, static_cast<std::uint8_t>(value.state));
}

Link decode_link(CanonicalDecoder& in) {
  Link value;
  in.get_tag("link");
  value.id = decode_value<LinkId>(in);
  value.spec = decode_link_spec(in);
  value.state = static_cast<EntityState>(
      decode_enum_byte(in, static_cast<std::uint8_t>(EntityState::removed), "entity-state"));
  return value;
}

void encode_route_spec(CanonicalEncoder& out, const RouteSpec& value) {
  encode_value(out, value.source);
  encode_value(out, value.sink);
  encode_sequence(out, value.path);
  encode_value(out, value.policy);
}

RouteSpec decode_route_spec(CanonicalDecoder& in) {
  RouteSpec value;
  value.source = decode_value<DeviceId>(in);
  value.sink = decode_value<DeviceId>(in);
  value.path = decode_sequence<LinkId>(in, 4);
  value.policy = decode_value<PolicyId>(in);
  return value;
}

void encode_route(CanonicalEncoder& out, const Route& value) {
  out.put_tag("route");
  encode_value(out, value.id);
  encode_route_spec(out, value.spec);
  encode_value(out, static_cast<std::uint8_t>(value.state));
}

Route decode_route(CanonicalDecoder& in) {
  Route value;
  in.get_tag("route");
  value.id = decode_value<RouteId>(in);
  value.spec = decode_route_spec(in);
  value.state = static_cast<RouteState>(
      decode_enum_byte(in, static_cast<std::uint8_t>(RouteState::active), "route-state"));
  return value;
}

void encode_route_binding(CanonicalEncoder& out, const RouteBinding& value) {
  out.put_tag("route-binding");
  encode_value(out, value.route);
  encode_value(out, value.active);
  encode_value(out, value.demand);
}

RouteBinding decode_route_binding(CanonicalDecoder& in) {
  RouteBinding value;
  in.get_tag("route-binding");
  value.route = decode_value<RouteId>(in);
  value.active = in.get_bool();
  value.demand = decode_value<DemandUnits>(in);
  return value;
}

void encode_workload_requirements(CanonicalEncoder& out, const WorkloadRequirements& value) {
  encode_value(out, value.require_connectivity);
  encode_value(out, value.min_disjoint_domains);
  encode_value(out, value.min_link_disjoint_paths);
  encode_value(out, value.min_guaranteed_capacity);
  encode_sequence(out, value.forbidden_domains);
}

WorkloadRequirements decode_workload_requirements(CanonicalDecoder& in) {
  WorkloadRequirements value;
  value.require_connectivity = in.get_bool();
  value.min_disjoint_domains = in.get_u32();
  value.min_link_disjoint_paths = in.get_u32();
  value.min_guaranteed_capacity = decode_value<CapacityUnits>(in);
  value.forbidden_domains = decode_sequence<FailureDomainId>(in, 4);
  return value;
}

void encode_workload(CanonicalEncoder& out, const Workload& value) {
  out.put_tag("workload");
  encode_value(out, value.id);
  encode_list(out, value.bindings, encode_route_binding);
  encode_workload_requirements(out, value.requirements);
  encode_value(out, value.contract_code);
}

Workload decode_workload(CanonicalDecoder& in) {
  Workload value;
  in.get_tag("workload");
  value.id = decode_value<WorkloadId>(in);
  value.bindings = decode_list<RouteBinding>(in, 9, decode_route_binding);
  value.requirements = decode_workload_requirements(in);
  value.contract_code = decode_token(in, kMaxTokenBytes, "workload-contract");
  return value;
}

void encode_policy_target(CanonicalEncoder& out, const PolicyTarget& value) {
  encode_value(out, static_cast<std::uint8_t>(value.scope));
  encode_value(out, value.identity);
}

PolicyTarget decode_policy_target(CanonicalDecoder& in) {
  PolicyTarget value;
  value.scope = static_cast<PolicyScope>(
      decode_enum_byte(in, static_cast<std::uint8_t>(PolicyScope::workload), "policy-scope"));
  value.identity = decode_token(in, cplan::kMaxIdentityLength, "policy-target");
  return value;
}

void encode_policy_object(CanonicalEncoder& out, const PolicyObject& value) {
  out.put_tag("policy-object");
  encode_value(out, value.id);
  encode_policy_target(out, value.target);
  encode_value(out, value.value_code);
  encode_value(out, value.enforced);
}

PolicyObject decode_policy_object(CanonicalDecoder& in) {
  PolicyObject value;
  in.get_tag("policy-object");
  value.id = decode_value<PolicyId>(in);
  value.target = decode_policy_target(in);
  value.value_code = decode_token(in, kMaxTokenBytes, "policy-value");
  value.enforced = in.get_bool();
  return value;
}

void encode_config_entry(CanonicalEncoder& out, const ConfigEntry& value) {
  out.put_tag("config-entry");
  encode_value(out, value.device);
  encode_value(out, value.key);
  encode_value(out, value.value_code);
  encode_value(out, value.revision);
}

ConfigEntry decode_config_entry(CanonicalDecoder& in) {
  ConfigEntry value;
  in.get_tag("config-entry");
  value.device = decode_value<DeviceId>(in);
  value.key = decode_value<ConfigKey>(in);
  value.value_code = decode_token(in, kMaxTokenBytes, "config-value");
  value.revision = decode_value<ConfigRevision>(in);
  return value;
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

void CurrentStateSnapshot::canonicalize() {
  sort_by_identity(devices);
  sort_by_identity(links);
  sort_by_identity(routes);
  sort_by_identity(workloads);
  sort_by_identity(policies);
  std::sort(configs.begin(), configs.end());
  for (Device& device : devices) {
    std::sort(device.exclusions.begin(), device.exclusions.end());
    std::sort(device.maintenance_windows.begin(), device.maintenance_windows.end());
  }
  for (Link& link : links) {
    sort_unique_domains(link.spec.transit_domains);
  }
  for (Workload& workload : workloads) {
    std::sort(workload.bindings.begin(), workload.bindings.end());
    sort_unique_domains(workload.requirements.forbidden_domains);
  }
}

Result<void> CurrentStateSnapshot::validate() const {
  if (authority.empty()) {
    return Status::error(ErrorCode::invalid_argument, "snapshot authority identity is empty");
  }
  if (has_duplicate_ids(devices)) {
    return duplicate("device", "<snapshot>");
  }
  if (has_duplicate_ids(links)) {
    return duplicate("link", "<snapshot>");
  }
  if (has_duplicate_ids(routes)) {
    return duplicate("route", "<snapshot>");
  }
  if (has_duplicate_ids(workloads)) {
    return duplicate("workload", "<snapshot>");
  }
  if (has_duplicate_ids(policies)) {
    return duplicate("policy", "<snapshot>");
  }

  for (const Device& device : devices) {
    if (device.spec.capabilities.mask() != 0 &&
        device.spec.termination_capacity.value() == 0) {
      // A device with capabilities but no termination capacity can still be a
      // pure control-plane element; this is legal, so nothing to reject.
    }
    if (device.spec.profile_code.size() > kMaxTokenBytes) {
      return Status::error(ErrorCode::size_limit, "device profile code too long");
    }
    std::set<WindowId> window_ids;
    for (const ExclusionWindow& window : device.exclusions) {
      if (window.end <= window.begin) {
        return Status::error(ErrorCode::invalid_argument,
                             "exclusion window on device '" + device.id.str() +
                                 "' has a non-positive duration");
      }
      if (!window_ids.insert(window.id).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "duplicate window identity on device '" + device.id.str() + "'");
      }
    }
    for (const MaintenanceWindow& window : device.maintenance_windows) {
      if (window.end <= window.begin) {
        return Status::error(ErrorCode::invalid_argument,
                             "maintenance window on device '" + device.id.str() +
                                 "' has a non-positive duration");
      }
      if (!window_ids.insert(window.id).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "duplicate window identity on device '" + device.id.str() + "'");
      }
    }
  }

  for (const Link& link : links) {
    if (link.spec.endpoint_a == link.spec.endpoint_b) {
      return Status::error(ErrorCode::invalid_argument,
                           "link '" + link.id.str() + "' has identical endpoints");
    }
    if (find_device(link.spec.endpoint_a) == nullptr) {
      return dangling("link", link.id.str(), "device '" + link.spec.endpoint_a.str() + "'");
    }
    if (find_device(link.spec.endpoint_b) == nullptr) {
      return dangling("link", link.id.str(), "device '" + link.spec.endpoint_b.str() + "'");
    }
    if (link.spec.capacity.value() < link.spec.reserved.value()) {
      return Status::error(ErrorCode::capacity_exceeded,
                           "link '" + link.id.str() + "' reserved capacity exceeds capacity");
    }
  }

  for (const Route& route : routes) {
    if (find_device(route.spec.source) == nullptr) {
      return dangling("route", route.id.str(), "device '" + route.spec.source.str() + "'");
    }
    if (find_device(route.spec.sink) == nullptr) {
      return dangling("route", route.id.str(), "device '" + route.spec.sink.str() + "'");
    }
    if (route.spec.path.empty()) {
      return Status::error(ErrorCode::invalid_argument,
                           "route '" + route.id.str() + "' has an empty path");
    }
    for (const LinkId& link_id : route.spec.path) {
      if (find_link(link_id) == nullptr) {
        return dangling("route", route.id.str(), "link '" + link_id.str() + "'");
      }
    }
    if (!route.spec.policy.empty() && find_policy(route.spec.policy) == nullptr) {
      return dangling("route", route.id.str(), "policy '" + route.spec.policy.str() + "'");
    }
  }

  for (const Workload& workload : workloads) {
    if (workload.contract_code.size() > kMaxTokenBytes) {
      return Status::error(ErrorCode::size_limit, "workload contract code too long");
    }
    std::set<RouteId> bound;
    for (const RouteBinding& binding : workload.bindings) {
      if (find_route(binding.route) == nullptr) {
        return dangling("workload", workload.id.str(), "route '" + binding.route.str() + "'");
      }
      if (!bound.insert(binding.route).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "workload '" + workload.id.str() + "' binds route '" +
                                 binding.route.str() + "' more than once");
      }
    }
  }

  for (const PolicyObject& policy : policies) {
    if (policy.value_code.size() > kMaxTokenBytes) {
      return Status::error(ErrorCode::size_limit, "policy value code too long");
    }
    switch (policy.target.scope) {
      case PolicyScope::global:
        break;
      case PolicyScope::device: {
        const auto target = DeviceId::parse(policy.target.identity);
        if (!target.ok()) {
          return target.status();
        }
        if (find_device(target.value()) == nullptr) {
          return dangling("policy", policy.id.str(), "device '" + policy.target.identity + "'");
        }
        break;
      }
      case PolicyScope::link: {
        const auto target = LinkId::parse(policy.target.identity);
        if (!target.ok()) {
          return target.status();
        }
        if (find_link(target.value()) == nullptr) {
          return dangling("policy", policy.id.str(), "link '" + policy.target.identity + "'");
        }
        break;
      }
      case PolicyScope::route: {
        const auto target = RouteId::parse(policy.target.identity);
        if (!target.ok()) {
          return target.status();
        }
        if (find_route(target.value()) == nullptr) {
          return dangling("policy", policy.id.str(), "route '" + policy.target.identity + "'");
        }
        break;
      }
      case PolicyScope::workload: {
        const auto target = WorkloadId::parse(policy.target.identity);
        if (!target.ok()) {
          return target.status();
        }
        if (find_workload(target.value()) == nullptr) {
          return dangling("policy", policy.id.str(), "workload '" + policy.target.identity + "'");
        }
        break;
      }
    }
  }

  {
    std::set<std::pair<DeviceId, ConfigKey>> seen;
    for (const ConfigEntry& entry : configs) {
      if (find_device(entry.device) == nullptr) {
        return dangling("config", entry.key.str(), "device '" + entry.device.str() + "'");
      }
      if (entry.value_code.size() > kMaxTokenBytes) {
        return Status::error(ErrorCode::size_limit, "config value code too long");
      }
      if (!seen.insert({entry.device, entry.key}).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "duplicate config key '" + entry.key.str() + "' on device '" +
                                 entry.device.str() + "'");
      }
    }
  }

  return Status::success();
}

Digest CurrentStateSnapshot::digest() const {
  CanonicalEncoder encoder;
  encode(encoder);
  return encoder.digest();
}

void CurrentStateSnapshot::encode(CanonicalEncoder& out) const {
  out.put_tag("cplan.state.v1");
  encode_value(out, topology_generation);
  encode_value(out, authority);
  encode_value(out, authority_generation);
  encode_value(out, boot_incarnation);
  encode_value(out, epoch);
  encode_value(out, observed_at);
  encode_list(out, devices, encode_device);
  encode_list(out, links, encode_link);
  encode_list(out, routes, encode_route);
  encode_list(out, workloads, encode_workload);
  encode_list(out, policies, encode_policy_object);
  encode_list(out, configs, encode_config_entry);
}

CurrentStateSnapshot CurrentStateSnapshot::decode(CanonicalDecoder& in) {
  CurrentStateSnapshot state;
  in.get_tag("cplan.state.v1");
  state.topology_generation = decode_value<TopologyGeneration>(in);
  state.authority = decode_value<AuthorityId>(in);
  state.authority_generation = decode_value<AuthorityGeneration>(in);
  state.boot_incarnation = decode_value<BootIncarnation>(in);
  state.epoch = decode_value<Epoch>(in);
  state.observed_at = decode_value<TimestampNs>(in);
  state.devices = decode_list<Device>(in, 8, decode_device);
  state.links = decode_list<Link>(in, 8, decode_link);
  state.routes = decode_list<Route>(in, 8, decode_route);
  state.workloads = decode_list<Workload>(in, 8, decode_workload);
  state.policies = decode_list<PolicyObject>(in, 8, decode_policy_object);
  state.configs = decode_list<ConfigEntry>(in, 8, decode_config_entry);
  return state;
}

const Device* CurrentStateSnapshot::find_device(const DeviceId& id) const {
  for (const Device& device : devices) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

const Link* CurrentStateSnapshot::find_link(const LinkId& id) const {
  for (const Link& link : links) {
    if (link.id == id) {
      return &link;
    }
  }
  return nullptr;
}

const Route* CurrentStateSnapshot::find_route(const RouteId& id) const {
  for (const Route& route : routes) {
    if (route.id == id) {
      return &route;
    }
  }
  return nullptr;
}

const Workload* CurrentStateSnapshot::find_workload(const WorkloadId& id) const {
  for (const Workload& workload : workloads) {
    if (workload.id == id) {
      return &workload;
    }
  }
  return nullptr;
}

const PolicyObject* CurrentStateSnapshot::find_policy(const PolicyId& id) const {
  for (const PolicyObject& policy : policies) {
    if (policy.id == id) {
      return &policy;
    }
  }
  return nullptr;
}

const ConfigEntry* CurrentStateSnapshot::find_config(const DeviceId& device,
                                                     const ConfigKey& key) const {
  for (const ConfigEntry& entry : configs) {
    if (entry.device == device && entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// State index
// ---------------------------------------------------------------------------

StateIndex::StateIndex(const CurrentStateSnapshot& state) : state_(&state) {
  for (std::size_t i = 0; i < state.devices.size(); ++i) {
    device_slots_.emplace(state.devices[i].id, i);
  }
  for (std::size_t i = 0; i < state.links.size(); ++i) {
    link_slots_.emplace(state.links[i].id, i);
  }
  for (std::size_t i = 0; i < state.routes.size(); ++i) {
    route_slots_.emplace(state.routes[i].id, i);
  }
  for (std::size_t i = 0; i < state.workloads.size(); ++i) {
    workload_slots_.emplace(state.workloads[i].id, i);
  }
  for (std::size_t i = 0; i < state.policies.size(); ++i) {
    policy_slots_.emplace(state.policies[i].id, i);
  }
  for (std::size_t i = 0; i < state.configs.size(); ++i) {
    config_slots_.emplace(std::make_pair(state.configs[i].device, state.configs[i].key), i);
  }
}

const Device* StateIndex::device(const DeviceId& id) const {
  const auto slot = device_slots_.find(id);
  return slot == device_slots_.end() ? nullptr : &state_->devices[slot->second];
}

const Link* StateIndex::link(const LinkId& id) const {
  const auto slot = link_slots_.find(id);
  return slot == link_slots_.end() ? nullptr : &state_->links[slot->second];
}

const Route* StateIndex::route(const RouteId& id) const {
  const auto slot = route_slots_.find(id);
  return slot == route_slots_.end() ? nullptr : &state_->routes[slot->second];
}

const Workload* StateIndex::workload(const WorkloadId& id) const {
  const auto slot = workload_slots_.find(id);
  return slot == workload_slots_.end() ? nullptr : &state_->workloads[slot->second];
}

const PolicyObject* StateIndex::policy(const PolicyId& id) const {
  const auto slot = policy_slots_.find(id);
  return slot == policy_slots_.end() ? nullptr : &state_->policies[slot->second];
}

const ConfigEntry* StateIndex::config(const DeviceId& device_id, const ConfigKey& key) const {
  const auto slot = config_slots_.find({device_id, key});
  return slot == config_slots_.end() ? nullptr : &state_->configs[slot->second];
}

std::vector<DeviceId> StateIndex::devices_in_domain(const FailureDomainId& domain) const {
  std::vector<DeviceId> result;
  for (const Device& device : state_->devices) {
    if (device.spec.domain == domain) {
      result.push_back(device.id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<RouteId> StateIndex::workload_routes(const WorkloadId& id) const {
  std::vector<RouteId> result;
  const Workload* workload = this->workload(id);
  if (workload == nullptr) {
    return result;
  }
  for (const RouteBinding& binding : workload->bindings) {
    result.push_back(binding.route);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<LinkId> StateIndex::incident_links(const DeviceId& id) const {
  std::vector<LinkId> result;
  for (const Link& link : state_->links) {
    if (link.connects(id)) {
      result.push_back(link.id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<WorkloadId> StateIndex::workloads_on_link(const LinkId& id) const {
  std::vector<WorkloadId> result;
  for (const Workload& workload : state_->workloads) {
    for (const RouteBinding& binding : workload.bindings) {
      const Route* route = this->route(binding.route);
      if (route == nullptr) {
        continue;
      }
      if (std::find(route->spec.path.begin(), route->spec.path.end(), id) !=
          route->spec.path.end()) {
        result.push_back(workload.id);
        break;
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<WorkloadId> StateIndex::workloads_on_device(const DeviceId& id) const {
  std::vector<WorkloadId> result;
  for (const Workload& workload : state_->workloads) {
    for (const RouteBinding& binding : workload.bindings) {
      const Route* route = this->route(binding.route);
      if (route == nullptr) {
        continue;
      }
      if (route->spec.source == id || route->spec.sink == id) {
        result.push_back(workload.id);
        break;
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

// ---------------------------------------------------------------------------
// Derived helpers
// ---------------------------------------------------------------------------

bool route_is_walkable(const CurrentStateSnapshot& state, const Route& route) {
  if (route.spec.path.empty()) {
    return false;
  }
  DeviceId current = route.spec.source;
  for (const LinkId& link_id : route.spec.path) {
    const Link* link = state.find_link(link_id);
    if (link == nullptr) {
      return false;
    }
    if (link->spec.endpoint_a == current) {
      current = link->spec.endpoint_b;
    } else if (link->spec.endpoint_b == current) {
      current = link->spec.endpoint_a;
    } else {
      return false;
    }
  }
  return current == route.spec.sink;
}

std::vector<FailureDomainId> route_domains(const CurrentStateSnapshot& state, const Route& route) {
  std::vector<FailureDomainId> domains;
  const Device* source = state.find_device(route.spec.source);
  if (source != nullptr) {
    domains.push_back(source->spec.domain);
  }
  const Device* sink = state.find_device(route.spec.sink);
  if (sink != nullptr) {
    domains.push_back(sink->spec.domain);
  }
  for (const LinkId& link_id : route.spec.path) {
    const Link* link = state.find_link(link_id);
    if (link == nullptr) {
      continue;
    }
    for (const DeviceId& endpoint : {link->spec.endpoint_a, link->spec.endpoint_b}) {
      const Device* device = state.find_device(endpoint);
      if (device != nullptr) {
        domains.push_back(device->spec.domain);
      }
    }
    for (const FailureDomainId& domain : link->spec.transit_domains) {
      domains.push_back(domain);
    }
  }
  sort_unique_domains(domains);
  return domains;
}

DemandUnits workload_demand_per_binding(const Workload& workload, const RouteBinding& binding) {
  static_cast<void>(workload);
  return binding.active ? binding.demand : DemandUnits(0);
}

}  // namespace cplan
