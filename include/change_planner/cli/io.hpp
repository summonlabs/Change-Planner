#pragma once

#include <string>
#include <vector>

#include "change_planner/core/json.hpp"
#include "change_planner/model/constraints.hpp"
#include "change_planner/model/evidence.hpp"
#include "change_planner/model/target.hpp"
#include "change_planner/plan/compare.hpp"
#include "change_planner/plan/generate.hpp"
#include "change_planner/plan/invalidate.hpp"
#include "change_planner/plan/replan.hpp"
#include "change_planner/plan/verify.hpp"

namespace cplan::cli {

// Input documents are strictly parsed: unknown enum tokens, missing required
// members, wrong JSON kinds and out-of-range numbers are all rejected with a
// precise message rather than silently defaulted.
[[nodiscard]] Result<JsonValue> read_json_file(const std::string& path,
                                               const JsonLimits& limits = {});

[[nodiscard]] Result<CurrentStateSnapshot> parse_state_document(const JsonValue& document);
[[nodiscard]] Result<TargetIntent> parse_target_document(const JsonValue& document);
[[nodiscard]] Result<SafetyConstraints> parse_constraints_document(const JsonValue& document);
[[nodiscard]] Result<TopologyEvidence> parse_evidence_document(const JsonValue& document);
[[nodiscard]] Result<Objective> parse_objective(const JsonValue& document);
[[nodiscard]] Result<PlanningLimits> parse_limits(const JsonValue& document);
[[nodiscard]] Result<PlanRequest> parse_plan_request(const JsonValue& document);
[[nodiscard]] Result<ObservedExecution> parse_observed_execution(const JsonValue& document);

[[nodiscard]] JsonValue plan_to_json(const Plan& plan);
[[nodiscard]] JsonValue refusal_to_json(const Refusal& refusal);
[[nodiscard]] JsonValue step_explanation_to_json(const StepExplanation& explanation);
[[nodiscard]] JsonValue comparison_to_json(const PlanComparison& comparison);
[[nodiscard]] JsonValue invalidation_to_json(const InvalidationDecision& decision);
[[nodiscard]] JsonValue verification_to_json(const VerificationReport& report);
[[nodiscard]] JsonValue plan_summary_to_json(const Plan& plan);

[[nodiscard]] std::string describe_plan(const Plan& plan);
[[nodiscard]] std::string describe_invalidation(const InvalidationDecision& decision);
[[nodiscard]] std::string describe_verification(const VerificationReport& report);

}  // namespace cplan::cli
