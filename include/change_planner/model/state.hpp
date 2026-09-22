#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "change_planner/core/canonical.hpp"
#include "change_planner/model/ids.hpp"

namespace cplan {

// ---------------------------------------------------------------------------
// Enumerations carry an explicit textual token so that persisted plans, CLI
// output and diagnostics stay stable across builds.
// ---------------------------------------------------------------------------

enum class EntityState : std::uint8_t {
  absent = 0,
  present,
  draining,
  in_maintenance,
  removed,
};

const char* to_string(EntityState state) noexcept;
Result<EntityState> parse_entity_state(std::string_view token);

enum class RouteState : std::uint8_t { inactive = 0, active };

const char* to_string(RouteState state) noexcept;
Result<RouteState> parse_route_state(std::string_view token);

enum class PolicyScope : std::uint8_t { global = 0, device, link, route, workload };

const char* to_string(PolicyScope scope) noexcept;
Result<PolicyScope> parse_policy_scope(std::string_view token);

// Capabilities are the operations the underlying Fabric OS runtime is able to
// perform on a device. A change whose capability is absent is an impossible
// transition, not a scheduling problem.
enum class ChangeCapability : std::uint32_t {
  none = 0,
  device_add = 1u << 0,
  device_remove = 1u << 1,
  device_capacity = 1u << 2,
  device_state = 1u << 3,
  device_drain = 1u << 4,
  device_maintenance = 1u << 5,
  link_add = 1u << 6,
  link_remove = 1u << 7,
  link_capacity = 1u << 8,
  route_add = 1u << 9,
  route_remove = 1u << 10,
  route_state = 1u << 11,
  policy_bind = 1u << 12,
  config_set = 1u << 13,
  workload_binding = 1u << 14,
  workload_requirements = 1u << 15,
};

const char* to_string(ChangeCapability capability) noexcept;
Result<ChangeCapability> parse_change_capability(std::string_view token);

class CapabilitySet {
 public:
  constexpr CapabilitySet() noexcept = default;
  constexpr explicit CapabilitySet(std::uint32_t mask) noexcept : mask_(mask) {}

  [[nodiscard]] constexpr bool has(ChangeCapability capability) const noexcept {
    return (mask_ & static_cast<std::uint32_t>(capability)) != 0;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return mask_ == 0; }
  [[nodiscard]] constexpr std::uint32_t mask() const noexcept { return mask_; }

  constexpr CapabilitySet& add(ChangeCapability capability) noexcept {
    mask_ |= static_cast<std::uint32_t>(capability);
    return *this;
  }
  [[nodiscard]] CapabilitySet intersect(const CapabilitySet& other) const noexcept {
    return CapabilitySet(mask_ & other.mask_);
  }
  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(const CapabilitySet&, const CapabilitySet&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const CapabilitySet& lhs,
                                                    const CapabilitySet& rhs) noexcept {
    return lhs.mask_ <=> rhs.mask_;
  }

 private:
  std::uint32_t mask_{0};
};

// ---------------------------------------------------------------------------
// Structural model
// ---------------------------------------------------------------------------

// Interval in which changes to a device are forbidden.
struct ExclusionWindow {
  WindowId id;
  TimestampNs begin;
  TimestampNs end;
  std::string reason_code;

  friend bool operator==(const ExclusionWindow&, const ExclusionWindow&) = default;
  friend bool operator<(const ExclusionWindow& lhs, const ExclusionWindow& rhs) {
    if (lhs.begin != rhs.begin) return lhs.begin < rhs.begin;
    if (lhs.end != rhs.end) return lhs.end < rhs.end;
    return lhs.id < rhs.id;
  }
  [[nodiscard]] bool contains(TimestampNs instant) const noexcept {
    return instant >= begin && instant < end;
  }
};

// Interval in which maintenance work on a device is permitted.
struct MaintenanceWindow {
  WindowId id;
  TimestampNs begin;
  TimestampNs end;
  std::string ticket_code;

  friend bool operator==(const MaintenanceWindow&, const MaintenanceWindow&) = default;
  friend bool operator<(const MaintenanceWindow& lhs, const MaintenanceWindow& rhs) {
    if (lhs.begin != rhs.begin) return lhs.begin < rhs.begin;
    if (lhs.end != rhs.end) return lhs.end < rhs.end;
    return lhs.id < rhs.id;
  }
  [[nodiscard]] bool contains(TimestampNs instant) const noexcept {
    return instant >= begin && instant < end;
  }
};

struct DeviceSpec {
  FailureDomainId domain;
  EntityState state{EntityState::present};
  CapacityUnits termination_capacity;
  CapacityUnits termination_reserved;
  CapabilitySet capabilities;
  std::uint32_t max_concurrent_changes{1};
  std::string profile_code;

  friend bool operator==(const DeviceSpec&, const DeviceSpec&) = default;
};

struct Device {
  DeviceId id;
  DeviceSpec spec;
  std::vector<ExclusionWindow> exclusions;
  std::vector<MaintenanceWindow> maintenance_windows;

  friend bool operator==(const Device&, const Device&) = default;
};

struct LinkSpec {
  DeviceId endpoint_a;
  DeviceId endpoint_b;
  CapacityUnits capacity;
  CapacityUnits reserved;
  std::uint32_t latency_class{0};
  // Failure domains the link traverses beyond its endpoints (transit domains).
  std::vector<FailureDomainId> transit_domains;

  friend bool operator==(const LinkSpec&, const LinkSpec&) = default;
};

struct Link {
  LinkId id;
  LinkSpec spec;
  EntityState state{EntityState::present};

  friend bool operator==(const Link&, const Link&) = default;

  [[nodiscard]] bool connects(const DeviceId& device) const {
    return spec.endpoint_a == device || spec.endpoint_b == device;
  }
  [[nodiscard]] DeviceId other_endpoint(const DeviceId& device) const {
    return spec.endpoint_a == device ? spec.endpoint_b : spec.endpoint_a;
  }
};

struct RouteSpec {
  DeviceId source;
  DeviceId sink;
  std::vector<LinkId> path;
  PolicyId policy;

  friend bool operator==(const RouteSpec&, const RouteSpec&) = default;
};

struct Route {
  RouteId id;
  RouteSpec spec;
  RouteState state{RouteState::inactive};

  friend bool operator==(const Route&, const Route&) = default;
};

struct RouteBinding {
  RouteId route;
  bool active{false};
  DemandUnits demand;

  friend bool operator==(const RouteBinding&, const RouteBinding&) = default;
  friend bool operator<(const RouteBinding& lhs, const RouteBinding& rhs) {
    return lhs.route < rhs.route;
  }
};

struct WorkloadRequirements {
  bool require_connectivity{false};
  std::uint32_t min_disjoint_domains{0};
  std::uint32_t min_link_disjoint_paths{0};
  CapacityUnits min_guaranteed_capacity;
  std::vector<FailureDomainId> forbidden_domains;

  friend bool operator==(const WorkloadRequirements&, const WorkloadRequirements&) = default;
};

struct Workload {
  WorkloadId id;
  std::vector<RouteBinding> bindings;
  WorkloadRequirements requirements;
  std::string contract_code;

  friend bool operator==(const Workload&, const Workload&) = default;
};

struct PolicyTarget {
  PolicyScope scope{PolicyScope::global};
  std::string identity;  // empty for global scope, identity text otherwise

  friend bool operator==(const PolicyTarget&, const PolicyTarget&) = default;
  friend bool operator<(const PolicyTarget& lhs, const PolicyTarget& rhs) {
    if (lhs.scope != rhs.scope) return lhs.scope < rhs.scope;
    return lhs.identity < rhs.identity;
  }
};

struct PolicyObject {
  PolicyId id;
  PolicyTarget target;
  std::string value_code;
  bool enforced{true};

  friend bool operator==(const PolicyObject&, const PolicyObject&) = default;
};

struct ConfigEntry {
  DeviceId device;
  ConfigKey key;
  std::string value_code;
  ConfigRevision revision;

  friend bool operator==(const ConfigEntry&, const ConfigEntry&) = default;
  friend bool operator<(const ConfigEntry& lhs, const ConfigEntry& rhs) {
    if (lhs.device != rhs.device) return lhs.device < rhs.device;
    return lhs.key < rhs.key;
  }
};

// ---------------------------------------------------------------------------
// Current authoritative state
// ---------------------------------------------------------------------------

// A snapshot is generation bound: the topology generation, authority identity,
// authority generation, boot incarnation and epoch it was observed under travel
// with the data. Stale snapshots are rejected rather than silently planned
// against.
struct CurrentStateSnapshot {
  TopologyGeneration topology_generation;
  AuthorityId authority;
  AuthorityGeneration authority_generation;
  BootIncarnation boot_incarnation;
  Epoch epoch;
  TimestampNs observed_at;
  std::vector<Device> devices;
  std::vector<Link> links;
  std::vector<Route> routes;
  std::vector<Workload> workloads;
  std::vector<PolicyObject> policies;
  std::vector<ConfigEntry> configs;

  friend bool operator==(const CurrentStateSnapshot&, const CurrentStateSnapshot&) = default;

  // Structural validation: duplicate identities, dangling references,
  // non-walkable routes, malformed windows. Returns a typed failure that names
  // the offending entity.
  [[nodiscard]] Result<void> validate() const;

  // Deterministic content digest over the canonical encoding of every field.
  [[nodiscard]] Digest digest() const;

  [[nodiscard]] const Device* find_device(const DeviceId& id) const;
  [[nodiscard]] const Link* find_link(const LinkId& id) const;
  [[nodiscard]] const Route* find_route(const RouteId& id) const;
  [[nodiscard]] const Workload* find_workload(const WorkloadId& id) const;
  [[nodiscard]] const PolicyObject* find_policy(const PolicyId& id) const;
  [[nodiscard]] const ConfigEntry* find_config(const DeviceId& device, const ConfigKey& key) const;

  void encode(CanonicalEncoder& out) const;
  static CurrentStateSnapshot decode(CanonicalDecoder& in);

  // Sorts every collection into canonical (deterministic) order and normalises
  // nested collections. Applied after construction and after every mutation.
  void canonicalize();
};

// Index over a snapshot for repeated lookups during planning.
class StateIndex {
 public:
  explicit StateIndex(const CurrentStateSnapshot& state);

  [[nodiscard]] const Device* device(const DeviceId& id) const;
  [[nodiscard]] const Link* link(const LinkId& id) const;
  [[nodiscard]] const Route* route(const RouteId& id) const;
  [[nodiscard]] const Workload* workload(const WorkloadId& id) const;
  [[nodiscard]] const PolicyObject* policy(const PolicyId& id) const;
  [[nodiscard]] const ConfigEntry* config(const DeviceId& device, const ConfigKey& key) const;

  // Devices that participate in a failure domain.
  [[nodiscard]] std::vector<DeviceId> devices_in_domain(const FailureDomainId& domain) const;
  // Active plus inactive routes bound by a workload.
  [[nodiscard]] std::vector<RouteId> workload_routes(const WorkloadId& id) const;
  // Links that are incident to a device.
  [[nodiscard]] std::vector<LinkId> incident_links(const DeviceId& id) const;
  // Workloads whose active bindings traverse a link.
  [[nodiscard]] std::vector<WorkloadId> workloads_on_link(const LinkId& id) const;
  // Workloads whose active bindings terminate on a device.
  [[nodiscard]] std::vector<WorkloadId> workloads_on_device(const DeviceId& id) const;

 private:
  const CurrentStateSnapshot* state_;
  std::map<DeviceId, std::size_t> device_slots_;
  std::map<LinkId, std::size_t> link_slots_;
  std::map<RouteId, std::size_t> route_slots_;
  std::map<WorkloadId, std::size_t> workload_slots_;
  std::map<PolicyId, std::size_t> policy_slots_;
  std::map<std::pair<DeviceId, ConfigKey>, std::size_t> config_slots_;
};

// ---------------------------------------------------------------------------
// Shared canonical codecs for model components. These are explicit free
// functions rather than implicit template dispatch so that every byte written to
// an artifact is visible at the call site.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxTokenBytes = 512;

void encode_exclusion_window(CanonicalEncoder& out, const ExclusionWindow& value);
ExclusionWindow decode_exclusion_window(CanonicalDecoder& in);
void encode_maintenance_window(CanonicalEncoder& out, const MaintenanceWindow& value);
MaintenanceWindow decode_maintenance_window(CanonicalDecoder& in);

void encode_device_spec(CanonicalEncoder& out, const DeviceSpec& value);
DeviceSpec decode_device_spec(CanonicalDecoder& in);
void encode_device(CanonicalEncoder& out, const Device& value);
Device decode_device(CanonicalDecoder& in);

void encode_link_spec(CanonicalEncoder& out, const LinkSpec& value);
LinkSpec decode_link_spec(CanonicalDecoder& in);
void encode_link(CanonicalEncoder& out, const Link& value);
Link decode_link(CanonicalDecoder& in);

void encode_route_spec(CanonicalEncoder& out, const RouteSpec& value);
RouteSpec decode_route_spec(CanonicalDecoder& in);
void encode_route(CanonicalEncoder& out, const Route& value);
Route decode_route(CanonicalDecoder& in);

void encode_route_binding(CanonicalEncoder& out, const RouteBinding& value);
RouteBinding decode_route_binding(CanonicalDecoder& in);
void encode_workload_requirements(CanonicalEncoder& out, const WorkloadRequirements& value);
WorkloadRequirements decode_workload_requirements(CanonicalDecoder& in);
void encode_workload(CanonicalEncoder& out, const Workload& value);
Workload decode_workload(CanonicalDecoder& in);

void encode_policy_target(CanonicalEncoder& out, const PolicyTarget& value);
PolicyTarget decode_policy_target(CanonicalDecoder& in);
void encode_policy_object(CanonicalEncoder& out, const PolicyObject& value);
PolicyObject decode_policy_object(CanonicalDecoder& in);

void encode_config_entry(CanonicalEncoder& out, const ConfigEntry& value);
ConfigEntry decode_config_entry(CanonicalDecoder& in);

// Walks a route from its source to its sink over existing links. Returns false
// when the path is not connected in the declared order or references a missing
// link.
[[nodiscard]] bool route_is_walkable(const CurrentStateSnapshot& state, const Route& route);

// Failure domains a route depends on: the domains of every device it traverses
// plus the transit domains declared by its links.
[[nodiscard]] std::vector<FailureDomainId> route_domains(const CurrentStateSnapshot& state,
                                                         const Route& route);

// Total demand a workload places on a link when the given bindings are active.
[[nodiscard]] DemandUnits workload_demand_per_binding(const Workload& workload,
                                                      const RouteBinding& binding);

}  // namespace cplan
