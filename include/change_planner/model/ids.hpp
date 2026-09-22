#pragma once

#include <cstdint>

#include "change_planner/core/canonical.hpp"
#include "change_planner/core/strong.hpp"

namespace cplan {

// ---------------------------------------------------------------------------
// Strongly typed identities. Every entity kind has a distinct C++ type, so a
// LinkId can never be passed where a DeviceId is expected.
// ---------------------------------------------------------------------------

#define CPLAN_DECLARE_NAME_ID(TypeName, KindText)                 \
  struct TypeName##Tag {                                          \
    static constexpr const char* kind_name() { return KindText; } \
  };                                                              \
  using TypeName = NameId<TypeName##Tag>

CPLAN_DECLARE_NAME_ID(DeviceId, "device");
CPLAN_DECLARE_NAME_ID(LinkId, "link");
CPLAN_DECLARE_NAME_ID(RouteId, "route");
CPLAN_DECLARE_NAME_ID(WorkloadId, "workload");
CPLAN_DECLARE_NAME_ID(FailureDomainId, "failure-domain");
CPLAN_DECLARE_NAME_ID(PolicyId, "policy");
CPLAN_DECLARE_NAME_ID(ContractId, "contract");
CPLAN_DECLARE_NAME_ID(ConfigKey, "config-key");
CPLAN_DECLARE_NAME_ID(WindowId, "window");
CPLAN_DECLARE_NAME_ID(PlanId, "plan");
CPLAN_DECLARE_NAME_ID(StepId, "step");
CPLAN_DECLARE_NAME_ID(ConstraintId, "constraint");
CPLAN_DECLARE_NAME_ID(AuthorityId, "authority");

#undef CPLAN_DECLARE_NAME_ID

// ---------------------------------------------------------------------------
// Scalar identities and quantities. Generations, epochs, incarnations and
// revisions are distinct types: an authority generation cannot be assigned to a
// topology generation even though both are 64-bit counters.
// ---------------------------------------------------------------------------

#define CPLAN_DECLARE_SCALAR(TypeName, Rep, KindText)             \
  struct TypeName##Tag {                                          \
    static constexpr const char* kind_name() { return KindText; } \
  };                                                              \
  using TypeName = Scalar<TypeName##Tag, Rep>

CPLAN_DECLARE_SCALAR(TopologyGeneration, std::uint64_t, "topology-generation");
CPLAN_DECLARE_SCALAR(AuthorityGeneration, std::uint64_t, "authority-generation");
CPLAN_DECLARE_SCALAR(BootIncarnation, std::uint64_t, "boot-incarnation");
CPLAN_DECLARE_SCALAR(PlanGeneration, std::uint64_t, "plan-generation");
CPLAN_DECLARE_SCALAR(TargetRevision, std::uint64_t, "target-revision");
CPLAN_DECLARE_SCALAR(CapabilityVersion, std::uint64_t, "capability-version");
CPLAN_DECLARE_SCALAR(AttemptNumber, std::uint32_t, "attempt-number");
CPLAN_DECLARE_SCALAR(Epoch, std::uint64_t, "epoch");
CPLAN_DECLARE_SCALAR(ConfigRevision, std::uint64_t, "config-revision");
CPLAN_DECLARE_SCALAR(CapacityUnits, std::uint64_t, "capacity-units");
CPLAN_DECLARE_SCALAR(DemandUnits, std::uint64_t, "demand-units");
CPLAN_DECLARE_SCALAR(Permille, std::uint32_t, "permille");
CPLAN_DECLARE_SCALAR(DurationNs, std::uint64_t, "duration-ns");
CPLAN_DECLARE_SCALAR(TimestampNs, std::int64_t, "timestamp-ns");
CPLAN_DECLARE_SCALAR(StageIndex, std::uint32_t, "stage-index");
CPLAN_DECLARE_SCALAR(RiskScore, std::uint32_t, "risk-score");

#undef CPLAN_DECLARE_SCALAR

}  // namespace cplan
