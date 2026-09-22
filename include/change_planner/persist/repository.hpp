#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "change_planner/persist/artifact.hpp"

namespace cplan {

// Bounded, thread-safe, integrity-checked store of generated plans.
//
// Concurrency audit: exactly one mutex guards the slot map; no callback, no
// event emission and no user code runs while it is held; no nested lock is ever
// acquired; every accessor returns copies. Serialisation happens before the lock
// is taken, so a long encode never blocks readers.
class PlanRepository {
 public:
  explicit PlanRepository(std::size_t capacity = 64);

  // Publishes a plan. Re-publishing identical content is idempotent; publishing
  // different content for an existing (plan id, generation) is rejected.
  Result<void> publish(const Plan& plan);

  [[nodiscard]] Result<PlanArtifact> find(const PlanId& id, PlanGeneration generation) const;

  [[nodiscard]] std::vector<PlanArtifact> list() const;

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t capacity() const;
  [[nodiscard]] std::uint64_t evictions() const;
  [[nodiscard]] std::uint64_t rejections() const;

  // Removes every slot whose plan generation is strictly below the bound. Used
  // when an authority generation advances and older plans can no longer apply.
  std::size_t retire_below(PlanGeneration generation);

 private:
  mutable std::mutex mutex_;
  std::map<std::pair<PlanId, PlanGeneration>, PlanArtifact> slots_;
  std::size_t capacity_;
  std::uint64_t evictions_{0};
  std::uint64_t rejections_{0};
};

}  // namespace cplan
