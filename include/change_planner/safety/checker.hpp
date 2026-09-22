#pragma once

#include <string>
#include <vector>

#include "change_planner/model/constraints.hpp"
#include "change_planner/model/evidence.hpp"
#include "change_planner/model/state.hpp"
#include "change_planner/safety/invariant.hpp"

namespace cplan {

// Evaluation context: the logical planning instant plus the governing policy and
// the capability evidence that was observed for the snapshot.
struct EvaluationContext {
  TimestampNs instant;
  const SafetyConstraints* constraints{nullptr};
  const TopologyEvidence* evidence{nullptr};
  // The authoritative snapshot a plan started from. Devices that do not appear
  // in it are created by the plan under evaluation, so no capability evidence can
  // exist for them yet: they are exempt from the pre-existing-evidence
  // requirement (they are still capability-gated by their declared target spec).
  // A null baseline means every device must be evidenced.
  const CurrentStateSnapshot* baseline{nullptr};
};

// Full invariant evaluation of a state under the declared constraints. Results
// are deterministically ordered so that two runs produce identical diagnostics.
[[nodiscard]] std::vector<Violation> check_invariants(const CurrentStateSnapshot& state,
                                                      const EvaluationContext& context);

// True when no violation with Severity::violation is present.
[[nodiscard]] bool state_is_safe(const CurrentStateSnapshot& state,
                                 const EvaluationContext& context);

// Window predicates used to explain refusals precisely.
[[nodiscard]] bool change_permitted_at(const Device& device, TimestampNs instant,
                                       std::string* blocking_reason);
[[nodiscard]] bool maintenance_permitted_at(const Device& device, TimestampNs instant,
                                            std::string* blocking_reason);

// Devices currently out of service in a failure domain (draining, in
// maintenance or removed). Used by the serialization constraint.
[[nodiscard]] std::vector<DeviceId> devices_out_of_service(const CurrentStateSnapshot& state,
                                                           const FailureDomainId& domain);

}  // namespace cplan
