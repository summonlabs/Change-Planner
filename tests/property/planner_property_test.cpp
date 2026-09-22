#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "change_planner/model/evaluate.hpp"
#include "change_planner/persist/artifact.hpp"
#include "change_planner/plan/invalidate.hpp"
#include "change_planner/plan/replan.hpp"
#include "change_planner/plan/verify.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/test_framework.hpp"

using namespace cplan_test;

namespace {

cplan::EvaluationContext context_of(const Scenario& scenario) {
  return cplan::EvaluationContext{scenario.request.planning_instant, &scenario.request.constraints,
                                  &scenario.request.evidence};
}

// Mutates a random scenario in ways that are always expressible: capacity bumps,
// configuration values, policy binding, an extra link between existing devices
// and the removal of a device that carries no traffic.
void mutate_target(Scenario& scenario, Rng& rng) {
  for (cplan::DesiredLink& link : scenario.target.links) {
    if (rng.chance(1, 2)) {
      link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 1000);
    }
  }
  if (rng.chance(2, 3)) {
    scenario.target.configs.push_back(cplan::DesiredConfig{
        cplan::DesiredAction::ensure, scenario.state.devices.front().id,
        cplan::ConfigKey::from_validated("mtu"), "9000"});
  }
  if (rng.chance(1, 3)) {
    scenario.target.policies.push_back(cplan::DesiredPolicy{
        cplan::DesiredAction::ensure, cplan::PolicyId::from_validated("p-fabric"),
        cplan::PolicyTarget{cplan::PolicyScope::global, std::string()}, "strict", true});
  }
  if (rng.chance(1, 3) && scenario.target.links.size() >= 2) {
    // Retire the final link of the chain and the route that used it, keeping the
    // workload contract satisfiable by relaxing the diversity requirement.
    cplan::DesiredLink& last = scenario.target.links.back();
    const cplan::LinkId retired = last.id;
    last.action = cplan::DesiredAction::remove;
    for (cplan::DesiredRoute& route : scenario.target.routes) {
      const bool uses = std::find(route.spec.path.begin(), route.spec.path.end(), retired) !=
                        route.spec.path.end();
      if (uses) {
        route.action = cplan::DesiredAction::remove;
      }
    }
    for (cplan::DesiredWorkload& workload : scenario.target.workloads) {
      workload.bindings.erase(
          std::remove_if(workload.bindings.begin(), workload.bindings.end(),
                         [&](const cplan::RouteBinding& binding) {
                           const auto route = std::find_if(
                               scenario.target.routes.begin(), scenario.target.routes.end(),
                               [&](const cplan::DesiredRoute& candidate) {
                                 return candidate.id == binding.route;
                               });
                           return route != scenario.target.routes.end() &&
                                  route->action == cplan::DesiredAction::remove;
                         }),
          workload.bindings.end());
      workload.requirements.min_link_disjoint_paths = 1;
      workload.requirements.min_disjoint_domains = 0;
    }
  }
  refresh_request(scenario);
}

// Every blocking constraint named in a refusal must exist in the governing
// constraint set: a refusal explains itself with real declared obligations.
bool names_a_declared_constraint(const Scenario& scenario,
                                 const cplan::BlockingConstraint& blocking) {
  for (const cplan::ConstraintItem& item : scenario.request.constraints.items) {
    if (item.id == blocking.id && item.kind == blocking.kind) {
      return true;
    }
  }
  return false;
}

}  // namespace

CPLAN_TEST(property, planner_always_returns_a_verified_plan_or_a_reasoned_refusal) {
  std::uint32_t planned = 0;
  std::uint32_t refused = 0;
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    Rng rng(seed * 7919);
    Scenario scenario = make_random(TopologyOptions{}, rng,
                                    2 + rng.next_bounded(6),   // devices
                                    1 + rng.next_bounded(3),   // domains
                                    rng.next_bounded(4));      // extra links
    mutate_target(scenario, rng);

    const cplan::PlanResult result = run_plan(scenario);
    if (result.ok()) {
      ++planned;
      const cplan::Plan& plan = result.plan();
      CPLAN_CHECK(plan.verify_structure().ok());

      // Exhaustive verification: every interleaving of every stage.
      const cplan::VerificationReport report = cplan::verify_plan(plan, scenario.request, true);
      CPLAN_CHECK(report.ok);

      // Independent prefix simulation: every prefix in canonical order keeps
      // every invariant.
      cplan::CurrentStateSnapshot state = scenario.request.state;
      const cplan::EvaluationContext context = context_of(scenario);
      for (const cplan::Stage& stage : plan.stages) {
        std::vector<cplan::StepId> ids = stage.steps;
        std::sort(ids.begin(), ids.end());
        for (const cplan::StepId& id : ids) {
          const cplan::Step* step = plan.find_step(id);
          CPLAN_REQUIRE(step != nullptr);
          for (const cplan::Condition& condition : step->preconditions) {
            auto satisfied = cplan::evaluate_condition(state, condition, context);
            CPLAN_REQUIRE_OK(satisfied);
            CPLAN_CHECK(satisfied.value());
          }
          auto applied = cplan::apply_operation(state, step->operation, context);
          CPLAN_REQUIRE_OK(applied);
          state = applied.value().state;
          CPLAN_CHECK_EQ(hard_violation_count(cplan::check_invariants(state, context)),
                         static_cast<std::size_t>(0));
        }
      }

      // Every step carries preconditions, postconditions and compensation data.
      for (const cplan::Step& step : plan.steps) {
        CPLAN_CHECK(!step.preconditions.empty());
        CPLAN_CHECK(!step.postconditions.empty());
        CPLAN_CHECK(!step.justification.checked_invariants.empty());
        CPLAN_CHECK(!step.justification.rationale_code.empty());
      }

      // The artifact round trip preserves the plan exactly.
      const std::vector<std::byte> bytes = cplan::serialize_plan_artifact(plan);
      auto parsed = cplan::parse_plan_artifact(bytes);
      CPLAN_REQUIRE_OK(parsed);
      CPLAN_CHECK_EQ(parsed.value().plan.content_digest(), plan.content_digest());
    } else {
      ++refused;
      const cplan::Refusal& refusal = result.refusal();
      CPLAN_CHECK(!refusal.explain().empty());
      CPLAN_CHECK(!refusal.summary_code.empty());
      CPLAN_CHECK(!refusal.justification.notes.empty() || !refusal.residual_violations.empty() ||
                  !refusal.blocked_steps.empty());
      // A refusal never fabricates a plan.
      CPLAN_CHECK(!result.outcome.valueless_by_exception());
      for (const cplan::BlockingConstraint& blocking : refusal.blocking) {
        CPLAN_CHECK(!blocking.id.empty());
        CPLAN_CHECK(!blocking.message.empty());
        CPLAN_CHECK(names_a_declared_constraint(scenario, blocking));
      }
    }
  }
  CPLAN_CHECK_GT(planned + refused, 0u);
  CPLAN_CHECK_EQ(planned + refused, 40u);
}

CPLAN_TEST(property, identical_inputs_yield_identical_plans) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    Rng rng(seed * 104729);
    Scenario scenario = make_random(TopologyOptions{}, rng, 3 + rng.next_bounded(5),
                                    1 + rng.next_bounded(2), rng.next_bounded(3));
    mutate_target(scenario, rng);
    const cplan::PlanResult first = run_plan(scenario);
    const cplan::PlanResult second = run_plan(scenario);
    CPLAN_CHECK_EQ(first.ok(), second.ok());
    if (first.ok()) {
      CPLAN_CHECK_EQ(first.plan().content_digest(), second.plan().content_digest());
      CPLAN_CHECK_EQ(first.plan().verification_digest, second.plan().verification_digest);
      CPLAN_CHECK(first.plan().objective == second.plan().objective);
    } else {
      CPLAN_CHECK(first.refusal().code == second.refusal().code);
      CPLAN_CHECK_EQ(first.refusal().summary_code, second.refusal().summary_code);
    }

    // Reordering the input collections must not change the outcome: canonical
    // form is enforced before planning.
    Scenario shuffled = scenario;
    std::reverse(shuffled.request.state.devices.begin(), shuffled.request.state.devices.end());
    std::reverse(shuffled.request.state.links.begin(), shuffled.request.state.links.end());
    std::reverse(shuffled.request.target.links.begin(), shuffled.request.target.links.end());
    shuffled.request.state.canonicalize();
    shuffled.request.target.canonicalize();
    const cplan::PlanResult third = cplan::generate_plan(shuffled.request);
    CPLAN_CHECK_EQ(third.ok(), first.ok());
    if (first.ok() && third.ok()) {
      CPLAN_CHECK_EQ(first.plan().content_digest(), third.plan().content_digest());
    }
  }
}

CPLAN_TEST(property, stages_never_contain_dependent_or_conflicting_steps) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    Rng rng(seed * 15485863);
    Scenario scenario = make_random(TopologyOptions{}, rng, 4 + rng.next_bounded(6),
                                    1 + rng.next_bounded(3), 1 + rng.next_bounded(4));
    mutate_target(scenario, rng);
    const cplan::PlanResult result = run_plan(scenario);
    if (!result.ok()) {
      continue;
    }
    const cplan::Plan& plan = result.plan();
    for (const cplan::Stage& stage : plan.stages) {
      for (std::size_t i = 0; i < stage.steps.size(); ++i) {
        for (std::size_t j = i + 1; j < stage.steps.size(); ++j) {
          for (const cplan::DependencyEdge& edge : plan.edges) {
            const bool forward = edge.from == stage.steps[i] && edge.to == stage.steps[j];
            const bool backward = edge.from == stage.steps[j] && edge.to == stage.steps[i];
            CPLAN_CHECK(!forward && !backward);
          }
        }
      }
      CPLAN_CHECK(stage.steps.size() <= scenario.request.limits.max_stage_width);
    }
  }
}

CPLAN_TEST(property, every_partial_execution_can_be_replanned) {
  std::uint32_t replanned = 0;
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    Rng rng(seed * 2654435761u);
    Scenario scenario = make_random(TopologyOptions{}, rng, 4 + rng.next_bounded(3), 2,
                                    rng.next_bounded(2));
    mutate_target(scenario, rng);
    const cplan::PlanResult result = run_plan(scenario);
    if (!result.ok()) {
      continue;
    }
    const cplan::Plan& plan = result.plan();
    const cplan::EvaluationContext context = context_of(scenario);

    // Execute a prefix, then replan from that observed state.
    cplan::CurrentStateSnapshot observed_state = scenario.request.state;
    std::vector<cplan::StepObservation> observations;
    std::vector<cplan::StepId> executed;
    for (const cplan::Step& step : plan.steps) {
      if (executed.size() >= plan.steps.size() / 2) {
        break;
      }
      auto applied = cplan::apply_operation(observed_state, step.operation, context);
      if (!applied.ok()) {
        break;
      }
      observed_state = applied.value().state;
      executed.push_back(step.id);
      observations.push_back(cplan::StepObservation{
          step.id, cplan::AttemptNumber(1),
          executed.size() % 3 == 0 ? cplan::StepOutcome::unknown : cplan::StepOutcome::succeeded,
          "simulated"});
    }
    observed_state.topology_generation = cplan::TopologyGeneration(
        scenario.request.state.topology_generation.value() + 1);

    cplan::ObservedExecution observed;
    observed.plan_id = plan.id;
    observed.plan_generation = plan.generation;
    observed.attempt = cplan::AttemptNumber(1);
    observed.authority_generation = scenario.request.state.authority_generation;
    observed.observed_generation = observed_state.topology_generation;
    observed.boot_incarnation = scenario.request.state.boot_incarnation;
    observed.observed_epoch = scenario.request.state.epoch;
    observed.observed_state = observed_state;
    observed.observations = observations;
    CPLAN_REQUIRE_OK(observed.validate());

    const cplan::ReplanningOptions options{};
    const cplan::PlanRequest continuation_request =
        cplan::replan_request_for(plan, observed, scenario.request, options);
    const cplan::PlanResult continuation = cplan::replan(plan, observed, scenario.request, options);
    CPLAN_REQUIRE(continuation.ok());
    CPLAN_CHECK(continuation.plan().lineage.kind == cplan::LineageKind::replan);
    CPLAN_CHECK(cplan::verify_plan(continuation.plan(), continuation_request, true).ok);

    // Executing the continuation reaches the target intent. The final state is
    // evaluated under the constraint and evidence generation the continuation was
    // bound to, not the pre-execution context.
    auto finished = apply_plan(continuation.plan(), continuation_request);
    CPLAN_REQUIRE_OK(finished);
    const cplan::EvaluationContext continuation_context{
        continuation_request.planning_instant, &continuation_request.constraints,
        &continuation_request.evidence};
    CPLAN_CHECK_EQ(
        hard_violation_count(cplan::check_invariants(finished.value(), continuation_context)),
        static_cast<std::size_t>(0));
    for (const cplan::DesiredLink& desired : scenario.target.links) {
      if (desired.action != cplan::DesiredAction::ensure) {
        continue;
      }
      const cplan::Link* link = finished.value().find_link(desired.id);
      CPLAN_REQUIRE(link != nullptr);
      CPLAN_CHECK_EQ(link->spec.capacity.value(), desired.spec.capacity.value());
    }
    ++replanned;
  }
  CPLAN_CHECK_GT(replanned, 0u);
}

CPLAN_TEST(property, stale_generations_are_always_fenced) {
  Rng rng(4242);
  Scenario scenario = make_random(TopologyOptions{}, rng, 4, 2, 1);
  mutate_target(scenario, rng);

  {
    Scenario stale = scenario;
    stale.request.target.based_on_generation =
        cplan::TopologyGeneration(stale.request.state.topology_generation.value() + 1);
    const cplan::PlanResult result = run_plan(stale);
    CPLAN_REQUIRE(!result.ok());
    CPLAN_CHECK(result.refusal().code == cplan::RefusalCode::stale_input);
  }
  {
    Scenario stale = scenario;
    stale.request.target.authority_generation = cplan::AuthorityGeneration(
        stale.request.state.authority_generation.value() + 1);
    CPLAN_CHECK(!run_plan(stale).ok());
  }
  {
    Scenario stale = scenario;
    stale.request.target.based_on_epoch =
        cplan::Epoch(stale.request.state.epoch.value() + 1);
    CPLAN_CHECK(!run_plan(stale).ok());
  }
  {
    Scenario stale = scenario;
    stale.request.evidence.boot_incarnation =
        cplan::BootIncarnation(stale.request.state.boot_incarnation.value() + 1);
    CPLAN_CHECK(!run_plan(stale).ok());
  }
  {
    Scenario stale = scenario;
    stale.request.state.authority = cplan::AuthorityId::from_validated("other-authority");
    CPLAN_CHECK(!run_plan(stale).ok());
  }
}

CPLAN_TEST(property, cancellation_never_publishes_a_plan) {
  Rng rng(99);
  Scenario scenario = make_random(TopologyOptions{}, rng, 6, 2, 2);
  mutate_target(scenario, rng);

  cplan::StopSource source;
  source.request_stop();
  const cplan::PlanResult result = cplan::generate_plan(scenario.request, source.token());
  CPLAN_REQUIRE(!result.ok());
  CPLAN_CHECK(result.refusal().code == cplan::RefusalCode::cancelled);
  CPLAN_CHECK(result.refusal().explain().find("cancelled") != std::string::npos);
}
