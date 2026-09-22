#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "change_planner/model/constraints.hpp"
#include "change_planner/model/evidence.hpp"
#include "change_planner/model/target.hpp"
#include "change_planner/plan/plan.hpp"

namespace cplan {

// Dimensions along which a plan can stop being valid. Invalidation is explicit:
// a plan is never silently reused against materially different state.
enum class InvalidationDimension : std::uint8_t {
  none = 0,
  authority,
  incarnation,
  epoch,
  topology,
  capability,
  target_intent,
  constraint_policy,
  planning_instant,
};

const char* to_string(InvalidationDimension dimension) noexcept;
Result<InvalidationDimension> parse_invalidation_dimension(std::string_view token);

struct InvalidationContext {
  CurrentStateSnapshot state;
  TopologyEvidence evidence;
  TargetIntent target;
  SafetyConstraints constraints;
  TimestampNs planning_instant;
};

struct InvalidationDecision {
  bool valid{false};
  InvalidationDimension dimension{InvalidationDimension::none};
  std::string detail_code;
  std::string message;
  std::vector<EntityRef> evidence;
  std::uint32_t added_entities{0};
  std::uint32_t removed_entities{0};
  std::uint32_t changed_entities{0};
  Permille capacity_drift;
  Permille demand_drift;
  Permille topology_drift;
  ChangeTolerance tolerance;

  [[nodiscard]] std::string explain() const;
};

// Evaluates whether a plan may still be executed against the supplied context
// under the tolerances declared in the governing constraints.
[[nodiscard]] InvalidationDecision evaluate_validity(const Plan& plan,
                                                     const InvalidationContext& context);

}  // namespace cplan
