#include "change_planner/model/target.hpp"

#include <algorithm>
#include <set>

namespace cplan {

const char* to_string(DesiredAction action) noexcept {
  switch (action) {
    case DesiredAction::ensure:
      return "ensure";
    case DesiredAction::remove:
      return "remove";
  }
  return "unknown";
}

Result<DesiredAction> parse_desired_action(std::string_view token) {
  if (token == "ensure") return DesiredAction::ensure;
  if (token == "remove") return DesiredAction::remove;
  return Status::error(ErrorCode::malformed_input,
                       "unknown desired action token: " + std::string(token));
}

namespace {

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
                       std::string("duplicate target ") + kind + " entry: " + identity);
}

}  // namespace

Result<void> TargetIntent::validate() const {
  if (authority.empty()) {
    return Status::error(ErrorCode::invalid_argument, "target intent authority identity is empty");
  }
  if (based_on_generation.value() == 0) {
    return Status::error(ErrorCode::invalid_argument,
                         "target intent must declare the topology generation it is based on");
  }
  if (has_duplicate_ids(devices)) return duplicate("device", "<intent>");
  if (has_duplicate_ids(links)) return duplicate("link", "<intent>");
  if (has_duplicate_ids(routes)) return duplicate("route", "<intent>");
  if (has_duplicate_ids(workloads)) return duplicate("workload", "<intent>");
  if (has_duplicate_ids(policies)) return duplicate("policy", "<intent>");

  for (const DesiredDevice& desired : devices) {
    if (desired.id.empty()) {
      return Status::error(ErrorCode::invalid_argument, "target device entry has an empty identity");
    }
    if (desired.action == DesiredAction::ensure && desired.spec.domain.empty()) {
      return Status::error(ErrorCode::invalid_argument,
                           "target device '" + desired.id.str() + "' has no failure domain");
    }
    std::set<std::string> window_ids;
    for (const ExclusionWindow& window : desired.exclusions) {
      if (window.end <= window.begin) {
        return Status::error(ErrorCode::invalid_argument,
                             "target exclusion window on device '" + desired.id.str() +
                                 "' has a non-positive duration");
      }
      if (!window_ids.insert(window.id.str()).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "duplicate window identity on device '" + desired.id.str() + "'");
      }
    }
    for (const MaintenanceWindow& window : desired.maintenance_windows) {
      if (window.end <= window.begin) {
        return Status::error(ErrorCode::invalid_argument,
                             "target maintenance window on device '" + desired.id.str() +
                                 "' has a non-positive duration");
      }
      if (!window_ids.insert(window.id.str()).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "duplicate window identity on device '" + desired.id.str() + "'");
      }
    }
  }

  for (const DesiredLink& desired : links) {
    if (desired.action == DesiredAction::remove) {
      continue;
    }
    if (desired.spec.endpoint_a.empty() || desired.spec.endpoint_b.empty()) {
      return Status::error(ErrorCode::invalid_argument,
                           "target link '" + desired.id.str() + "' has an empty endpoint");
    }
    if (desired.spec.endpoint_a == desired.spec.endpoint_b) {
      return Status::error(ErrorCode::invalid_argument,
                           "target link '" + desired.id.str() + "' has identical endpoints");
    }
  }

  for (const DesiredRoute& desired : routes) {
    if (desired.action == DesiredAction::remove) {
      continue;
    }
    if (desired.spec.source.empty() || desired.spec.sink.empty()) {
      return Status::error(ErrorCode::invalid_argument,
                           "target route '" + desired.id.str() + "' has an empty endpoint");
    }
    if (desired.spec.path.empty()) {
      return Status::error(ErrorCode::invalid_argument,
                           "target route '" + desired.id.str() + "' has an empty path");
    }
  }

  for (const DesiredWorkload& desired : workloads) {
    std::set<std::string> bound;
    for (const RouteBinding& binding : desired.bindings) {
      if (binding.route.empty()) {
        return Status::error(ErrorCode::invalid_argument,
                             "target workload '" + desired.id.str() + "' binds an empty route id");
      }
      if (!bound.insert(binding.route.str()).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "target workload '" + desired.id.str() + "' binds route '" +
                                 binding.route.str() + "' more than once");
      }
    }
  }

  for (const DesiredPolicy& desired : policies) {
    if (desired.target.scope == PolicyScope::global) {
      if (!desired.target.identity.empty()) {
        return Status::error(ErrorCode::invalid_argument,
                             "global policy target must not carry an identity");
      }
      continue;
    }
    if (desired.target.identity.empty()) {
      return Status::error(ErrorCode::invalid_argument,
                           "scoped policy target requires an identity");
    }
    const bool parses =
        (desired.target.scope == PolicyScope::device && DeviceId::parse(desired.target.identity).ok()) ||
        (desired.target.scope == PolicyScope::link && LinkId::parse(desired.target.identity).ok()) ||
        (desired.target.scope == PolicyScope::route && RouteId::parse(desired.target.identity).ok()) ||
        (desired.target.scope == PolicyScope::workload &&
         WorkloadId::parse(desired.target.identity).ok());
    if (!parses) {
      return Status::error(ErrorCode::invalid_argument,
                           "policy target identity '" + desired.target.identity +
                               "' is not a valid identity for scope " +
                               to_string(desired.target.scope));
    }
  }

  {
    std::set<std::pair<DeviceId, ConfigKey>> seen;
    for (const DesiredConfig& desired : configs) {
      if (desired.device.empty() || desired.key.empty()) {
        return Status::error(ErrorCode::invalid_argument,
                             "target config entry requires a device and a key");
      }
      if (!seen.insert({desired.device, desired.key}).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "duplicate target config entry for key '" + desired.key.str() +
                                 "' on device '" + desired.device.str() + "'");
      }
    }
  }

  return Status::success();
}

Digest TargetIntent::digest() const {
  CanonicalEncoder encoder;
  encode(encoder);
  return encoder.digest();
}

void TargetIntent::canonicalize() {
  std::sort(devices.begin(), devices.end(),
            [](const DesiredDevice& lhs, const DesiredDevice& rhs) { return lhs.id < rhs.id; });
  std::sort(links.begin(), links.end(),
            [](const DesiredLink& lhs, const DesiredLink& rhs) { return lhs.id < rhs.id; });
  std::sort(routes.begin(), routes.end(),
            [](const DesiredRoute& lhs, const DesiredRoute& rhs) { return lhs.id < rhs.id; });
  std::sort(workloads.begin(), workloads.end(),
            [](const DesiredWorkload& lhs, const DesiredWorkload& rhs) { return lhs.id < rhs.id; });
  std::sort(policies.begin(), policies.end(),
            [](const DesiredPolicy& lhs, const DesiredPolicy& rhs) { return lhs.id < rhs.id; });
  std::sort(configs.begin(), configs.end(), [](const DesiredConfig& lhs, const DesiredConfig& rhs) {
    if (lhs.device != rhs.device) return lhs.device < rhs.device;
    return lhs.key < rhs.key;
  });
  for (DesiredDevice& desired : devices) {
    std::sort(desired.exclusions.begin(), desired.exclusions.end());
    std::sort(desired.maintenance_windows.begin(), desired.maintenance_windows.end());
  }
  for (DesiredLink& desired : links) {
    std::sort(desired.spec.transit_domains.begin(), desired.spec.transit_domains.end());
    desired.spec.transit_domains.erase(
        std::unique(desired.spec.transit_domains.begin(), desired.spec.transit_domains.end()),
        desired.spec.transit_domains.end());
  }
  for (DesiredWorkload& desired : workloads) {
    std::sort(desired.bindings.begin(), desired.bindings.end());
    std::sort(desired.requirements.forbidden_domains.begin(),
              desired.requirements.forbidden_domains.end());
    desired.requirements.forbidden_domains.erase(
        std::unique(desired.requirements.forbidden_domains.begin(),
                    desired.requirements.forbidden_domains.end()),
        desired.requirements.forbidden_domains.end());
  }
}

namespace {

void encode_action(CanonicalEncoder& out, DesiredAction action) {
  encode_value(out, static_cast<std::uint8_t>(action));
}

DesiredAction decode_action(CanonicalDecoder& in) {
  const std::uint8_t raw = in.get_u8();
  if (!in.ok()) {
    return DesiredAction::ensure;
  }
  if (raw > static_cast<std::uint8_t>(DesiredAction::remove)) {
    in.fail(ErrorCode::malformed_input, "desired action out of range during decode");
    return DesiredAction::ensure;
  }
  return static_cast<DesiredAction>(raw);
}

void encode_desired_device(CanonicalEncoder& out, const DesiredDevice& value) {
  out.put_tag("desired-device");
  encode_action(out, value.action);
  encode_value(out, value.id);
  encode_device_spec(out, value.spec);
  encode_list(out, value.exclusions, encode_exclusion_window);
  encode_list(out, value.maintenance_windows, encode_maintenance_window);
}

DesiredDevice decode_desired_device(CanonicalDecoder& in) {
  DesiredDevice value;
  in.get_tag("desired-device");
  value.action = decode_action(in);
  value.id = decode_value<DeviceId>(in);
  value.spec = decode_device_spec(in);
  value.exclusions = decode_list<ExclusionWindow>(in, 8, decode_exclusion_window);
  value.maintenance_windows = decode_list<MaintenanceWindow>(in, 8, decode_maintenance_window);
  return value;
}

void encode_desired_link(CanonicalEncoder& out, const DesiredLink& value) {
  out.put_tag("desired-link");
  encode_action(out, value.action);
  encode_value(out, value.id);
  encode_link_spec(out, value.spec);
}

DesiredLink decode_desired_link(CanonicalDecoder& in) {
  DesiredLink value;
  in.get_tag("desired-link");
  value.action = decode_action(in);
  value.id = decode_value<LinkId>(in);
  value.spec = decode_link_spec(in);
  return value;
}

void encode_desired_route(CanonicalEncoder& out, const DesiredRoute& value) {
  out.put_tag("desired-route");
  encode_action(out, value.action);
  encode_value(out, value.id);
  encode_route_spec(out, value.spec);
  encode_value(out, static_cast<std::uint8_t>(value.state));
}

DesiredRoute decode_desired_route(CanonicalDecoder& in) {
  DesiredRoute value;
  in.get_tag("desired-route");
  value.action = decode_action(in);
  value.id = decode_value<RouteId>(in);
  value.spec = decode_route_spec(in);
  const std::uint8_t raw_state = in.get_u8();
  if (!in.ok()) {
    return value;
  }
  if (raw_state > static_cast<std::uint8_t>(RouteState::active)) {
    in.fail(ErrorCode::malformed_input, "route state out of range during decode");
    return value;
  }
  value.state = static_cast<RouteState>(raw_state);
  return value;
}

void encode_desired_workload(CanonicalEncoder& out, const DesiredWorkload& value) {
  out.put_tag("desired-workload");
  encode_action(out, value.action);
  encode_value(out, value.id);
  encode_list(out, value.bindings, encode_route_binding);
  encode_workload_requirements(out, value.requirements);
  encode_value(out, value.contract_code);
}

DesiredWorkload decode_desired_workload(CanonicalDecoder& in) {
  DesiredWorkload value;
  in.get_tag("desired-workload");
  value.action = decode_action(in);
  value.id = decode_value<WorkloadId>(in);
  value.bindings = decode_list<RouteBinding>(in, 9, decode_route_binding);
  value.requirements = decode_workload_requirements(in);
  value.contract_code = in.get_text();
  return value;
}

void encode_desired_policy(CanonicalEncoder& out, const DesiredPolicy& value) {
  out.put_tag("desired-policy");
  encode_action(out, value.action);
  encode_value(out, value.id);
  encode_policy_target(out, value.target);
  encode_value(out, value.value_code);
  encode_value(out, value.enforced);
}

DesiredPolicy decode_desired_policy(CanonicalDecoder& in) {
  DesiredPolicy value;
  in.get_tag("desired-policy");
  value.action = decode_action(in);
  value.id = decode_value<PolicyId>(in);
  value.target = decode_policy_target(in);
  value.value_code = in.get_text();
  value.enforced = in.get_bool();
  return value;
}

void encode_desired_config(CanonicalEncoder& out, const DesiredConfig& value) {
  out.put_tag("desired-config");
  encode_action(out, value.action);
  encode_value(out, value.device);
  encode_value(out, value.key);
  encode_value(out, value.value_code);
}

DesiredConfig decode_desired_config(CanonicalDecoder& in) {
  DesiredConfig value;
  in.get_tag("desired-config");
  value.action = decode_action(in);
  value.device = decode_value<DeviceId>(in);
  value.key = decode_value<ConfigKey>(in);
  value.value_code = in.get_text();
  return value;
}

}  // namespace

void TargetIntent::encode(CanonicalEncoder& out) const {
  out.put_tag("cplan.target.v1");
  encode_value(out, revision);
  encode_value(out, based_on_generation);
  encode_value(out, authority);
  encode_value(out, authority_generation);
  encode_value(out, based_on_epoch);
  encode_value(out, authored_at);
  encode_value(out, intent_code);
  encode_list(out, devices, encode_desired_device);
  encode_list(out, links, encode_desired_link);
  encode_list(out, routes, encode_desired_route);
  encode_list(out, workloads, encode_desired_workload);
  encode_list(out, policies, encode_desired_policy);
  encode_list(out, configs, encode_desired_config);
}

TargetIntent TargetIntent::decode(CanonicalDecoder& in) {
  TargetIntent target;
  in.get_tag("cplan.target.v1");
  target.revision = decode_value<TargetRevision>(in);
  target.based_on_generation = decode_value<TopologyGeneration>(in);
  target.authority = decode_value<AuthorityId>(in);
  target.authority_generation = decode_value<AuthorityGeneration>(in);
  target.based_on_epoch = decode_value<Epoch>(in);
  target.authored_at = decode_value<TimestampNs>(in);
  target.intent_code = in.get_text();
  target.devices = decode_list<DesiredDevice>(in, 8, decode_desired_device);
  target.links = decode_list<DesiredLink>(in, 8, decode_desired_link);
  target.routes = decode_list<DesiredRoute>(in, 8, decode_desired_route);
  target.workloads = decode_list<DesiredWorkload>(in, 8, decode_desired_workload);
  target.policies = decode_list<DesiredPolicy>(in, 8, decode_desired_policy);
  target.configs = decode_list<DesiredConfig>(in, 8, decode_desired_config);
  return target;
}

TargetIntent target_from_state(const CurrentStateSnapshot& state, TargetRevision revision,
                                 std::string intent_code) {
  TargetIntent target;
  target.revision = revision;
  target.based_on_generation = state.topology_generation;
  target.authority = state.authority;
  target.authority_generation = state.authority_generation;
  target.based_on_epoch = state.epoch;
  target.authored_at = state.observed_at;
  target.intent_code = std::move(intent_code);
  for (const Device& device : state.devices) {
    DesiredDevice desired;
    desired.action = device.spec.state == EntityState::removed ? DesiredAction::remove
                                                              : DesiredAction::ensure;
    desired.id = device.id;
    desired.spec = device.spec;
    desired.exclusions = device.exclusions;
    desired.maintenance_windows = device.maintenance_windows;
    target.devices.push_back(std::move(desired));
  }
  for (const Link& link : state.links) {
    DesiredLink desired;
    desired.action = link.state == EntityState::removed ? DesiredAction::remove
                                                        : DesiredAction::ensure;
    desired.id = link.id;
    desired.spec = link.spec;
    target.links.push_back(std::move(desired));
  }
  for (const Route& route : state.routes) {
    DesiredRoute desired;
    desired.action = DesiredAction::ensure;
    desired.id = route.id;
    desired.spec = route.spec;
    desired.state = route.state;
    target.routes.push_back(std::move(desired));
  }
  for (const Workload& workload : state.workloads) {
    DesiredWorkload desired;
    desired.action = DesiredAction::ensure;
    desired.id = workload.id;
    desired.bindings = workload.bindings;
    desired.requirements = workload.requirements;
    desired.contract_code = workload.contract_code;
    target.workloads.push_back(std::move(desired));
  }
  for (const PolicyObject& policy : state.policies) {
    DesiredPolicy desired;
    desired.action = DesiredAction::ensure;
    desired.id = policy.id;
    desired.target = policy.target;
    desired.value_code = policy.value_code;
    desired.enforced = policy.enforced;
    target.policies.push_back(std::move(desired));
  }
  for (const ConfigEntry& entry : state.configs) {
    DesiredConfig desired;
    desired.action = DesiredAction::ensure;
    desired.device = entry.device;
    desired.key = entry.key;
    desired.value_code = entry.value_code;
    target.configs.push_back(std::move(desired));
  }
  target.canonicalize();
  return target;
}

}  // namespace cplan
