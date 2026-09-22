#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "change_planner/model/state.hpp"

namespace cplan {

// ---------------------------------------------------------------------------
// Heterogeneous entity references. Domain APIs use the strongly typed identity
// types; a reference is used where a set of different entity kinds must be
// compared, ordered and explained.
// ---------------------------------------------------------------------------
enum class EntityKind : std::uint8_t {
  global = 0,
  device,
  link,
  route,
  workload,
  failure_domain,
  policy,
  config,
  contract,
};

const char* to_string(EntityKind kind) noexcept;
Result<EntityKind> parse_entity_kind(std::string_view token);

struct EntityRef {
  EntityKind kind{EntityKind::global};
  std::string identity;

  friend bool operator==(const EntityRef&, const EntityRef&) = default;
  friend bool operator<(const EntityRef& lhs, const EntityRef& rhs) {
    if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
    return lhs.identity < rhs.identity;
  }
  [[nodiscard]] std::string to_string() const;

  static EntityRef of(const DeviceId& id);
  static EntityRef of(const LinkId& id);
  static EntityRef of(const RouteId& id);
  static EntityRef of(const WorkloadId& id);
  static EntityRef of(const FailureDomainId& id);
  static EntityRef of(const PolicyId& id);
  static EntityRef of(const DeviceId& device, const ConfigKey& key);
  static EntityRef of(const ContractId& id);
  static EntityRef global();
};

// ---------------------------------------------------------------------------
// Operations: the explicit transition vocabulary. Every change the planner can
// emit is one of these typed operations; nothing is implied or executed here.
// ---------------------------------------------------------------------------
enum class OperationKind : std::uint8_t {
  device_add = 0,
  device_remove,
  device_set_capacity,
  device_set_state,
  device_set_windows,
  link_add,
  link_remove,
  link_set_capacity,
  route_add,
  route_remove,
  route_set_path,
  route_set_state,
  workload_add,
  workload_remove,
  workload_set_bindings,
  workload_set_requirements,
  policy_bind,
  policy_unbind,
  config_set,
  config_clear,
  drain_resource,
  enter_maintenance,
  exit_maintenance,
};

const char* to_string(OperationKind kind) noexcept;
Result<OperationKind> parse_operation_kind(std::string_view token);

// Whether an operation is a change step (this planner emits it for another
// runtime) or a maintenance/drain step (emitted for the maintenance runtime).
enum class OperationClass : std::uint8_t { configuration = 0, maintenance };

const char* to_string(OperationClass operation_class) noexcept;

struct Operation {
  OperationKind kind{OperationKind::device_add};

  DeviceId device;
  DeviceSpec device_spec;
  std::vector<ExclusionWindow> exclusions;
  std::vector<MaintenanceWindow> maintenance_windows;
  EntityState entity_state{EntityState::present};
  CapacityUnits capacity;

  LinkId link;
  LinkSpec link_spec;

  WorkloadId workload;
  std::vector<RouteBinding> bindings;
  WorkloadRequirements requirements;
  std::string contract_code;

  RouteId route;
  RouteSpec route_spec;
  RouteState route_state{RouteState::inactive};

  PolicyId policy;
  PolicyTarget policy_target;
  std::string value_code;
  bool enforced{true};

  ConfigKey config_key;

  WindowId window;
  DurationNs estimated_duration;
  RiskScore risk;
  std::string reason_code;

  friend bool operator==(const Operation&, const Operation&) = default;

  // Deterministic identity: digest over kind, target identity and full payload.
  [[nodiscard]] Digest identity_digest() const;

  // Total order used for deterministic tie-breaking. Identical inputs always
  // produce the same ordering.
  friend bool operator<(const Operation& lhs, const Operation& rhs);

  [[nodiscard]] OperationClass operation_class() const;
  [[nodiscard]] bool requires_maintenance_window() const;

  // Entities this operation replaces or mutates.
  [[nodiscard]] std::vector<EntityRef> write_set() const;
  // Entities this operation validates or depends on.
  [[nodiscard]] std::vector<EntityRef> read_set() const;
  // Devices whose runtime must support the operation. Resolved against the
  // state the operation is planned from, so workload and route operations name
  // the devices that actually carry them.
  [[nodiscard]] std::vector<DeviceId> capability_subjects(
      const CurrentStateSnapshot& state) const;
  [[nodiscard]] ChangeCapability required_capability() const;
  // Primary identity of the operation target (used for stable ordering).
  [[nodiscard]] std::string target_identity() const;

  [[nodiscard]] std::string to_string() const;

  void encode(CanonicalEncoder& out) const;
  static Operation decode(CanonicalDecoder& in);
};

// ---------------------------------------------------------------------------
// Step conditions: typed predicates evaluated against a simulated state.
// ---------------------------------------------------------------------------
enum class ConditionKind : std::uint8_t {
  entity_absent = 0,
  entity_present,
  entity_state_is,
  route_walkable,
  route_state_is,
  config_equals,
  policy_bound,
  binding_active,
  capacity_at_least,
  invariant_satisfied,
  maintenance_window_open,
  exclusion_inactive,
  device_drained,
};

const char* to_string(ConditionKind kind) noexcept;
Result<ConditionKind> parse_condition_kind(std::string_view token);

struct Condition {
  ConditionKind kind{ConditionKind::entity_present};
  EntityRef subject;
  // Parameter token: state name, invariant name, or expected value.
  std::string parameter;
  std::uint64_t quantity{0};

  friend bool operator==(const Condition&, const Condition&) = default;
  friend bool operator<(const Condition& lhs, const Condition& rhs) {
    if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
    if (lhs.subject != rhs.subject) return lhs.subject < rhs.subject;
    if (lhs.parameter != rhs.parameter) return lhs.parameter < rhs.parameter;
    return lhs.quantity < rhs.quantity;
  }

  [[nodiscard]] std::string to_string() const;

  void encode(CanonicalEncoder& out) const;
  static Condition decode(CanonicalDecoder& in);
};

void encode_entity_ref(CanonicalEncoder& out, const EntityRef& value);
EntityRef decode_entity_ref(CanonicalDecoder& in);
void encode_operation(CanonicalEncoder& out, const Operation& value);
Operation decode_operation(CanonicalDecoder& in);
void encode_condition(CanonicalEncoder& out, const Condition& value);
Condition decode_condition(CanonicalDecoder& in);

}  // namespace cplan
