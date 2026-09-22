#include <algorithm>
#include <string>
#include <vector>

#include "change_planner/model/evaluate.hpp"
#include "change_planner/persist/artifact.hpp"
#include "change_planner/persist/repository.hpp"
#include "change_planner/plan/invalidate.hpp"
#include "change_planner/plan/replan.hpp"
#include "change_planner/plan/verify.hpp"
#include "change_planner/version.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/test_framework.hpp"

using namespace cplan_test;

namespace {

Scenario capacity_scenario() {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 2500);
  }
  refresh_request(scenario);
  return scenario;
}

cplan::Plan plan_or_fail(const Scenario& scenario) {
  const cplan::PlanResult result = run_plan(scenario);
  return result.ok() ? result.plan() : cplan::Plan{};
}

}  // namespace

CPLAN_TEST(persist, artifact_round_trip_preserves_content_and_integrity) {
  const Scenario scenario = capacity_scenario();
  const cplan::Plan plan = plan_or_fail(scenario);
  CPLAN_REQUIRE(!plan.steps.empty());

  const std::vector<std::byte> bytes = cplan::serialize_plan_artifact(plan);
  CPLAN_CHECK(bytes.size() > cplan::kArtifactHeaderBytes);
  auto parsed = cplan::parse_plan_artifact(bytes);
  CPLAN_REQUIRE_OK(parsed);
  CPLAN_CHECK(parsed.value().integrity_verified);
  CPLAN_CHECK_EQ(parsed.value().format_version, cplan::kArtifactFormatVersion);
  CPLAN_CHECK_EQ(parsed.value().schema_version, cplan::kPlanSchemaVersion);
  CPLAN_CHECK_EQ(parsed.value().plan.content_digest(), plan.content_digest());
  CPLAN_CHECK(parsed.value().plan == plan);

  // Round trip through the filesystem, atomically.
  const std::string directory = make_scratch_directory("artifact");
  const std::string path = directory + "/plan.cplan";
  CPLAN_CHECK_OK(cplan::write_plan_artifact(path, plan));
  auto loaded = cplan::read_plan_artifact(path);
  CPLAN_REQUIRE_OK(loaded);
  CPLAN_CHECK_EQ(loaded.value().plan.content_digest(), plan.content_digest());
  CPLAN_CHECK_EQ(loaded.value().source_path, path);
  // No temporary files survive a successful publication.
  auto scan = cplan::scan_plan_directory(directory);
  CPLAN_REQUIRE_OK(scan);
  CPLAN_CHECK_EQ(scan.value().readable_count, static_cast<std::size_t>(1));
  CPLAN_CHECK_EQ(scan.value().unreadable_count, static_cast<std::size_t>(0));
  remove_scratch_directory(directory);
}

CPLAN_TEST(persist, artifact_reader_rejects_corruption_truncation_and_versions) {
  const Scenario scenario = capacity_scenario();
  const cplan::Plan plan = plan_or_fail(scenario);
  const std::vector<std::byte> bytes = cplan::serialize_plan_artifact(plan);

  {  // Bit flip in the payload
    std::vector<std::byte> corrupted = bytes;
    corrupted[corrupted.size() - 1] ^= std::byte{0x01};
    CPLAN_CHECK_ERROR(cplan::parse_plan_artifact(corrupted), cplan::ErrorCode::integrity_failure);
  }
  {  // Bit flip in the header
    std::vector<std::byte> corrupted = bytes;
    corrupted[20] ^= std::byte{0xFF};
    CPLAN_CHECK_ERROR(cplan::parse_plan_artifact(corrupted),
                      cplan::ErrorCode::unsupported_version);
  }
  {  // Truncation
    std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + (bytes.size() / 2));
    CPLAN_CHECK_ERROR(cplan::parse_plan_artifact(truncated), cplan::ErrorCode::truncated);
  }
  {  // Trailing bytes
    std::vector<std::byte> extended = bytes;
    extended.push_back(std::byte{0x00});
    CPLAN_CHECK_ERROR(cplan::parse_plan_artifact(extended), cplan::ErrorCode::truncated);
  }
  {  // Wrong magic
    std::vector<std::byte> wrong_magic = bytes;
    wrong_magic[0] = std::byte{'X'};
    CPLAN_CHECK_ERROR(cplan::parse_plan_artifact(wrong_magic), cplan::ErrorCode::malformed_input);
  }
  {  // Too short to contain an envelope at all
    std::vector<std::byte> tiny(bytes.begin(), bytes.begin() + 8);
    CPLAN_CHECK_ERROR(cplan::parse_plan_artifact(tiny), cplan::ErrorCode::truncated);
  }
  {  // Size limit
    cplan::ArtifactLimits limits;
    limits.max_bytes = 64;
    CPLAN_CHECK_ERROR(cplan::parse_plan_artifact(bytes, limits), cplan::ErrorCode::size_limit);
  }
  {  // Payload that decodes but is not the canonical encoding of the plan
    cplan::CanonicalEncoder encoder;
    plan.encode(encoder);
    std::vector<std::byte> payload = encoder.data();
    payload.push_back(std::byte{0x00});  // extra trailing byte inside the payload
    std::vector<std::byte> crafted;
    const std::vector<std::byte> base = cplan::serialize_plan_artifact(plan);
    crafted.insert(crafted.end(), base.begin(), base.begin() + cplan::kArtifactHeaderBytes - 32);
    // recompute the length and digest for the crafted payload
    const std::uint64_t length = static_cast<std::uint64_t>(payload.size());
    crafted[24] = static_cast<std::byte>(length & 0xFF);
    crafted[25] = static_cast<std::byte>((length >> 8) & 0xFF);
    crafted[26] = static_cast<std::byte>((length >> 16) & 0xFF);
    crafted[27] = static_cast<std::byte>((length >> 24) & 0xFF);
    crafted[28] = crafted[29] = crafted[30] = crafted[31] = std::byte{0};
    cplan::Sha256 hasher;
    hasher.update(crafted.data(), crafted.size());
    hasher.update(payload.data(), payload.size());
    const cplan::Digest digest = hasher.finalize();
    for (std::size_t i = 0; i < cplan::Digest::kSize; ++i) {
      crafted.push_back(static_cast<std::byte>(digest.data()[i]));
    }
    crafted.insert(crafted.end(), payload.begin(), payload.end());
    CPLAN_CHECK_ERROR(cplan::parse_plan_artifact(crafted), cplan::ErrorCode::malformed_input);
  }
}

CPLAN_TEST(persist, scan_reports_unreadable_files_instead_of_skipping_them) {
  const Scenario scenario = capacity_scenario();
  const cplan::Plan plan = plan_or_fail(scenario);
  const std::string directory = make_scratch_directory("scan");
  CPLAN_CHECK_OK(cplan::write_plan_artifact(directory + "/good.cplan", plan));
  write_file_bytes(directory + "/garbage.cplan", {std::byte{'n'}, std::byte{'o'}, std::byte{'p'}});
  write_file_bytes(directory + "/plan.cplan.partial-7", {std::byte{'i'}, std::byte{'n'}});

  auto scan = cplan::scan_plan_directory(directory);
  CPLAN_REQUIRE_OK(scan);
  CPLAN_CHECK_EQ(scan.value().readable_count, static_cast<std::size_t>(1));
  // The garbage file and the orphaned partial publication are both reported.
  CPLAN_CHECK_EQ(scan.value().unreadable_count, static_cast<std::size_t>(2));
  bool orphan_reported = false;
  for (const cplan::ArtifactScanEntry& entry : scan.value().entries) {
    if (!entry.readable) {
      CPLAN_CHECK(!entry.error.empty());
      if (entry.error.find("incomplete publication") != std::string::npos) {
        orphan_reported = true;
      }
    }
  }
  CPLAN_CHECK(orphan_reported);
  remove_scratch_directory(directory);
}

CPLAN_TEST(persist, repository_is_bounded_and_rejects_conflicting_republication) {
  const Scenario scenario = capacity_scenario();
  cplan::Plan plan = plan_or_fail(scenario);
  CPLAN_REQUIRE(!plan.steps.empty());

  cplan::PlanRepository repository(2);
  CPLAN_CHECK_OK(repository.publish(plan));
  CPLAN_CHECK_OK(repository.publish(plan));  // idempotent
  CPLAN_CHECK_EQ(repository.size(), static_cast<std::size_t>(1));
  CPLAN_CHECK_EQ(repository.rejections(), static_cast<std::uint64_t>(0));

  cplan::Plan conflicting = plan;
  conflicting.label = "different content";
  CPLAN_CHECK_ERROR(repository.publish(conflicting), cplan::ErrorCode::conflict);
  CPLAN_CHECK_EQ(repository.rejections(), static_cast<std::uint64_t>(1));

  cplan::Plan newer = plan;
  newer.generation = cplan::PlanGeneration(plan.generation.value() + 1);
  CPLAN_CHECK_OK(repository.publish(newer));
  cplan::Plan newest = plan;
  newest.generation = cplan::PlanGeneration(plan.generation.value() + 2);
  CPLAN_CHECK_OK(repository.publish(newest));
  CPLAN_CHECK_EQ(repository.size(), static_cast<std::size_t>(2));
  CPLAN_CHECK_EQ(repository.evictions(), static_cast<std::uint64_t>(1));

  auto missing = repository.find(plan.id, plan.generation);
  CPLAN_CHECK_ERROR(missing, cplan::ErrorCode::not_found);
  auto present = repository.find(plan.id, newest.generation);
  CPLAN_REQUIRE_OK(present);
  CPLAN_CHECK_EQ(present.value().plan.generation.value(), newest.generation.value());

  CPLAN_CHECK_EQ(repository.retire_below(cplan::PlanGeneration(newest.generation.value())),
                 static_cast<std::size_t>(1));
  CPLAN_CHECK_EQ(repository.size(), static_cast<std::size_t>(1));
}

CPLAN_TEST(persist, repository_refuses_structurally_invalid_plans) {
  cplan::PlanRepository repository(4);
  cplan::Plan broken;
  broken.id = cplan::PlanId::from_validated("plan-broken");
  broken.generation = cplan::PlanGeneration(1);
  CPLAN_CHECK_ERROR(repository.publish(broken), cplan::ErrorCode::invalid_argument);
  CPLAN_CHECK_EQ(repository.size(), static_cast<std::size_t>(0));
}

CPLAN_TEST(invalidation, unchanged_inputs_keep_a_plan_valid) {
  const Scenario scenario = capacity_scenario();
  const cplan::Plan plan = plan_or_fail(scenario);
  const cplan::InvalidationContext context{scenario.request.state, scenario.request.evidence,
                                           scenario.request.target, scenario.request.constraints,
                                           scenario.request.planning_instant};
  const cplan::InvalidationDecision decision = cplan::evaluate_validity(plan, context);
  CPLAN_CHECK(decision.valid);
  CPLAN_CHECK(decision.dimension == cplan::InvalidationDimension::none);
  CPLAN_CHECK(!decision.explain().empty());
}

CPLAN_TEST(invalidation, material_changes_invalidate_with_a_named_dimension) {
  const Scenario scenario = capacity_scenario();
  const cplan::Plan plan = plan_or_fail(scenario);

  {  // Authority generation advanced
    cplan::InvalidationContext context{scenario.request.state, scenario.request.evidence,
                                       scenario.request.target, scenario.request.constraints,
                                       scenario.request.planning_instant};
    context.state.authority_generation = cplan::AuthorityGeneration(
        scenario.request.state.authority_generation.value() + 1);
    const cplan::InvalidationDecision decision = cplan::evaluate_validity(plan, context);
    CPLAN_CHECK(!decision.valid);
    CPLAN_CHECK(decision.dimension == cplan::InvalidationDimension::authority);
  }
  {  // New boot incarnation
    cplan::InvalidationContext context{scenario.request.state, scenario.request.evidence,
                                       scenario.request.target, scenario.request.constraints,
                                       scenario.request.planning_instant};
    context.state.boot_incarnation =
        cplan::BootIncarnation(scenario.request.state.boot_incarnation.value() + 1);
    const cplan::InvalidationDecision decision = cplan::evaluate_validity(plan, context);
    CPLAN_CHECK(!decision.valid);
    CPLAN_CHECK(decision.dimension == cplan::InvalidationDimension::incarnation);
  }
  {  // Topology changed beyond tolerance
    cplan::InvalidationContext context{scenario.request.state, scenario.request.evidence,
                                       scenario.request.target, scenario.request.constraints,
                                       scenario.request.planning_instant};
    context.state.devices.pop_back();
    const cplan::InvalidationDecision decision = cplan::evaluate_validity(plan, context);
    CPLAN_CHECK(!decision.valid);
    CPLAN_CHECK(decision.dimension == cplan::InvalidationDimension::topology);
    CPLAN_CHECK(decision.removed_entities >= 1);
  }
  {  // Governing policy changed
    cplan::InvalidationContext context{scenario.request.state, scenario.request.evidence,
                                       scenario.request.target, scenario.request.constraints,
                                       scenario.request.planning_instant};
    context.constraints.governing_policy = cplan::PolicyId::from_validated("other-policy");
    const cplan::InvalidationDecision decision = cplan::evaluate_validity(plan, context);
    CPLAN_CHECK(!decision.valid);
    CPLAN_CHECK(decision.dimension == cplan::InvalidationDimension::constraint_policy);
  }
  {  // Target intent advanced
    cplan::InvalidationContext context{scenario.request.state, scenario.request.evidence,
                                       scenario.request.target, scenario.request.constraints,
                                       scenario.request.planning_instant};
    context.target.revision = cplan::TargetRevision(context.target.revision.value() + 1);
    context.target.intent_code = "changed-intent";
    const cplan::InvalidationDecision decision = cplan::evaluate_validity(plan, context);
    CPLAN_CHECK(!decision.valid);
    CPLAN_CHECK(decision.dimension == cplan::InvalidationDimension::target_intent);
  }
  {  // Capability version grew without declared tolerance
    cplan::InvalidationContext context{scenario.request.state, scenario.request.evidence,
                                       scenario.request.target, scenario.request.constraints,
                                       scenario.request.planning_instant};
    context.evidence.capability_version =
        cplan::CapabilityVersion(plan.binding.capability_version.value() + 1);
    const cplan::InvalidationDecision decision = cplan::evaluate_validity(plan, context);
    CPLAN_CHECK(!decision.valid);
    CPLAN_CHECK(decision.dimension == cplan::InvalidationDimension::capability);
  }
}

CPLAN_TEST(invalidation, declared_tolerance_absorbs_minor_drift) {
  Scenario scenario = capacity_scenario();
  // Tolerances are declared up front: they are part of the governing policy and
  // therefore part of the plan binding.
  scenario.request.constraints.tolerance.capacity_drift = cplan::Permille(500);
  scenario.request.constraints.tolerance.demand_drift = cplan::Permille(500);
  scenario.request.constraints.tolerance.max_added_entities = 2;
  scenario.request.constraints.tolerance.max_removed_entities = 2;
  const cplan::Plan plan = plan_or_fail(scenario);

  cplan::InvalidationContext context{scenario.request.state, scenario.request.evidence,
                                     scenario.request.target, scenario.request.constraints,
                                     scenario.request.planning_instant};
  // A capacity change of one link is within the declared drift.
  for (cplan::Link& link : context.state.links) {
    if (link.id.str() == "l-a1") {
      link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 500);
    }
  }
  const cplan::InvalidationDecision decision = cplan::evaluate_validity(plan, context);
  CPLAN_CHECK(decision.valid);
  CPLAN_CHECK(decision.changed_entities >= 1);
}

CPLAN_TEST(replanning, refusal_is_fenced_for_stale_observations) {
  const Scenario scenario = capacity_scenario();
  const cplan::Plan plan = plan_or_fail(scenario);

  cplan::ObservedExecution observed;
  observed.plan_id = plan.id;
  observed.plan_generation = cplan::PlanGeneration(plan.generation.value() + 5);  // stale claim
  observed.attempt = cplan::AttemptNumber(2);
  observed.authority_generation = scenario.request.state.authority_generation;
  observed.observed_generation = scenario.request.state.topology_generation;
  observed.boot_incarnation = scenario.request.state.boot_incarnation;
  observed.observed_epoch = scenario.request.state.epoch;
  observed.observed_state = scenario.request.state;

  const cplan::PlanResult result =
      cplan::replan(plan, observed, scenario.request, cplan::ReplanningOptions{});
  CPLAN_REQUIRE(!result.ok());
  CPLAN_CHECK(result.refusal().code == cplan::RefusalCode::stale_input);
  CPLAN_CHECK(!result.refusal().explain().empty());
}

CPLAN_TEST(replanning, continuation_reissues_only_unobserved_work) {
  const Scenario scenario = capacity_scenario();
  const cplan::Plan plan = plan_or_fail(scenario);
  CPLAN_REQUIRE(plan.steps.size() >= 2);

  // Mark the first step as applied in the observed state.
  cplan::ObservedExecution observed;
  observed.plan_id = plan.id;
  observed.plan_generation = plan.generation;
  observed.attempt = cplan::AttemptNumber(1);
  observed.authority_generation = scenario.request.state.authority_generation;
  observed.observed_generation = cplan::TopologyGeneration(
      scenario.request.state.topology_generation.value() + 1);
  observed.boot_incarnation = scenario.request.state.boot_incarnation;
  observed.observed_epoch = scenario.request.state.epoch;

  const cplan::Step& first = plan.steps.front();
  cplan::CurrentStateSnapshot observed_state = scenario.request.state;
  {
    const cplan::EvaluationContext context{scenario.request.planning_instant,
                                           &scenario.request.constraints,
                                           &scenario.request.evidence};
    auto applied = cplan::apply_operation(observed_state, first.operation, context);
    CPLAN_REQUIRE_OK(applied);
    observed_state = applied.value().state;
  }
  observed_state.topology_generation = observed.observed_generation;
  observed.observed_state = observed_state;
  observed.observations.push_back(cplan::StepObservation{
      first.id, cplan::AttemptNumber(1), cplan::StepOutcome::succeeded, "applied"});

  const cplan::ReplanningOptions options{};
  const cplan::PlanRequest continuation_request =
      cplan::replan_request_for(plan, observed, scenario.request, options);
  const cplan::PlanResult result = cplan::replan(plan, observed, scenario.request, options);
  CPLAN_REQUIRE(result.ok());
  const cplan::Plan& continuation = result.plan();
  CPLAN_CHECK(continuation.lineage.kind == cplan::LineageKind::replan);
  CPLAN_CHECK_EQ(continuation.lineage.parent_plan, plan.id);
  CPLAN_CHECK_EQ(continuation.lineage.parent_generation.value(), plan.generation.value());
  CPLAN_CHECK_EQ(continuation.generation.value(), plan.generation.value() + 1);
  CPLAN_CHECK(continuation.find_step(first.id) == nullptr);
  CPLAN_CHECK_EQ(continuation.binding.topology_generation.value(),
                 observed.observed_generation.value());
  CPLAN_CHECK(cplan::verify_plan(continuation, continuation_request, true).ok);
}

CPLAN_TEST(replanning, rollback_plan_returns_to_the_original_state) {
  const Scenario scenario = capacity_scenario();
  const cplan::Plan plan = plan_or_fail(scenario);
  CPLAN_REQUIRE(!plan.steps.empty());

  // Pretend the whole plan was executed.
  cplan::ObservedExecution observed;
  observed.plan_id = plan.id;
  observed.plan_generation = plan.generation;
  observed.attempt = cplan::AttemptNumber(1);
  observed.authority_generation = scenario.request.state.authority_generation;
  observed.observed_generation = cplan::TopologyGeneration(
      scenario.request.state.topology_generation.value() + 1);
  observed.boot_incarnation = scenario.request.state.boot_incarnation;
  observed.observed_epoch = scenario.request.state.epoch;
  auto executed = apply_plan(plan, scenario.request);
  CPLAN_REQUIRE_OK(executed);
  cplan::CurrentStateSnapshot executed_state = executed.value();
  executed_state.topology_generation = observed.observed_generation;
  observed.observed_state = executed_state;

  const cplan::ReplanningOptions options{};
  const cplan::PlanRequest rollback_request =
      cplan::rollback_request_for(plan, observed, scenario.request, options);
  const cplan::PlanResult result =
      cplan::build_rollback_plan(plan, observed, scenario.request, options);
  CPLAN_REQUIRE(result.ok());
  CPLAN_CHECK(result.plan().lineage.kind == cplan::LineageKind::rollback);
  CPLAN_CHECK(cplan::verify_plan(result.plan(), rollback_request, true).ok);
  auto rolled_back = apply_plan(result.plan(), rollback_request);
  CPLAN_REQUIRE_OK(rolled_back);
  for (const cplan::Link& link : rolled_back.value().links) {
    CPLAN_CHECK_EQ(link.spec.capacity.value(), 10000u);
  }
}
