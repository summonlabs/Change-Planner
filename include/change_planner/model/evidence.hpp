#pragma once

#include <map>
#include <string>
#include <vector>

#include "change_planner/model/state.hpp"

namespace cplan {

// Capability evidence is what the runtime actually verified about a device: the
// operations the device supports, the capability version that was observed, and
// the topology generation it was observed under. Evidence never becomes fresh
// because it can be deserialized: it is bound to a generation and incarnation.
struct CapabilityEvidence {
  DeviceId device;
  CapabilitySet capabilities;
  CapabilityVersion capability_version;

  friend bool operator==(const CapabilityEvidence&, const CapabilityEvidence&) = default;
};

struct TopologyEvidence {
  TopologyGeneration topology_generation;
  BootIncarnation boot_incarnation;
  CapabilityVersion capability_version;
  TimestampNs observed_at;
  std::vector<CapabilityEvidence> devices;

  friend bool operator==(const TopologyEvidence&, const TopologyEvidence&) = default;

  [[nodiscard]] Result<void> validate() const;
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] const CapabilityEvidence* find(const DeviceId& id) const;

  void canonicalize();
  void encode(CanonicalEncoder& out) const;
  static TopologyEvidence decode(CanonicalDecoder& in);
};

}  // namespace cplan
