#include "change_planner/model/evidence.hpp"

#include <algorithm>
#include <set>

#include "change_planner/core/checked.hpp"

namespace cplan {

Result<void> TopologyEvidence::validate() const {
  if (topology_generation.value() == 0) {
    return Status::error(ErrorCode::invalid_argument,
                         "topology evidence must name a non-zero topology generation");
  }
  std::set<std::string> seen;
  for (const CapabilityEvidence& entry : devices) {
    if (!seen.insert(entry.device.str()).second) {
      return Status::error(ErrorCode::duplicate_identity,
                           "duplicate capability evidence for device '" + entry.device.str() + "'");
    }
  }
  return Status::success();
}

Digest TopologyEvidence::digest() const {
  CanonicalEncoder encoder;
  encode(encoder);
  return encoder.digest();
}

void TopologyEvidence::canonicalize() {
  std::sort(devices.begin(), devices.end(),
            [](const CapabilityEvidence& lhs, const CapabilityEvidence& rhs) {
              return lhs.device < rhs.device;
            });
}

void TopologyEvidence::encode(CanonicalEncoder& out) const {
  out.put_tag("cplan.evidence.v1");
  encode_value(out, topology_generation);
  encode_value(out, boot_incarnation);
  encode_value(out, capability_version);
  encode_value(out, observed_at);
  out.put_u32(static_cast<std::uint32_t>(devices.size()));
  for (const CapabilityEvidence& entry : devices) {
    encode_value(out, entry.device);
    out.put_u32(entry.capabilities.mask());
    encode_value(out, entry.capability_version);
  }
}

TopologyEvidence TopologyEvidence::decode(CanonicalDecoder& in) {
  TopologyEvidence evidence;
  in.get_tag("cplan.evidence.v1");
  evidence.topology_generation = decode_value<TopologyGeneration>(in);
  evidence.boot_incarnation = decode_value<BootIncarnation>(in);
  evidence.capability_version = decode_value<CapabilityVersion>(in);
  evidence.observed_at = decode_value<TimestampNs>(in);
  const std::uint32_t count = in.get_count(16);
  if (!in.ok()) {
    return evidence;
  }
  evidence.devices.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    CapabilityEvidence entry;
    entry.device = decode_value<DeviceId>(in);
    entry.capabilities = CapabilitySet(in.get_u32());
    entry.capability_version = decode_value<CapabilityVersion>(in);
    if (!in.ok()) {
      return {};
    }
    evidence.devices.push_back(std::move(entry));
  }
  return evidence;
}

const CapabilityEvidence* TopologyEvidence::find(const DeviceId& id) const {
  for (const CapabilityEvidence& entry : devices) {
    if (entry.device == id) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace cplan
