#pragma once

#include <string>
#include <vector>

#include "change_planner/model/state.hpp"

namespace cplan {

// Declarative target intent: what the operator wants the fabric to look like.
// Every entry is either "ensure this exact configuration" or "remove this
// entity". The planner derives the minimal ordered transition, never the target
// document itself.
enum class DesiredAction : std::uint8_t { ensure = 0, remove };

const char* to_string(DesiredAction action) noexcept;
Result<DesiredAction> parse_desired_action(std::string_view token);

struct DesiredDevice {
  DesiredAction action{DesiredAction::ensure};
  DeviceId id;
  DeviceSpec spec;
  std::vector<ExclusionWindow> exclusions;
  std::vector<MaintenanceWindow> maintenance_windows;

  friend bool operator==(const DesiredDevice&, const DesiredDevice&) = default;
};

struct DesiredLink {
  DesiredAction action{DesiredAction::ensure};
  LinkId id;
  LinkSpec spec;

  friend bool operator==(const DesiredLink&, const DesiredLink&) = default;
};

struct DesiredRoute {
  DesiredAction action{DesiredAction::ensure};
  RouteId id;
  RouteSpec spec;
  RouteState state{RouteState::active};

  friend bool operator==(const DesiredRoute&, const DesiredRoute&) = default;
};

struct DesiredWorkload {
  DesiredAction action{DesiredAction::ensure};
  WorkloadId id;
  std::vector<RouteBinding> bindings;
  WorkloadRequirements requirements;
  std::string contract_code;

  friend bool operator==(const DesiredWorkload&, const DesiredWorkload&) = default;
};

struct DesiredPolicy {
  DesiredAction action{DesiredAction::ensure};
  PolicyId id;
  PolicyTarget target;
  std::string value_code;
  bool enforced{true};

  friend bool operator==(const DesiredPolicy&, const DesiredPolicy&) = default;
};

struct DesiredConfig {
  DesiredAction action{DesiredAction::ensure};
  DeviceId device;
  ConfigKey key;
  std::string value_code;

  friend bool operator==(const DesiredConfig&, const DesiredConfig&) = default;
};

struct TargetIntent {
  TargetRevision revision;
  TopologyGeneration based_on_generation;
  AuthorityId authority;
  AuthorityGeneration authority_generation;
  Epoch based_on_epoch;
  TimestampNs authored_at;
  std::string intent_code;

  std::vector<DesiredDevice> devices;
  std::vector<DesiredLink> links;
  std::vector<DesiredRoute> routes;
  std::vector<DesiredWorkload> workloads;
  std::vector<DesiredPolicy> policies;
  std::vector<DesiredConfig> configs;

  friend bool operator==(const TargetIntent&, const TargetIntent&) = default;

  [[nodiscard]] Result<void> validate() const;
  [[nodiscard]] Digest digest() const;

  void canonicalize();
  void encode(CanonicalEncoder& out) const;
  static TargetIntent decode(CanonicalDecoder& in);
};

// Converts an authoritative snapshot into the equivalent declarative target:
// every present entity is ensured with its current configuration. Used for
// rollback planning and for CLI-driven "keep the fabric as it is" intents.
[[nodiscard]] TargetIntent target_from_state(const CurrentStateSnapshot& state,
                                             TargetRevision revision,
                                             std::string intent_code);

}  // namespace cplan
