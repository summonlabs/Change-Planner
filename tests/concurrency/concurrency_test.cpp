#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "change_planner/model/evaluate.hpp"
#include "change_planner/persist/artifact.hpp"
#include "change_planner/persist/repository.hpp"
#include "change_planner/plan/verify.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/test_framework.hpp"

using namespace cplan_test;

namespace {

Scenario parallel_scenario() {
  Scenario scenario = make_ladder(TopologyOptions{});
  for (cplan::DesiredLink& link : scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 500);
  }
  refresh_request(scenario);
  return scenario;
}

}  // namespace

CPLAN_TEST(concurrency, repository_supports_concurrent_publish_and_read) {
  const Scenario scenario = parallel_scenario();
  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());
  const cplan::Plan prototype = result.plan();

  cplan::PlanRepository repository(32);
  constexpr std::uint32_t kThreads = 8;
  constexpr std::uint32_t kIterations = 60;
  std::atomic<std::uint32_t> published{0};
  std::atomic<std::uint32_t> read_back{0};
  std::atomic<std::uint32_t> not_found{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::uint32_t worker = 0; worker < kThreads; ++worker) {
    workers.emplace_back([&repository, &prototype, worker, &published, &read_back, &not_found]() {
      for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        cplan::Plan plan = prototype;
        plan.generation = cplan::PlanGeneration((worker * kIterations) + iteration + 1);
        const cplan::Status status = repository.publish(plan);
        if (status.ok()) {
          published.fetch_add(1, std::memory_order_relaxed);
        }
        auto found = repository.find(plan.id, plan.generation);
        if (found.ok()) {
          if (found.value().plan.id == plan.id && found.value().plan.generation == plan.generation &&
              found.value().integrity_verified) {
            read_back.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          not_found.fetch_add(1, std::memory_order_relaxed);
        }
        const std::vector<cplan::PlanArtifact> listing = repository.list();
        for (const cplan::PlanArtifact& artifact : listing) {
          (void)artifact.payload_digest;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  CPLAN_CHECK_GT(published.load(), 0u);
  CPLAN_CHECK_GT(read_back.load(), 0u);
  CPLAN_CHECK_EQ(read_back.load() + not_found.load(), kThreads * kIterations);
  CPLAN_CHECK_LE(repository.size(), repository.capacity());
  CPLAN_CHECK(repository.size() > 0);
}

CPLAN_TEST(concurrency, concurrent_planning_is_deterministic) {
  const Scenario first_scenario = parallel_scenario();
  Scenario second_scenario = parallel_scenario();
  for (cplan::DesiredLink& link : second_scenario.target.links) {
    link.spec.capacity = cplan::CapacityUnits(link.spec.capacity.value() + 2500);
  }
  refresh_request(second_scenario);

  const cplan::PlanResult reference_first = run_plan(first_scenario);
  const cplan::PlanResult reference_second = run_plan(second_scenario);
  CPLAN_REQUIRE(reference_first.ok());
  CPLAN_REQUIRE(reference_second.ok());

  constexpr std::uint32_t kThreads = 6;
  std::atomic<std::uint32_t> mismatches{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::uint32_t worker = 0; worker < kThreads; ++worker) {
    // The index is captured by value: a by-reference capture would race with the
    // loop that spawns the threads.
    workers.emplace_back([&, worker]() {
      for (std::uint32_t iteration = 0; iteration < 6; ++iteration) {
        const cplan::PlanResult result =
            (worker % 2 == 0) ? run_plan(first_scenario) : run_plan(second_scenario);
        const cplan::Digest expected = (worker % 2 == 0) ? reference_first.plan().content_digest()
                                                         : reference_second.plan().content_digest();
        if (!result.ok() || !(result.plan().content_digest() == expected)) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  CPLAN_CHECK_EQ(mismatches.load(), 0u);
}

CPLAN_TEST(concurrency, cancellation_races_never_publish_partial_plans) {
  const Scenario scenario = parallel_scenario();
  const cplan::PlanResult reference = run_plan(scenario);
  CPLAN_REQUIRE(reference.ok());

  std::atomic<std::uint32_t> plans{0};
  std::atomic<std::uint32_t> cancellations{0};
  std::atomic<std::uint32_t> other{0};

  constexpr std::uint32_t kThreads = 4;
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::uint32_t worker = 0; worker < kThreads; ++worker) {
    workers.emplace_back([&scenario, &reference, worker, &plans, &cancellations, &other]() {
      for (std::uint32_t iteration = 0; iteration < 8; ++iteration) {
        cplan::StopSource source;
        if ((worker + iteration) % 3 == 0) {
          source.request_stop();
        }
        const cplan::PlanResult result =
            cplan::generate_plan(scenario.request, source.token());
        if (result.ok()) {
          // A cancelled run must never publish a plan; if one is produced it must
          // be the deterministic plan for these inputs.
          if (result.plan().content_digest() == reference.plan().content_digest()) {
            plans.fetch_add(1, std::memory_order_relaxed);
          } else {
            other.fetch_add(1, std::memory_order_relaxed);
          }
          continue;
        }
        if (result.refusal().code == cplan::RefusalCode::cancelled) {
          cancellations.fetch_add(1, std::memory_order_relaxed);
        } else {
          other.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  CPLAN_CHECK_EQ(other.load(), 0u);
  CPLAN_CHECK_GT(plans.load(), 0u);
  CPLAN_CHECK_GT(cancellations.load(), 0u);
}

CPLAN_TEST(concurrency, repeated_lifecycle_leaves_no_residue) {
  const std::string directory = make_scratch_directory("lifecycle");
  const Scenario scenario = parallel_scenario();
  const cplan::PlanResult result = run_plan(scenario);
  CPLAN_REQUIRE(result.ok());

  for (std::uint32_t round = 0; round < 25; ++round) {
    cplan::PlanRepository repository(4);
    const std::string path = directory + "/plan-" + std::to_string(round) + ".cplan";
    CPLAN_CHECK_OK(repository.publish(result.plan()));
    CPLAN_CHECK_OK(cplan::write_plan_artifact(path, result.plan()));
    auto loaded = cplan::read_plan_artifact(path);
    CPLAN_REQUIRE_OK(loaded);
    CPLAN_CHECK_EQ(loaded.value().plan.content_digest(), result.plan().content_digest());
    CPLAN_REQUIRE_OK(cplan::verify_plan(loaded.value().plan, scenario.request, false).ok
                         ? cplan::Status::success()
                         : cplan::Status::error(cplan::ErrorCode::internal_invariant,
                                                "reloaded plan failed verification"));
    std::error_code error;
    std::filesystem::remove(path, error);
  }
  auto scan = cplan::scan_plan_directory(directory);
  CPLAN_REQUIRE_OK(scan);
  CPLAN_CHECK_EQ(scan.value().entries.size(), static_cast<std::size_t>(0));
  remove_scratch_directory(directory);
}
