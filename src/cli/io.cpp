#include "change_planner/cli/io.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "change_planner/persist/artifact.hpp"
#include "change_planner/version.hpp"

namespace cplan::cli {
namespace {

Status document_error(const std::string& message) {
  return Status::error(ErrorCode::malformed_input, message);
}

Result<const JsonValue*> require_member(const JsonValue& object, const char* key) {
  if (!object.is_object()) {
    return document_error(std::string("expected a JSON object while reading member '") + key + "'");
  }
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return document_error(std::string("required member '") + key + "' is missing");
  }
  return member;
}

Result<const JsonValue*> optional_member(const JsonValue& object, const char* key) {
  if (!object.is_object()) {
    return document_error(std::string("expected a JSON object while reading member '") + key + "'");
  }
  return object.find(key);
}

Result<std::string> require_string(const JsonValue& object, const char* key) {
  auto member = require_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  if (!member.value()->is_string()) {
    return document_error(std::string("member '") + key + "' must be a string");
  }
  return member.value()->as_string();
}

Result<std::string> optional_string(const JsonValue& object, const char* key,
                                    const std::string& fallback) {
  auto member = optional_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  if (member.value() == nullptr) {
    return fallback;
  }
  if (!member.value()->is_string()) {
    return document_error(std::string("member '") + key + "' must be a string");
  }
  return member.value()->as_string();
}

Result<std::uint64_t> require_uint(const JsonValue& object, const char* key) {
  auto member = require_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  const JsonValue& value = *member.value();
  if (!value.is_integer() || value.kind() == JsonKind::integer_signed) {
    if (!value.is_integer() || value.as_int64(-1) < 0) {
      return document_error(std::string("member '") + key +
                            "' must be a non-negative integer");
    }
  }
  return value.as_uint64(0);
}

Result<std::uint64_t> optional_uint(const JsonValue& object, const char* key,
                                    std::uint64_t fallback) {
  auto member = optional_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  if (member.value() == nullptr) {
    return fallback;
  }
  const JsonValue& value = *member.value();
  if (!value.is_number()) {
    return document_error(std::string("member '") + key + "' must be a number");
  }
  if (value.is_integer() && value.kind() == JsonKind::integer_signed && value.as_int64(-1) < 0) {
    return document_error(std::string("member '") + key + "' must not be negative");
  }
  return value.as_uint64(fallback);
}

Result<bool> optional_bool(const JsonValue& object, const char* key, bool fallback) {
  auto member = optional_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  if (member.value() == nullptr) {
    return fallback;
  }
  if (!member.value()->is_bool()) {
    return document_error(std::string("member '") + key + "' must be a boolean");
  }
  return member.value()->as_bool(fallback);
}

template <class Id>
Result<Id> parse_identity(const JsonValue& object, const char* key) {
  auto text = require_string(object, key);
  if (!text.ok()) {
    return text.status();
  }
  return Id::parse(text.value());
}

template <class Id>
Result<Id> optional_identity(const JsonValue& object, const char* key) {
  auto text = optional_string(object, key, "");
  if (!text.ok()) {
    return text.status();
  }
  if (text.value().empty()) {
    return Id{};
  }
  return Id::parse(text.value());
}

Result<std::vector<std::string>> optional_string_array(const JsonValue& object, const char* key) {
  auto member = optional_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  std::vector<std::string> result;
  if (member.value() == nullptr) {
    return result;
  }
  const JsonValue& value = *member.value();
  if (!value.is_array()) {
    return document_error(std::string("member '") + key + "' must be an array");
  }
  for (const JsonValue& element : value.elements()) {
    if (!element.is_string()) {
      return document_error(std::string("member '") + key + "' must contain only strings");
    }
    result.push_back(element.as_string());
  }
  return result;
}

Result<const JsonValue*> optional_array(const JsonValue& object, const char* key) {
  auto member = optional_member(object, key);
  if (!member.ok()) {
    return member.status();
  }
  if (member.value() == nullptr) {
    return static_cast<const JsonValue*>(nullptr);
  }
  if (!member.value()->is_array()) {
    return document_error(std::string("member '") + key + "' must be an array");
  }
  return member.value();
}

Result<CapabilitySet> parse_capabilities(const JsonValue& object, const char* key) {
  auto tokens = optional_string_array(object, key);
  if (!tokens.ok()) {
    return tokens.status();
  }
  CapabilitySet capabilities;
  for (const std::string& token : tokens.value()) {
    auto capability = parse_change_capability(token);
    if (!capability.ok()) {
      return capability.status();
    }
    capabilities.add(capability.value());
  }
  return capabilities;
}

Result<ExclusionWindow> parse_exclusion(const JsonValue& value) {
  ExclusionWindow window;
  auto id = parse_identity<WindowId>(value, "id");
  if (!id.ok()) return id.status();
  window.id = id.value();
  auto begin = require_uint(value, "begin");
  if (!begin.ok()) return begin.status();
  window.begin = TimestampNs(static_cast<std::int64_t>(begin.value()));
  auto end = require_uint(value, "end");
  if (!end.ok()) return end.status();
  window.end = TimestampNs(static_cast<std::int64_t>(end.value()));
  auto reason = optional_string(value, "reason", "");
  if (!reason.ok()) return reason.status();
  window.reason_code = reason.value();
  return window;
}

Result<MaintenanceWindow> parse_maintenance_window(const JsonValue& value) {
  MaintenanceWindow window;
  auto id = parse_identity<WindowId>(value, "id");
  if (!id.ok()) return id.status();
  window.id = id.value();
  auto begin = require_uint(value, "begin");
  if (!begin.ok()) return begin.status();
  window.begin = TimestampNs(static_cast<std::int64_t>(begin.value()));
  auto end = require_uint(value, "end");
  if (!end.ok()) return end.status();
  window.end = TimestampNs(static_cast<std::int64_t>(end.value()));
  auto ticket = optional_string(value, "ticket", "");
  if (!ticket.ok()) return ticket.status();
  window.ticket_code = ticket.value();
  return window;
}

Result<std::vector<FailureDomainId>> parse_domains(const JsonValue& object, const char* key) {
  auto tokens = optional_string_array(object, key);
  if (!tokens.ok()) {
    return tokens.status();
  }
  std::vector<FailureDomainId> domains;
  for (const std::string& token : tokens.value()) {
    auto domain = FailureDomainId::parse(token);
    if (!domain.ok()) {
      return domain.status();
    }
    domains.push_back(domain.value());
  }
  return domains;
}

Result<DeviceSpec> parse_device_spec(const JsonValue& value) {
  DeviceSpec spec;
  auto domain = parse_identity<FailureDomainId>(value, "domain");
  if (!domain.ok()) return domain.status();
  spec.domain = domain.value();
  auto state_token = optional_string(value, "state", "present");
  if (!state_token.ok()) return state_token.status();
  auto state = parse_entity_state(state_token.value());
  if (!state.ok()) return state.status();
  spec.state = state.value();
  auto capacity = optional_uint(value, "terminationCapacity", 0);
  if (!capacity.ok()) return capacity.status();
  spec.termination_capacity = CapacityUnits(capacity.value());
  auto reserved = optional_uint(value, "terminationReserved", 0);
  if (!reserved.ok()) return reserved.status();
  spec.termination_reserved = CapacityUnits(reserved.value());
  auto capabilities = parse_capabilities(value, "capabilities");
  if (!capabilities.ok()) return capabilities.status();
  spec.capabilities = capabilities.value();
  auto concurrency = optional_uint(value, "maxConcurrentChanges", 1);
  if (!concurrency.ok()) return concurrency.status();
  spec.max_concurrent_changes = static_cast<std::uint32_t>(concurrency.value());
  auto profile = optional_string(value, "profileCode", "");
  if (!profile.ok()) return profile.status();
  spec.profile_code = profile.value();
  return spec;
}

Result<Device> parse_device(const JsonValue& value) {
  Device device;
  auto id = parse_identity<DeviceId>(value, "id");
  if (!id.ok()) return id.status();
  device.id = id.value();
  auto spec = parse_device_spec(value);
  if (!spec.ok()) return spec.status();
  device.spec = spec.value();
  auto exclusions = optional_array(value, "exclusions");
  if (!exclusions.ok()) return exclusions.status();
  if (exclusions.value() != nullptr) {
    for (const JsonValue& element : exclusions.value()->elements()) {
      auto window = parse_exclusion(element);
      if (!window.ok()) return window.status();
      device.exclusions.push_back(window.value());
    }
  }
  auto windows = optional_array(value, "maintenanceWindows");
  if (!windows.ok()) return windows.status();
  if (windows.value() != nullptr) {
    for (const JsonValue& element : windows.value()->elements()) {
      auto window = parse_maintenance_window(element);
      if (!window.ok()) return window.status();
      device.maintenance_windows.push_back(window.value());
    }
  }
  return device;
}

Result<LinkSpec> parse_link_spec(const JsonValue& value) {
  LinkSpec spec;
  auto a = parse_identity<DeviceId>(value, "endpointA");
  if (!a.ok()) return a.status();
  spec.endpoint_a = a.value();
  auto b = parse_identity<DeviceId>(value, "endpointB");
  if (!b.ok()) return b.status();
  spec.endpoint_b = b.value();
  auto capacity = optional_uint(value, "capacity", 0);
  if (!capacity.ok()) return capacity.status();
  spec.capacity = CapacityUnits(capacity.value());
  auto reserved = optional_uint(value, "reserved", 0);
  if (!reserved.ok()) return reserved.status();
  spec.reserved = CapacityUnits(reserved.value());
  auto latency = optional_uint(value, "latencyClass", 0);
  if (!latency.ok()) return latency.status();
  spec.latency_class = static_cast<std::uint32_t>(latency.value());
  auto domains = parse_domains(value, "transitDomains");
  if (!domains.ok()) return domains.status();
  spec.transit_domains = domains.value();
  return spec;
}

Result<Link> parse_link(const JsonValue& value) {
  Link link;
  auto id = parse_identity<LinkId>(value, "id");
  if (!id.ok()) return id.status();
  link.id = id.value();
  auto spec = parse_link_spec(value);
  if (!spec.ok()) return spec.status();
  link.spec = spec.value();
  auto state_token = optional_string(value, "state", "present");
  if (!state_token.ok()) return state_token.status();
  auto state = parse_entity_state(state_token.value());
  if (!state.ok()) return state.status();
  link.state = state.value();
  return link;
}

Result<RouteSpec> parse_route_spec(const JsonValue& value) {
  RouteSpec spec;
  auto source = parse_identity<DeviceId>(value, "source");
  if (!source.ok()) return source.status();
  spec.source = source.value();
  auto sink = parse_identity<DeviceId>(value, "sink");
  if (!sink.ok()) return sink.status();
  spec.sink = sink.value();
  auto path = optional_string_array(value, "path");
  if (!path.ok()) return path.status();
  for (const std::string& hop : path.value()) {
    auto link = LinkId::parse(hop);
    if (!link.ok()) return link.status();
    spec.path.push_back(link.value());
  }
  auto policy = optional_identity<PolicyId>(value, "policy");
  if (!policy.ok()) return policy.status();
  spec.policy = policy.value();
  return spec;
}

Result<Route> parse_route(const JsonValue& value) {
  Route route;
  auto id = parse_identity<RouteId>(value, "id");
  if (!id.ok()) return id.status();
  route.id = id.value();
  auto spec = parse_route_spec(value);
  if (!spec.ok()) return spec.status();
  route.spec = spec.value();
  auto state_token = optional_string(value, "state", "inactive");
  if (!state_token.ok()) return state_token.status();
  auto state = parse_route_state(state_token.value());
  if (!state.ok()) return state.status();
  route.state = state.value();
  return route;
}

Result<RouteBinding> parse_binding(const JsonValue& value) {
  RouteBinding binding;
  auto route = parse_identity<RouteId>(value, "route");
  if (!route.ok()) return route.status();
  binding.route = route.value();
  auto active = optional_bool(value, "active", true);
  if (!active.ok()) return active.status();
  binding.active = active.value();
  auto demand = optional_uint(value, "demand", 0);
  if (!demand.ok()) return demand.status();
  binding.demand = DemandUnits(demand.value());
  return binding;
}

Result<WorkloadRequirements> parse_requirements(const JsonValue& value) {
  WorkloadRequirements requirements;
  auto connectivity = optional_bool(value, "requireConnectivity", true);
  if (!connectivity.ok()) return connectivity.status();
  requirements.require_connectivity = connectivity.value();
  auto domains = optional_uint(value, "minDisjointDomains", 0);
  if (!domains.ok()) return domains.status();
  requirements.min_disjoint_domains = static_cast<std::uint32_t>(domains.value());
  auto paths = optional_uint(value, "minLinkDisjointPaths", 0);
  if (!paths.ok()) return paths.status();
  requirements.min_link_disjoint_paths = static_cast<std::uint32_t>(paths.value());
  auto guaranteed = optional_uint(value, "minGuaranteedCapacity", 0);
  if (!guaranteed.ok()) return guaranteed.status();
  requirements.min_guaranteed_capacity = CapacityUnits(guaranteed.value());
  auto forbidden = parse_domains(value, "forbiddenDomains");
  if (!forbidden.ok()) return forbidden.status();
  requirements.forbidden_domains = forbidden.value();
  return requirements;
}

Result<Workload> parse_workload(const JsonValue& value) {
  Workload workload;
  auto id = parse_identity<WorkloadId>(value, "id");
  if (!id.ok()) return id.status();
  workload.id = id.value();
  auto bindings = optional_array(value, "bindings");
  if (!bindings.ok()) return bindings.status();
  if (bindings.value() != nullptr) {
    for (const JsonValue& element : bindings.value()->elements()) {
      auto binding = parse_binding(element);
      if (!binding.ok()) return binding.status();
      workload.bindings.push_back(binding.value());
    }
  }
  auto requirements = optional_member(value, "requirements");
  if (!requirements.ok()) return requirements.status();
  if (requirements.value() != nullptr) {
    auto parsed = parse_requirements(*requirements.value());
    if (!parsed.ok()) return parsed.status();
    workload.requirements = parsed.value();
  }
  auto contract = optional_string(value, "contractCode", "");
  if (!contract.ok()) return contract.status();
  workload.contract_code = contract.value();
  return workload;
}

Result<PolicyTarget> parse_policy_target(const JsonValue& value) {
  PolicyTarget target;
  auto scope_token = optional_string(value, "scope", "global");
  if (!scope_token.ok()) return scope_token.status();
  auto scope = parse_policy_scope(scope_token.value());
  if (!scope.ok()) return scope.status();
  target.scope = scope.value();
  auto identity = optional_string(value, "target", "");
  if (!identity.ok()) return identity.status();
  target.identity = identity.value();
  return target;
}

Result<PolicyObject> parse_policy(const JsonValue& value) {
  PolicyObject policy;
  auto id = parse_identity<PolicyId>(value, "id");
  if (!id.ok()) return id.status();
  policy.id = id.value();
  auto target = parse_policy_target(value);
  if (!target.ok()) return target.status();
  policy.target = target.value();
  auto code = optional_string(value, "value", "");
  if (!code.ok()) return code.status();
  policy.value_code = code.value();
  auto enforced = optional_bool(value, "enforced", true);
  if (!enforced.ok()) return enforced.status();
  policy.enforced = enforced.value();
  return policy;
}

Result<ConfigEntry> parse_config_entry(const JsonValue& value) {
  ConfigEntry entry;
  auto device = parse_identity<DeviceId>(value, "device");
  if (!device.ok()) return device.status();
  entry.device = device.value();
  auto key = parse_identity<ConfigKey>(value, "key");
  if (!key.ok()) return key.status();
  entry.key = key.value();
  auto code = optional_string(value, "value", "");
  if (!code.ok()) return code.status();
  entry.value_code = code.value();
  auto revision = optional_uint(value, "revision", 0);
  if (!revision.ok()) return revision.status();
  entry.revision = ConfigRevision(revision.value());
  return entry;
}

Result<DesiredAction> parse_action(const JsonValue& value, const char* fallback) {
  auto token = optional_string(value, "action", fallback);
  if (!token.ok()) {
    return token.status();
  }
  return parse_desired_action(token.value());
}

Result<TimestampNs> optional_timestamp(const JsonValue& object, const char* key,
                                       TimestampNs fallback) {
  auto value = optional_uint(object, key, static_cast<std::uint64_t>(fallback.value()));
  if (!value.ok()) {
    return value.status();
  }
  return TimestampNs(static_cast<std::int64_t>(value.value()));
}

}  // namespace

Result<JsonValue> read_json_file(const std::string& path, const JsonLimits& limits) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return Status::error(ErrorCode::not_found, "cannot open '" + path + "' for reading");
  }
  std::ostringstream buffer;
  std::string chunk;
  std::size_t total = 0;
  while (std::getline(stream, chunk)) {
    total += chunk.size() + 1;
    if (total > limits.max_bytes) {
      return Status::error(ErrorCode::size_limit,
                           "'" + path + "' exceeds the maximum document size");
    }
    buffer << chunk << '\n';
  }
  if (stream.bad()) {
    return Status::error(ErrorCode::malformed_input, "failed while reading '" + path + "'");
  }
  auto document = parse_json(buffer.str(), limits);
  if (!document.ok()) {
    return Status::error(document.status().code(),
                         "'" + path + "': " + document.status().message());
  }
  return document;
}

Result<CurrentStateSnapshot> parse_state_document(const JsonValue& document) {
  CurrentStateSnapshot state;
  auto generation = require_uint(document, "topologyGeneration");
  if (!generation.ok()) return generation.status();
  state.topology_generation = TopologyGeneration(generation.value());
  auto authority = parse_identity<AuthorityId>(document, "authority");
  if (!authority.ok()) return authority.status();
  state.authority = authority.value();
  auto authority_generation = optional_uint(document, "authorityGeneration", 0);
  if (!authority_generation.ok()) return authority_generation.status();
  state.authority_generation = AuthorityGeneration(authority_generation.value());
  auto incarnation = optional_uint(document, "bootIncarnation", 0);
  if (!incarnation.ok()) return incarnation.status();
  state.boot_incarnation = BootIncarnation(incarnation.value());
  auto epoch = optional_uint(document, "epoch", 0);
  if (!epoch.ok()) return epoch.status();
  state.epoch = Epoch(epoch.value());
  auto observed = optional_timestamp(document, "observedAt", TimestampNs(0));
  if (!observed.ok()) return observed.status();
  state.observed_at = observed.value();

  auto devices = optional_array(document, "devices");
  if (!devices.ok()) return devices.status();
  if (devices.value() != nullptr) {
    for (const JsonValue& element : devices.value()->elements()) {
      auto device = parse_device(element);
      if (!device.ok()) return device.status();
      state.devices.push_back(device.value());
    }
  }
  auto links = optional_array(document, "links");
  if (!links.ok()) return links.status();
  if (links.value() != nullptr) {
    for (const JsonValue& element : links.value()->elements()) {
      auto link = parse_link(element);
      if (!link.ok()) return link.status();
      state.links.push_back(link.value());
    }
  }
  auto routes = optional_array(document, "routes");
  if (!routes.ok()) return routes.status();
  if (routes.value() != nullptr) {
    for (const JsonValue& element : routes.value()->elements()) {
      auto route = parse_route(element);
      if (!route.ok()) return route.status();
      state.routes.push_back(route.value());
    }
  }
  auto workloads = optional_array(document, "workloads");
  if (!workloads.ok()) return workloads.status();
  if (workloads.value() != nullptr) {
    for (const JsonValue& element : workloads.value()->elements()) {
      auto workload = parse_workload(element);
      if (!workload.ok()) return workload.status();
      state.workloads.push_back(workload.value());
    }
  }
  auto policies = optional_array(document, "policies");
  if (!policies.ok()) return policies.status();
  if (policies.value() != nullptr) {
    for (const JsonValue& element : policies.value()->elements()) {
      auto policy = parse_policy(element);
      if (!policy.ok()) return policy.status();
      state.policies.push_back(policy.value());
    }
  }
  auto configs = optional_array(document, "configs");
  if (!configs.ok()) return configs.status();
  if (configs.value() != nullptr) {
    for (const JsonValue& element : configs.value()->elements()) {
      auto entry = parse_config_entry(element);
      if (!entry.ok()) return entry.status();
      state.configs.push_back(entry.value());
    }
  }
  state.canonicalize();
  CPLAN_TRY(state.validate());
  return state;
}

Result<TargetIntent> parse_target_document(const JsonValue& document) {
  TargetIntent target;
  auto revision = require_uint(document, "revision");
  if (!revision.ok()) return revision.status();
  target.revision = TargetRevision(revision.value());
  auto generation = require_uint(document, "basedOnGeneration");
  if (!generation.ok()) return generation.status();
  target.based_on_generation = TopologyGeneration(generation.value());
  auto authority = parse_identity<AuthorityId>(document, "authority");
  if (!authority.ok()) return authority.status();
  target.authority = authority.value();
  auto authority_generation = optional_uint(document, "authorityGeneration", 0);
  if (!authority_generation.ok()) return authority_generation.status();
  target.authority_generation = AuthorityGeneration(authority_generation.value());
  auto epoch = optional_uint(document, "basedOnEpoch", 0);
  if (!epoch.ok()) return epoch.status();
  target.based_on_epoch = Epoch(epoch.value());
  auto authored = optional_timestamp(document, "authoredAt", TimestampNs(0));
  if (!authored.ok()) return authored.status();
  target.authored_at = authored.value();
  auto intent = optional_string(document, "intentCode", "");
  if (!intent.ok()) return intent.status();
  target.intent_code = intent.value();

  auto devices = optional_array(document, "devices");
  if (!devices.ok()) return devices.status();
  if (devices.value() != nullptr) {
    for (const JsonValue& element : devices.value()->elements()) {
      DesiredDevice desired;
      auto action = parse_action(element, "ensure");
      if (!action.ok()) return action.status();
      desired.action = action.value();
      auto id = parse_identity<DeviceId>(element, "id");
      if (!id.ok()) return id.status();
      desired.id = id.value();
      if (desired.action == DesiredAction::ensure) {
        auto spec = parse_device_spec(element);
        if (!spec.ok()) return spec.status();
        desired.spec = spec.value();
        auto exclusions = optional_array(element, "exclusions");
        if (!exclusions.ok()) return exclusions.status();
        if (exclusions.value() != nullptr) {
          for (const JsonValue& entry : exclusions.value()->elements()) {
            auto window = parse_exclusion(entry);
            if (!window.ok()) return window.status();
            desired.exclusions.push_back(window.value());
          }
        }
        auto windows = optional_array(element, "maintenanceWindows");
        if (!windows.ok()) return windows.status();
        if (windows.value() != nullptr) {
          for (const JsonValue& entry : windows.value()->elements()) {
            auto window = parse_maintenance_window(entry);
            if (!window.ok()) return window.status();
            desired.maintenance_windows.push_back(window.value());
          }
        }
      }
      target.devices.push_back(std::move(desired));
    }
  }
  auto links = optional_array(document, "links");
  if (!links.ok()) return links.status();
  if (links.value() != nullptr) {
    for (const JsonValue& element : links.value()->elements()) {
      DesiredLink desired;
      auto action = parse_action(element, "ensure");
      if (!action.ok()) return action.status();
      desired.action = action.value();
      auto id = parse_identity<LinkId>(element, "id");
      if (!id.ok()) return id.status();
      desired.id = id.value();
      if (desired.action == DesiredAction::ensure) {
        auto spec = parse_link_spec(element);
        if (!spec.ok()) return spec.status();
        desired.spec = spec.value();
      }
      target.links.push_back(std::move(desired));
    }
  }
  auto routes = optional_array(document, "routes");
  if (!routes.ok()) return routes.status();
  if (routes.value() != nullptr) {
    for (const JsonValue& element : routes.value()->elements()) {
      DesiredRoute desired;
      auto action = parse_action(element, "ensure");
      if (!action.ok()) return action.status();
      desired.action = action.value();
      auto id = parse_identity<RouteId>(element, "id");
      if (!id.ok()) return id.status();
      desired.id = id.value();
      if (desired.action == DesiredAction::ensure) {
        auto spec = parse_route_spec(element);
        if (!spec.ok()) return spec.status();
        desired.spec = spec.value();
        auto state_token = optional_string(element, "state", "active");
        if (!state_token.ok()) return state_token.status();
        auto state = parse_route_state(state_token.value());
        if (!state.ok()) return state.status();
        desired.state = state.value();
      }
      target.routes.push_back(std::move(desired));
    }
  }
  auto workloads = optional_array(document, "workloads");
  if (!workloads.ok()) return workloads.status();
  if (workloads.value() != nullptr) {
    for (const JsonValue& element : workloads.value()->elements()) {
      DesiredWorkload desired;
      auto action = parse_action(element, "ensure");
      if (!action.ok()) return action.status();
      desired.action = action.value();
      auto id = parse_identity<WorkloadId>(element, "id");
      if (!id.ok()) return id.status();
      desired.id = id.value();
      if (desired.action == DesiredAction::ensure) {
        auto bindings = optional_array(element, "bindings");
        if (!bindings.ok()) return bindings.status();
        if (bindings.value() != nullptr) {
          for (const JsonValue& entry : bindings.value()->elements()) {
            auto binding = parse_binding(entry);
            if (!binding.ok()) return binding.status();
            desired.bindings.push_back(binding.value());
          }
        }
        auto requirements = optional_member(element, "requirements");
        if (!requirements.ok()) return requirements.status();
        if (requirements.value() != nullptr) {
          auto parsed = parse_requirements(*requirements.value());
          if (!parsed.ok()) return parsed.status();
          desired.requirements = parsed.value();
        }
        auto contract = optional_string(element, "contractCode", "");
        if (!contract.ok()) return contract.status();
        desired.contract_code = contract.value();
      }
      target.workloads.push_back(std::move(desired));
    }
  }
  auto policies = optional_array(document, "policies");
  if (!policies.ok()) return policies.status();
  if (policies.value() != nullptr) {
    for (const JsonValue& element : policies.value()->elements()) {
      DesiredPolicy desired;
      auto action = parse_action(element, "ensure");
      if (!action.ok()) return action.status();
      desired.action = action.value();
      auto id = parse_identity<PolicyId>(element, "id");
      if (!id.ok()) return id.status();
      desired.id = id.value();
      if (desired.action == DesiredAction::ensure) {
        auto target_spec = parse_policy_target(element);
        if (!target_spec.ok()) return target_spec.status();
        desired.target = target_spec.value();
        auto value = optional_string(element, "value", "");
        if (!value.ok()) return value.status();
        desired.value_code = value.value();
        auto enforced = optional_bool(element, "enforced", true);
        if (!enforced.ok()) return enforced.status();
        desired.enforced = enforced.value();
      }
      target.policies.push_back(std::move(desired));
    }
  }
  auto configs = optional_array(document, "configs");
  if (!configs.ok()) return configs.status();
  if (configs.value() != nullptr) {
    for (const JsonValue& element : configs.value()->elements()) {
      DesiredConfig desired;
      auto action = parse_action(element, "ensure");
      if (!action.ok()) return action.status();
      desired.action = action.value();
      auto device = parse_identity<DeviceId>(element, "device");
      if (!device.ok()) return device.status();
      desired.device = device.value();
      auto key = parse_identity<ConfigKey>(element, "key");
      if (!key.ok()) return key.status();
      desired.key = key.value();
      if (desired.action == DesiredAction::ensure) {
        auto value = optional_string(element, "value", "");
        if (!value.ok()) return value.status();
        desired.value_code = value.value();
      }
      target.configs.push_back(std::move(desired));
    }
  }
  target.canonicalize();
  CPLAN_TRY(target.validate());
  return target;
}

Result<SafetyConstraints> parse_constraints_document(const JsonValue& document) {
  SafetyConstraints constraints;
  auto id = parse_identity<ConstraintId>(document, "id");
  if (!id.ok()) return id.status();
  constraints.id = id.value();
  auto authority_generation = optional_uint(document, "authorityGeneration", 0);
  if (!authority_generation.ok()) return authority_generation.status();
  constraints.authority_generation = AuthorityGeneration(authority_generation.value());
  auto policy = parse_identity<PolicyId>(document, "governingPolicy");
  if (!policy.ok()) return policy.status();
  constraints.governing_policy = policy.value();
  auto digest_text = optional_string(document, "governingPolicyDigest", "");
  if (!digest_text.ok()) return digest_text.status();
  if (!digest_text.value().empty()) {
    auto digest = Digest::from_hex(digest_text.value());
    if (!digest.ok()) return digest.status();
    constraints.governing_policy_digest = digest.value();
  }

  auto items = require_member(document, "items");
  if (!items.ok()) return items.status();
  if (!items.value()->is_array()) {
    return document_error("'items' must be an array of constraint items");
  }
  for (const JsonValue& element : items.value()->elements()) {
    ConstraintItem item;
    auto item_id = parse_identity<ConstraintId>(element, "id");
    if (!item_id.ok()) return item_id.status();
    item.id = item_id.value();
    auto kind_token = require_string(element, "kind");
    if (!kind_token.ok()) return kind_token.status();
    auto kind = parse_constraint_kind(kind_token.value());
    if (!kind.ok()) return kind.status();
    item.kind = kind.value();
    auto enabled = optional_bool(element, "enabled", true);
    if (!enabled.ok()) return enabled.status();
    item.enabled = enabled.value();
    auto headroom = optional_uint(element, "headroom", 0);
    if (!headroom.ok()) return headroom.status();
    if (headroom.value() > 1000) {
      return document_error("constraint headroom must be within 0..1000 permille");
    }
    item.headroom = Permille(static_cast<std::uint32_t>(headroom.value()));
    auto concurrency = optional_uint(element, "maxConcurrentChanges", 1);
    if (!concurrency.ok()) return concurrency.status();
    item.max_concurrent_changes = static_cast<std::uint32_t>(concurrency.value());
    auto domains = optional_uint(element, "maxDomainsInMaintenance", 1);
    if (!domains.ok()) return domains.status();
    item.max_domains_in_maintenance = static_cast<std::uint32_t>(domains.value());
    auto parameter = optional_string(element, "parameter", "");
    if (!parameter.ok()) return parameter.status();
    item.parameter_code = parameter.value();
    auto justification = optional_string(element, "justification", "");
    if (!justification.ok()) return justification.status();
    item.justification_code = justification.value();
    constraints.items.push_back(std::move(item));
  }

  auto contracts = optional_array(document, "contracts");
  if (!contracts.ok()) return contracts.status();
  if (contracts.value() != nullptr) {
    for (const JsonValue& element : contracts.value()->elements()) {
      NetworkContract contract;
      auto contract_id = parse_identity<ContractId>(element, "id");
      if (!contract_id.ok()) return contract_id.status();
      contract.id = contract_id.value();
      auto workloads = optional_string_array(element, "workloads");
      if (!workloads.ok()) return workloads.status();
      for (const std::string& text : workloads.value()) {
        auto workload = WorkloadId::parse(text);
        if (!workload.ok()) return workload.status();
        contract.workloads.push_back(workload.value());
      }
      auto units = optional_uint(element, "requiredUnits", 0);
      if (!units.ok()) return units.status();
      contract.required_units = CapacityUnits(units.value());
      auto domains = optional_uint(element, "minDisjointDomains", 0);
      if (!domains.ok()) return domains.status();
      contract.min_disjoint_domains = static_cast<std::uint32_t>(domains.value());
      auto maintenance = optional_uint(element, "maxSimultaneousDomainMaintenance", 1);
      if (!maintenance.ok()) return maintenance.status();
      contract.max_simultaneous_domain_maintenance =
          static_cast<std::uint32_t>(maintenance.value());
      auto forbid = optional_bool(element, "forbidConcurrentChangesInSameDomain", true);
      if (!forbid.ok()) return forbid.status();
      contract.forbid_concurrent_changes_in_same_domain = forbid.value();
      auto sla = optional_string(element, "sla", "");
      if (!sla.ok()) return sla.status();
      contract.sla_code = sla.value();
      constraints.contracts.push_back(std::move(contract));
    }
  }

  auto tolerance = optional_member(document, "tolerance");
  if (!tolerance.ok()) return tolerance.status();
  if (tolerance.value() != nullptr) {
    const JsonValue& value = *tolerance.value();
    auto topology = optional_uint(value, "topologyDrift", 0);
    if (!topology.ok()) return topology.status();
    constraints.tolerance.topology_drift = Permille(static_cast<std::uint32_t>(topology.value()));
    auto capacity = optional_uint(value, "capacityDrift", 0);
    if (!capacity.ok()) return capacity.status();
    constraints.tolerance.capacity_drift = Permille(static_cast<std::uint32_t>(capacity.value()));
    auto demand = optional_uint(value, "demandDrift", 0);
    if (!demand.ok()) return demand.status();
    constraints.tolerance.demand_drift = Permille(static_cast<std::uint32_t>(demand.value()));
    auto added = optional_uint(value, "maxAddedEntities", 0);
    if (!added.ok()) return added.status();
    constraints.tolerance.max_added_entities = static_cast<std::uint32_t>(added.value());
    auto removed = optional_uint(value, "maxRemovedEntities", 0);
    if (!removed.ok()) return removed.status();
    constraints.tolerance.max_removed_entities = static_cast<std::uint32_t>(removed.value());
    auto growth = optional_bool(value, "allowCapabilityGrowth", false);
    if (!growth.ok()) return growth.status();
    constraints.tolerance.allow_capability_growth = growth.value();
    auto shrink = optional_bool(value, "allowCapabilityShrink", false);
    if (!shrink.ok()) return shrink.status();
    constraints.tolerance.allow_capability_shrink = shrink.value();
    auto exempt = optional_string_array(value, "exemptIdentities");
    if (!exempt.ok()) return exempt.status();
    constraints.tolerance.exempt_identities = exempt.value();
  }

  constraints.canonicalize();
  CPLAN_TRY(constraints.validate());
  return constraints;
}

Result<TopologyEvidence> parse_evidence_document(const JsonValue& document) {
  TopologyEvidence evidence;
  auto generation = require_uint(document, "topologyGeneration");
  if (!generation.ok()) return generation.status();
  evidence.topology_generation = TopologyGeneration(generation.value());
  auto incarnation = optional_uint(document, "bootIncarnation", 0);
  if (!incarnation.ok()) return incarnation.status();
  evidence.boot_incarnation = BootIncarnation(incarnation.value());
  auto version = optional_uint(document, "capabilityVersion", 0);
  if (!version.ok()) return version.status();
  evidence.capability_version = CapabilityVersion(version.value());
  auto observed = optional_timestamp(document, "observedAt", TimestampNs(0));
  if (!observed.ok()) return observed.status();
  evidence.observed_at = observed.value();
  auto devices = optional_array(document, "devices");
  if (!devices.ok()) return devices.status();
  if (devices.value() != nullptr) {
    for (const JsonValue& element : devices.value()->elements()) {
      CapabilityEvidence entry;
      auto device = parse_identity<DeviceId>(element, "device");
      if (!device.ok()) return device.status();
      entry.device = device.value();
      auto capabilities = parse_capabilities(element, "capabilities");
      if (!capabilities.ok()) return capabilities.status();
      entry.capabilities = capabilities.value();
      auto entry_version = optional_uint(element, "capabilityVersion", version.value());
      if (!entry_version.ok()) return entry_version.status();
      entry.capability_version = CapabilityVersion(entry_version.value());
      evidence.devices.push_back(std::move(entry));
    }
  }
  evidence.canonicalize();
  CPLAN_TRY(evidence.validate());
  return evidence;
}

Result<Objective> parse_objective(const JsonValue& document) {
  auto member = optional_member(document, "objective");
  if (!member.ok()) return member.status();
  if (member.value() == nullptr) {
    return Objective::default_objective();
  }
  const JsonValue& value = *member.value();
  if (value.is_string()) {
    return Objective::parse_code(value.as_string());
  }
  if (value.is_array()) {
    std::vector<ObjectiveMetric> metrics;
    for (const JsonValue& element : value.elements()) {
      if (!element.is_string()) {
        return document_error("'objective' array must contain metric name strings");
      }
      auto metric = parse_objective_metric(element.as_string());
      if (!metric.ok()) return metric.status();
      metrics.push_back(metric.value());
    }
    return Objective::from_metrics(std::move(metrics));
  }
  return document_error("'objective' must be a string or an array of metric names");
}

Result<PlanningLimits> parse_limits(const JsonValue& document) {
  PlanningLimits limits;
  auto member = optional_member(document, "limits");
  if (!member.ok()) return member.status();
  if (member.value() == nullptr) {
    CPLAN_TRY(limits.validate());
    return limits;
  }
  const JsonValue& value = *member.value();
  auto steps = optional_uint(value, "maxSteps", limits.max_steps);
  if (!steps.ok()) return steps.status();
  limits.max_steps = static_cast<std::uint32_t>(steps.value());
  auto candidates = optional_uint(value, "maxCandidates", limits.max_candidates);
  if (!candidates.ok()) return candidates.status();
  limits.max_candidates = static_cast<std::uint32_t>(candidates.value());
  auto width = optional_uint(value, "maxStageWidth", limits.max_stage_width);
  if (!width.ok()) return width.status();
  limits.max_stage_width = static_cast<std::uint32_t>(width.value());
  auto attempts = optional_uint(value, "maxSchedulingAttempts", limits.max_scheduling_attempts);
  if (!attempts.ok()) return attempts.status();
  limits.max_scheduling_attempts = static_cast<std::uint32_t>(attempts.value());
  auto core = optional_uint(value, "maxCoreMinimisationAttempts",
                            limits.max_core_minimisation_attempts);
  if (!core.ok()) return core.status();
  limits.max_core_minimisation_attempts = static_cast<std::uint32_t>(core.value());
  auto permutations = optional_uint(value, "maxPermutationChecks", limits.max_permutation_checks);
  if (!permutations.ok()) return permutations.status();
  limits.max_permutation_checks = permutations.value();
  auto states = optional_uint(value, "maxStatesVisited", limits.max_states_visited);
  if (!states.ok()) return states.status();
  limits.max_states_visited = states.value();
  auto shift = optional_bool(value, "allowWindowShift", limits.allow_window_shift);
  if (!shift.ok()) return shift.status();
  limits.allow_window_shift = shift.value();
  CPLAN_TRY(limits.validate());
  return limits;
}

Result<PlanRequest> parse_plan_request(const JsonValue& document) {
  PlanRequest request;
  auto plan_id = parse_identity<PlanId>(document, "planId");
  if (!plan_id.ok()) return plan_id.status();
  request.plan_id = plan_id.value();
  auto generation = optional_uint(document, "planGeneration", 1);
  if (!generation.ok()) return generation.status();
  request.generation = PlanGeneration(generation.value());
  auto label = optional_string(document, "label", "");
  if (!label.ok()) return label.status();
  request.label = label.value();

  auto state_member = require_member(document, "state");
  if (!state_member.ok()) return state_member.status();
  auto state = parse_state_document(*state_member.value());
  if (!state.ok()) return state.status();
  request.state = state.value();

  auto target_member = require_member(document, "target");
  if (!target_member.ok()) return target_member.status();
  auto target = parse_target_document(*target_member.value());
  if (!target.ok()) return target.status();
  request.target = target.value();

  auto constraints_member = require_member(document, "constraints");
  if (!constraints_member.ok()) return constraints_member.status();
  auto constraints = parse_constraints_document(*constraints_member.value());
  if (!constraints.ok()) return constraints.status();
  request.constraints = constraints.value();

  auto evidence_member = require_member(document, "evidence");
  if (!evidence_member.ok()) return evidence_member.status();
  auto evidence = parse_evidence_document(*evidence_member.value());
  if (!evidence.ok()) return evidence.status();
  request.evidence = evidence.value();

  auto instant = optional_timestamp(document, "planningInstant", request.state.observed_at);
  if (!instant.ok()) return instant.status();
  request.planning_instant = instant.value();

  auto objective = parse_objective(document);
  if (!objective.ok()) return objective.status();
  request.objective = objective.value();

  auto limits = parse_limits(document);
  if (!limits.ok()) return limits.status();
  request.limits = limits.value();

  CPLAN_TRY(request.validate());
  return request;
}

Result<ObservedExecution> parse_observed_execution(const JsonValue& document) {
  ObservedExecution observed;
  auto plan_id = parse_identity<PlanId>(document, "planId");
  if (!plan_id.ok()) return plan_id.status();
  observed.plan_id = plan_id.value();
  auto generation = require_uint(document, "planGeneration");
  if (!generation.ok()) return generation.status();
  observed.plan_generation = PlanGeneration(generation.value());
  auto attempt = optional_uint(document, "attempt", 1);
  if (!attempt.ok()) return attempt.status();
  observed.attempt = AttemptNumber(static_cast<std::uint32_t>(attempt.value()));
  auto authority = optional_uint(document, "authorityGeneration", 0);
  if (!authority.ok()) return authority.status();
  observed.authority_generation = AuthorityGeneration(authority.value());
  auto observed_generation = optional_uint(document, "observedGeneration", 0);
  if (!observed_generation.ok()) return observed_generation.status();
  observed.observed_generation = TopologyGeneration(observed_generation.value());
  auto incarnation = optional_uint(document, "bootIncarnation", 0);
  if (!incarnation.ok()) return incarnation.status();
  observed.boot_incarnation = BootIncarnation(incarnation.value());
  auto epoch = optional_uint(document, "observedEpoch", 0);
  if (!epoch.ok()) return epoch.status();
  observed.observed_epoch = Epoch(epoch.value());
  auto instant = optional_timestamp(document, "observedAt", TimestampNs(0));
  if (!instant.ok()) return instant.status();
  observed.observed_at = instant.value();

  auto state_member = require_member(document, "state");
  if (!state_member.ok()) return state_member.status();
  auto state = parse_state_document(*state_member.value());
  if (!state.ok()) return state.status();
  observed.observed_state = state.value();

  auto observations = optional_array(document, "observations");
  if (!observations.ok()) return observations.status();
  if (observations.value() != nullptr) {
    for (const JsonValue& element : observations.value()->elements()) {
      StepObservation observation;
      auto step = parse_identity<StepId>(element, "step");
      if (!step.ok()) return step.status();
      observation.step = step.value();
      auto step_attempt = optional_uint(element, "attempt", 1);
      if (!step_attempt.ok()) return step_attempt.status();
      observation.attempt = AttemptNumber(static_cast<std::uint32_t>(step_attempt.value()));
      auto outcome_token = optional_string(element, "outcome", "unknown");
      if (!outcome_token.ok()) return outcome_token.status();
      auto outcome = parse_step_outcome(outcome_token.value());
      if (!outcome.ok()) return outcome.status();
      observation.outcome = outcome.value();
      auto detail = optional_string(element, "detail", "");
      if (!detail.ok()) return detail.status();
      observation.detail_code = detail.value();
      observed.observations.push_back(std::move(observation));
    }
  }
  CPLAN_TRY(observed.validate());
  return observed;
}

namespace {

JsonValue entity_ref_to_json(const EntityRef& reference) {
  return JsonValue::make_string(reference.to_string());
}

JsonValue condition_to_json(const Condition& condition) {
  JsonValue::object_type members;
  members.emplace_back("kind", JsonValue::make_string(to_string(condition.kind)));
  members.emplace_back("subject", entity_ref_to_json(condition.subject));
  if (!condition.parameter.empty()) {
    members.emplace_back("parameter", JsonValue::make_string(condition.parameter));
  }
  if (condition.quantity != 0) {
    members.emplace_back("quantity", JsonValue::make_uint(condition.quantity));
  }
  members.emplace_back("text", JsonValue::make_string(condition.to_string()));
  return JsonValue::make_object(std::move(members));
}

JsonValue violation_to_json(const Violation& violation) {
  JsonValue::object_type members;
  members.emplace_back("invariant", JsonValue::make_string(to_string(violation.invariant)));
  members.emplace_back("severity", JsonValue::make_string(to_string(violation.severity)));
  members.emplace_back("detail", JsonValue::make_string(violation.detail_code));
  members.emplace_back("message", JsonValue::make_string(violation.message));
  JsonValue::array_type evidence;
  for (const EntityRef& reference : violation.evidence) {
    evidence.push_back(entity_ref_to_json(reference));
  }
  members.emplace_back("evidence", JsonValue::make_array(std::move(evidence)));
  return JsonValue::make_object(std::move(members));
}

JsonValue step_to_json(const Step& step) {
  JsonValue::object_type members;
  members.emplace_back("id", JsonValue::make_string(step.id.str()));
  members.emplace_back("stage", JsonValue::make_uint(step.stage.value()));
  members.emplace_back("operationKind", JsonValue::make_string(to_string(step.operation.kind)));
  members.emplace_back("operation", JsonValue::make_string(step.operation.to_string()));
  members.emplace_back("operationClass",
                       JsonValue::make_string(to_string(step.operation.operation_class())));
  members.emplace_back("risk", JsonValue::make_uint(step.risk.value()));
  members.emplace_back("estimatedDurationNs",
                       JsonValue::make_uint(step.estimated_duration.value()));
  JsonValue::array_type preconditions;
  for (const Condition& condition : step.preconditions) {
    preconditions.push_back(condition_to_json(condition));
  }
  members.emplace_back("preconditions", JsonValue::make_array(std::move(preconditions)));
  JsonValue::array_type postconditions;
  for (const Condition& condition : step.postconditions) {
    postconditions.push_back(condition_to_json(condition));
  }
  members.emplace_back("postconditions", JsonValue::make_array(std::move(postconditions)));
  members.emplace_back("compensationKind",
                       JsonValue::make_string(to_string(step.compensation.kind)));
  members.emplace_back("compensationAvailable", JsonValue::make_bool(step.compensation.available));
  if (step.compensation.available) {
    members.emplace_back("compensation",
                         JsonValue::make_string(step.compensation.inverse.to_string()));
  }
  members.emplace_back("compensationNote", JsonValue::make_string(step.compensation.note_code));
  JsonValue::array_type windows;
  for (const WindowId& window : step.windows) {
    windows.push_back(JsonValue::make_string(window.str()));
  }
  members.emplace_back("windows", JsonValue::make_array(std::move(windows)));
  JsonValue::array_type invariants;
  for (const InvariantId invariant : step.justification.checked_invariants) {
    invariants.push_back(JsonValue::make_string(to_string(invariant)));
  }
  members.emplace_back("verifiedInvariants", JsonValue::make_array(std::move(invariants)));
  members.emplace_back("rationale", JsonValue::make_string(step.justification.rationale_code));
  return JsonValue::make_object(std::move(members));
}

}  // namespace

JsonValue plan_summary_to_json(const Plan& plan) {
  JsonValue::object_type members;
  members.emplace_back("planId", JsonValue::make_string(plan.id.str()));
  members.emplace_back("generation", JsonValue::make_uint(plan.generation.value()));
  members.emplace_back("lineage", JsonValue::make_string(to_string(plan.lineage.kind)));
  members.emplace_back("parentPlan", JsonValue::make_string(plan.lineage.parent_plan.str()));
  members.emplace_back("parentGeneration", JsonValue::make_uint(plan.lineage.parent_generation.value()));
  members.emplace_back("observedAttempt", JsonValue::make_uint(plan.lineage.observed_attempt.value()));
  members.emplace_back("planningInstant",
                       JsonValue::make_uint(static_cast<std::uint64_t>(plan.planning_instant.value())));
  members.emplace_back("stages", JsonValue::make_uint(static_cast<std::uint64_t>(plan.stages.size())));
  members.emplace_back("steps", JsonValue::make_uint(static_cast<std::uint64_t>(plan.steps.size())));
  members.emplace_back("verifiedPermutations", JsonValue::make_uint(plan.verified_permutations));
  members.emplace_back("verificationDigest", JsonValue::make_string(plan.verification_digest.hex()));
  members.emplace_back("contentDigest", JsonValue::make_string(plan.content_digest().hex()));
  members.emplace_back("requestDigest", JsonValue::make_string(plan.request_digest.hex()));
  members.emplace_back("label", JsonValue::make_string(plan.label));

  JsonValue::object_type binding;
  binding.emplace_back("topologyGeneration",
                       JsonValue::make_uint(plan.binding.topology_generation.value()));
  binding.emplace_back("authority", JsonValue::make_string(plan.binding.authority.str()));
  binding.emplace_back("authorityGeneration",
                       JsonValue::make_uint(plan.binding.authority_generation.value()));
  binding.emplace_back("bootIncarnation",
                       JsonValue::make_uint(plan.binding.boot_incarnation.value()));
  binding.emplace_back("epoch", JsonValue::make_uint(plan.binding.epoch.value()));
  binding.emplace_back("capabilityVersion",
                       JsonValue::make_uint(plan.binding.capability_version.value()));
  binding.emplace_back("targetRevision", JsonValue::make_uint(plan.binding.target_revision.value()));
  binding.emplace_back("topologyDigest", JsonValue::make_string(plan.binding.topology_digest.hex()));
  binding.emplace_back("targetDigest", JsonValue::make_string(plan.binding.target_digest.hex()));
  binding.emplace_back("constraintDigest",
                       JsonValue::make_string(plan.binding.constraint_digest.hex()));
  binding.emplace_back("evidenceDigest", JsonValue::make_string(plan.binding.evidence_digest.hex()));
  members.emplace_back("binding", JsonValue::make_object(std::move(binding)));

  JsonValue::object_type objective;
  objective.emplace_back("stages", JsonValue::make_uint(plan.objective.stages.value()));
  objective.emplace_back("riskExposure", JsonValue::make_uint(plan.objective.risk_exposure.value()));
  objective.emplace_back("churn", JsonValue::make_uint(plan.objective.churn));
  objective.emplace_back("maintenanceDurationNs",
                         JsonValue::make_uint(plan.objective.maintenance_duration.value()));
  objective.emplace_back("tieBreak", JsonValue::make_string(plan.objective.tie_break.hex()));
  members.emplace_back("objective", JsonValue::make_object(std::move(objective)));

  members.emplace_back("rationale", JsonValue::make_string(plan.justification.rationale_code));
  JsonValue::array_type notes;
  for (const std::string& note : plan.justification.notes) {
    notes.push_back(JsonValue::make_string(note));
  }
  members.emplace_back("notes", JsonValue::make_array(std::move(notes)));

  JsonValue::array_type rejected;
  for (const RejectedAlternative& alternative : plan.rejected) {
    JsonValue::object_type entry;
    entry.emplace_back("code", JsonValue::make_string(alternative.code));
    entry.emplace_back("detail", JsonValue::make_string(alternative.detail));
    entry.emplace_back("invariant", JsonValue::make_string(to_string(alternative.violated)));
    rejected.push_back(JsonValue::make_object(std::move(entry)));
  }
  members.emplace_back("rejectedAlternatives", JsonValue::make_array(std::move(rejected)));
  return JsonValue::make_object(std::move(members));
}

JsonValue plan_to_json(const Plan& plan) {
  JsonValue::object_type members = plan_summary_to_json(plan).members();
  JsonValue::array_type stages;
  for (const Stage& stage : plan.stages) {
    JsonValue::object_type entry;
    entry.emplace_back("index", JsonValue::make_uint(stage.index.value()));
    JsonValue::array_type steps;
    for (const StepId& id : stage.steps) {
      steps.push_back(JsonValue::make_string(id.str()));
    }
    entry.emplace_back("steps", JsonValue::make_array(std::move(steps)));
    entry.emplace_back("rationale", JsonValue::make_string(stage.rationale_code));
    entry.emplace_back("verifiedPermutations", JsonValue::make_uint(stage.verified_permutations));
    entry.emplace_back("verifiedApplications", JsonValue::make_uint(stage.verified_applications));
    stages.push_back(JsonValue::make_object(std::move(entry)));
  }
  members.emplace_back("stageList", JsonValue::make_array(std::move(stages)));
  JsonValue::array_type steps;
  for (const Step& step : plan.steps) {
    steps.push_back(step_to_json(step));
  }
  members.emplace_back("stepList", JsonValue::make_array(std::move(steps)));
  JsonValue::array_type edges;
  for (const DependencyEdge& edge : plan.edges) {
    JsonValue::object_type entry;
    entry.emplace_back("from", JsonValue::make_string(edge.from.str()));
    entry.emplace_back("to", JsonValue::make_string(edge.to.str()));
    entry.emplace_back("reason", JsonValue::make_string(to_string(edge.reason)));
    entry.emplace_back("detail", JsonValue::make_string(edge.detail_code));
    edges.push_back(JsonValue::make_object(std::move(entry)));
  }
  members.emplace_back("dependencies", JsonValue::make_array(std::move(edges)));
  return JsonValue::make_object(std::move(members));
}

JsonValue refusal_to_json(const Refusal& refusal) {
  JsonValue::object_type members;
  members.emplace_back("outcome", JsonValue::make_string("refused"));
  members.emplace_back("code", JsonValue::make_string(to_string(refusal.code)));
  members.emplace_back("summary", JsonValue::make_string(refusal.summary_code));
  members.emplace_back("explanation", JsonValue::make_string(refusal.explain()));
  members.emplace_back("requestDigest", JsonValue::make_string(refusal.request_digest.hex()));

  JsonValue::array_type blocking;
  for (const BlockingConstraint& entry : refusal.blocking) {
    JsonValue::object_type item;
    item.emplace_back("id", JsonValue::make_string(entry.id.str()));
    item.emplace_back("kind", JsonValue::make_string(to_string(entry.kind)));
    item.emplace_back("invariant", JsonValue::make_string(to_string(entry.invariant)));
    item.emplace_back("message", JsonValue::make_string(entry.message));
    JsonValue::array_type evidence;
    for (const EntityRef& reference : entry.evidence) {
      evidence.push_back(entity_ref_to_json(reference));
    }
    item.emplace_back("evidence", JsonValue::make_array(std::move(evidence)));
    blocking.push_back(JsonValue::make_object(std::move(item)));
  }
  members.emplace_back("blockingConstraints", JsonValue::make_array(std::move(blocking)));

  JsonValue::array_type violations;
  for (const Violation& violation : refusal.residual_violations) {
    violations.push_back(violation_to_json(violation));
  }
  members.emplace_back("residualViolations", JsonValue::make_array(std::move(violations)));

  JsonValue::array_type blocked;
  for (const BlockedStep& step : refusal.blocked_steps) {
    JsonValue::object_type item;
    item.emplace_back("step", JsonValue::make_string(step.id.str()));
    item.emplace_back("operation", JsonValue::make_string(step.operation.to_string()));
    item.emplace_back("blockingCode", JsonValue::make_string(step.blocking_code));
    JsonValue::array_type conditions;
    for (const Condition& condition : step.unsatisfied_preconditions) {
      conditions.push_back(condition_to_json(condition));
    }
    item.emplace_back("unsatisfiedPreconditions", JsonValue::make_array(std::move(conditions)));
    JsonValue::array_type step_violations;
    for (const Violation& violation : step.violations) {
      step_violations.push_back(violation_to_json(violation));
    }
    item.emplace_back("violations", JsonValue::make_array(std::move(step_violations)));
    blocked.push_back(JsonValue::make_object(std::move(item)));
  }
  members.emplace_back("blockedSteps", JsonValue::make_array(std::move(blocked)));

  JsonValue::array_type rejected;
  for (const RejectedAlternative& alternative : refusal.rejected) {
    JsonValue::object_type entry;
    entry.emplace_back("code", JsonValue::make_string(alternative.code));
    entry.emplace_back("detail", JsonValue::make_string(alternative.detail));
    entry.emplace_back("invariant", JsonValue::make_string(to_string(alternative.violated)));
    rejected.push_back(JsonValue::make_object(std::move(entry)));
  }
  members.emplace_back("rejectedAlternatives", JsonValue::make_array(std::move(rejected)));

  JsonValue::array_type notes;
  for (const std::string& note : refusal.justification.notes) {
    notes.push_back(JsonValue::make_string(note));
  }
  members.emplace_back("notes", JsonValue::make_array(std::move(notes)));
  return JsonValue::make_object(std::move(members));
}

JsonValue step_explanation_to_json(const StepExplanation& explanation) {
  JsonValue::object_type members;
  members.emplace_back("step", JsonValue::make_string(explanation.id.str()));
  members.emplace_back("stage", JsonValue::make_uint(explanation.stage.value()));
  members.emplace_back("operation", JsonValue::make_string(explanation.operation));
  members.emplace_back("operationKind", JsonValue::make_string(to_string(explanation.operation_kind)));
  members.emplace_back("operationClass",
                       JsonValue::make_string(to_string(explanation.operation_class)));
  members.emplace_back("risk", JsonValue::make_uint(explanation.risk.value()));
  members.emplace_back("estimatedDurationNs",
                       JsonValue::make_uint(explanation.estimated_duration.value()));
  JsonValue::array_type preconditions;
  for (const Condition& condition : explanation.preconditions) {
    preconditions.push_back(condition_to_json(condition));
  }
  members.emplace_back("preconditions", JsonValue::make_array(std::move(preconditions)));
  JsonValue::array_type postconditions;
  for (const Condition& condition : explanation.postconditions) {
    postconditions.push_back(condition_to_json(condition));
  }
  members.emplace_back("postconditions", JsonValue::make_array(std::move(postconditions)));
  JsonValue::array_type incoming;
  for (const DependencyEdge& edge : explanation.incoming) {
    JsonValue::object_type entry;
    entry.emplace_back("from", JsonValue::make_string(edge.from.str()));
    entry.emplace_back("reason", JsonValue::make_string(to_string(edge.reason)));
    entry.emplace_back("detail", JsonValue::make_string(edge.detail_code));
    incoming.push_back(JsonValue::make_object(std::move(entry)));
  }
  members.emplace_back("dependsOn", JsonValue::make_array(std::move(incoming)));
  JsonValue::array_type outgoing;
  for (const DependencyEdge& edge : explanation.outgoing) {
    JsonValue::object_type entry;
    entry.emplace_back("to", JsonValue::make_string(edge.to.str()));
    entry.emplace_back("reason", JsonValue::make_string(to_string(edge.reason)));
    entry.emplace_back("detail", JsonValue::make_string(edge.detail_code));
    outgoing.push_back(JsonValue::make_object(std::move(entry)));
  }
  members.emplace_back("unlocks", JsonValue::make_array(std::move(outgoing)));
  JsonValue::array_type concurrent;
  for (const StepId& id : explanation.concurrent_steps) {
    concurrent.push_back(JsonValue::make_string(id.str()));
  }
  members.emplace_back("concurrentWith", JsonValue::make_array(std::move(concurrent)));
  members.emplace_back("compensationKind",
                       JsonValue::make_string(to_string(explanation.compensation.kind)));
  members.emplace_back("compensationAvailable",
                       JsonValue::make_bool(explanation.compensation.available));
  members.emplace_back("rationale", JsonValue::make_string(explanation.justification.rationale_code));
  members.emplace_back("explanation", JsonValue::make_string(explanation.explain()));
  return JsonValue::make_object(std::move(members));
}

JsonValue comparison_to_json(const PlanComparison& comparison) {
  JsonValue::object_type members;
  members.emplace_back("identical", JsonValue::make_bool(comparison.identical));
  members.emplace_back("semanticallyIdentical",
                       JsonValue::make_bool(comparison.semantically_identical));
  members.emplace_back("leftContent", JsonValue::make_string(comparison.left_content.hex()));
  members.emplace_back("rightContent", JsonValue::make_string(comparison.right_content.hex()));
  members.emplace_back("leftStages", JsonValue::make_uint(comparison.left_stages));
  members.emplace_back("rightStages", JsonValue::make_uint(comparison.right_stages));
  members.emplace_back("leftSteps", JsonValue::make_uint(comparison.left_steps));
  members.emplace_back("rightSteps", JsonValue::make_uint(comparison.right_steps));
  JsonValue::array_type differences;
  for (const StepDifference& difference : comparison.differences) {
    JsonValue::object_type entry;
    entry.emplace_back("step", JsonValue::make_string(difference.id.str()));
    entry.emplace_back("kind", JsonValue::make_string(to_string(difference.kind)));
    entry.emplace_back("leftStage", JsonValue::make_uint(difference.left_stage.value()));
    entry.emplace_back("rightStage", JsonValue::make_uint(difference.right_stage.value()));
    entry.emplace_back("detail", JsonValue::make_string(difference.detail));
    differences.push_back(JsonValue::make_object(std::move(entry)));
  }
  members.emplace_back("differences", JsonValue::make_array(std::move(differences)));
  members.emplace_back("summary", JsonValue::make_string(comparison.summary));
  return JsonValue::make_object(std::move(members));
}

JsonValue invalidation_to_json(const InvalidationDecision& decision) {
  JsonValue::object_type members;
  members.emplace_back("valid", JsonValue::make_bool(decision.valid));
  members.emplace_back("dimension", JsonValue::make_string(to_string(decision.dimension)));
  members.emplace_back("detail", JsonValue::make_string(decision.detail_code));
  members.emplace_back("message", JsonValue::make_string(decision.message));
  members.emplace_back("addedEntities", JsonValue::make_uint(decision.added_entities));
  members.emplace_back("removedEntities", JsonValue::make_uint(decision.removed_entities));
  members.emplace_back("changedEntities", JsonValue::make_uint(decision.changed_entities));
  members.emplace_back("capacityDriftPermille", JsonValue::make_uint(decision.capacity_drift.value()));
  members.emplace_back("demandDriftPermille", JsonValue::make_uint(decision.demand_drift.value()));
  members.emplace_back("topologyDriftPermille", JsonValue::make_uint(decision.topology_drift.value()));
  members.emplace_back("explanation", JsonValue::make_string(decision.explain()));
  return JsonValue::make_object(std::move(members));
}

JsonValue verification_to_json(const VerificationReport& report) {
  JsonValue::object_type members;
  members.emplace_back("ok", JsonValue::make_bool(report.ok));
  members.emplace_back("stepsApplied", JsonValue::make_uint(report.steps_applied));
  members.emplace_back("permutationsVerified", JsonValue::make_uint(report.permutations_verified));
  members.emplace_back("invariantEvaluations", JsonValue::make_uint(report.invariant_evaluations));
  members.emplace_back("traceDigest", JsonValue::make_string(report.trace_digest.hex()));
  members.emplace_back("failingStep", JsonValue::make_string(report.failing_step.str()));
  members.emplace_back("failingStage", JsonValue::make_uint(report.failing_stage.value()));
  members.emplace_back("message", JsonValue::make_string(report.message));
  members.emplace_back("explanation", JsonValue::make_string(report.explain()));
  JsonValue::array_type violations;
  for (const Violation& violation : report.violations) {
    violations.push_back(violation_to_json(violation));
  }
  members.emplace_back("violations", JsonValue::make_array(std::move(violations)));
  return JsonValue::make_object(std::move(members));
}

std::string describe_plan(const Plan& plan) {
  std::string out;
  out += "plan " + plan.id.str() + " generation " + std::to_string(plan.generation.value());
  out += " (" + std::string(to_string(plan.lineage.kind)) + " lineage)";
  out += "\n  steps: " + std::to_string(plan.steps.size()) + " in " +
         std::to_string(plan.stages.size()) + " stage(s)";
  out += "\n  planning instant: " + std::to_string(plan.planning_instant.value());
  out += "\n  topology generation: " +
         std::to_string(plan.binding.topology_generation.value()) + ", authority '" +
         plan.binding.authority.str() + "' generation " +
         std::to_string(plan.binding.authority_generation.value());
  out += "\n  objective: stages=" + std::to_string(plan.objective.stages.value()) +
         " risk=" + std::to_string(plan.objective.risk_exposure.value()) +
         " churn=" + std::to_string(plan.objective.churn) +
         " maintenance=" + std::to_string(plan.objective.maintenance_duration.value()) + "ns";
  out += "\n  verification digest: " + plan.verification_digest.hex();
  out += "\n  content digest:      " + plan.content_digest().hex();
  for (const Stage& stage : plan.stages) {
    out += "\n  stage " + std::to_string(stage.index.value()) + " (" + stage.rationale_code +
           ", " + std::to_string(stage.verified_permutations) + " verified interleaving(s)):";
    for (const StepId& id : stage.steps) {
      const Step* step = plan.find_step(id);
      if (step == nullptr) {
        continue;
      }
      out += "\n    - " + step->id.str() + " " + step->operation.to_string();
    }
  }
  if (!plan.rejected.empty()) {
    out += "\n  rejected alternatives:";
    for (const RejectedAlternative& alternative : plan.rejected) {
      out += "\n    - " + alternative.code + ": " + alternative.detail;
    }
  }
  return out;
}

std::string describe_invalidation(const InvalidationDecision& decision) {
  return decision.explain();
}

std::string describe_verification(const VerificationReport& report) { return report.explain(); }

}  // namespace cplan::cli
